#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arbc {

// The cache's shared eviction-ordered class vocabulary (doc 02). Declaration
// order **is** eviction order: Speculative is evicted first and Visible last,
// which is doc 02's "visible > adjacent > recently visible > speculative"
// read bottom-up. Both engines share this enum (doc 17: `cache` is
// engine-agnostic); doc 11's temporal prefetch ring is a later extension
// owned by `cache.prefetch`, and because the eviction walk visits classes in
// declaration order, appending a class is a localized change.
enum class PriorityClass {
  Speculative,
  Recent,
  Adjacent,
  Visible,
};

namespace detail {

// The four doc-02 classes in eviction order (victim-first). Kept out-of-line
// (see keyed_store.cpp) so the ordering has a single authoritative definition
// the template's eviction walk reads.
constexpr std::size_t k_priority_class_count = 4;
const std::array<PriorityClass, k_priority_class_count>& cache_eviction_order();

// A stored entry. Independent of Key so CacheHold need not name the store's
// Key: byte cost, priority class, LRU recency tick, the store-internal
// residency pin count, and a `removed` flag marking an entry that has been
// invalidated while pinned (unreachable by lookup, value drop deferred to the
// last unpin).
template <class Value> struct CacheEntry {
  Value value;
  std::size_t bytes;
  PriorityClass klass;
  std::uint64_t recency;
  unsigned pins;
  bool removed;
};

// Non-template-over-Key back-channel a CacheHold uses to release its pin,
// letting CacheHold<Value> stay independent of the store's Key. KeyedStore
// implements it privately.
template <class Value> class CacheHoldOwner {
public:
  virtual void unpin(CacheEntry<Value>* entry) noexcept = 0;

protected:
  ~CacheHoldOwner() = default;
};

} // namespace detail

// Move-only RAII residency pin on a stored entry (doc 02 delta). Constructing
// it pins the entry (excludes it from eviction candidacy); destroying or
// moving-out unpins exactly once. Dereferencing yields the live `Value&`.
// Moving transfers the unpin obligation, so there is never a double-unpin.
//
// This pin is store-internal -- it is **not** `arbc::ref`/`SlotRef` (which
// lives in `pool`, a forbidden L3 edge). It is layered above whatever backend
// refcount `Value` itself owns.
template <class Value> class CacheHold {
public:
  CacheHold() noexcept = default;

  CacheHold(const CacheHold&) = delete;
  CacheHold& operator=(const CacheHold&) = delete;

  CacheHold(CacheHold&& other) noexcept
      : d_owner(other.d_owner), d_entry(other.d_entry) {
    other.d_owner = nullptr;
    other.d_entry = nullptr;
  }

  CacheHold& operator=(CacheHold&& other) noexcept {
    if (this != &other) {
      release();
      d_owner = other.d_owner;
      d_entry = other.d_entry;
      other.d_owner = nullptr;
      other.d_entry = nullptr;
    }
    return *this;
  }

  ~CacheHold() { release(); }

  Value& operator*() const noexcept { return d_entry->value; }
  Value* operator->() const noexcept { return &d_entry->value; }
  Value& get() const noexcept { return d_entry->value; }

  // False for a default-constructed or moved-from hold (pins nothing).
  bool valid() const noexcept { return d_entry != nullptr; }

private:
  template <class K, class V> friend class KeyedStore;

  CacheHold(detail::CacheHoldOwner<Value>& owner,
            detail::CacheEntry<Value>* entry) noexcept
      : d_owner(&owner), d_entry(entry) {}

  void release() noexcept {
    if (d_entry != nullptr) {
      d_owner->unpin(d_entry);
      d_owner = nullptr;
      d_entry = nullptr;
    }
  }

  detail::CacheHoldOwner<Value>* d_owner{nullptr};
  detail::CacheEntry<Value>* d_entry{nullptr};
};

// The budgeted keyed store at the heart of `arbc::cache` (doc 02 Tile cache,
// doc 15 memory model). A generic container holding cached `Value`s under
// opaque `Key`s, tracking resident bytes against a byte budget and evicting
// **LRU within priority classes** when an insert would exceed the budget. Per
// doc 17 `cache` is engine-agnostic -- "tiles and blocks are the same
// machinery with different key shapes" -- so this is the machinery; the tile
// and block key/value shapes are `cache.key_shapes`.
//
// The store never introspects `Value`: byte cost is supplied explicitly at
// `insert`, and `Value` need only be movable (its destructor releases whatever
// backend resource it owns). `Key` need only be hashable and
// equality-comparable.
//
// Render-/mix-thread-confined and **not thread-safe** by design (doc 02:118-120
// "v1 may degenerate to everything on one thread"; surface_pool precedent). It
// holds no lock; concurrent reader-lookup vs. worker-fill is a designed future
// mode whose hardening is deferred. Not copyable or movable -- CacheHolds
// reference it by pointer, so callers hold it by reference.
template <class Key, class Value>
class KeyedStore : private detail::CacheHoldOwner<Value> {
public:
  using hold_type = CacheHold<Value>;

  explicit KeyedStore(std::size_t budget_bytes) : d_budget(budget_bytes) {}

  KeyedStore(const KeyedStore&) = delete;
  KeyedStore& operator=(const KeyedStore&) = delete;
  KeyedStore(KeyedStore&&) = delete;
  KeyedStore& operator=(KeyedStore&&) = delete;
  ~KeyedStore() = default;

  // Store `value` (costing `bytes`) under `key` in class `klass`, returning a
  // pinned hold on the just-stored entry. Inserting past budget evicts LRU
  // within priority classes (Speculative first) **before** the new entry is
  // admitted, until it fits or only pinned entries remain (soft budget). A
  // pre-existing entry under `key` is removed first (deferred drop if pinned).
  hold_type insert(Key key, Value value, std::size_t bytes, PriorityClass klass);

  // Exact-key lookup. A hit refreshes the entry's LRU recency within its class,
  // bumps hits(), and returns a pinned hold; a miss bumps misses() and returns
  // std::nullopt. (Whether a hit *qualifies* -- exact scale, current revision --
  // is the consumer's read of the value's metadata, not the store's concern.)
  std::optional<hold_type> lookup(const Key& key);

  // Move an entry between priority classes (no-op if absent). The policy
  // deciding an entry's class is the caller's; the store only honors the tag.
  void reclassify(const Key& key, PriorityClass klass);

  // Drop a key. If the entry is unpinned its value is released inline; if it is
  // pinned it becomes immediately unreachable by lookup but its value is held
  // until the last CacheHold releases (deferred drop). No-op if absent. This is
  // the primitive `cache.invalidation` builds its orphan index over.
  void remove(const Key& key);

  std::size_t resident_bytes() const noexcept { return d_resident; }
  std::size_t budget() const noexcept { return d_budget; }
  std::uint64_t hits() const noexcept { return d_hits; }
  std::uint64_t misses() const noexcept { return d_misses; }
  std::uint64_t evictions() const noexcept { return d_evictions; }

private:
  using Entry = detail::CacheEntry<Value>;
  using Map = std::unordered_map<Key, std::unique_ptr<Entry>>;

  void unpin(Entry* entry) noexcept override {
    --entry->pins;
    if (entry->pins == 0 && entry->removed) {
      d_resident -= entry->bytes;
      for (auto it = d_orphans.begin(); it != d_orphans.end(); ++it) {
        if (it->get() == entry) {
          d_orphans.erase(it);
          break;
        }
      }
    }
  }

  // Detach `it` from the map: release inline if unpinned, else orphan it (mark
  // removed, keep the Entry alive for outstanding holds, keep its bytes counted
  // until the deferred drop).
  void detach(typename Map::iterator it) {
    Entry* e = it->second.get();
    if (e->pins == 0) {
      d_resident -= e->bytes;
      d_map.erase(it);
    } else {
      e->removed = true;
      d_orphans.push_back(std::move(it->second));
      d_map.erase(it);
    }
  }

  // The min-recency evictable (unpinned) entry of the lowest priority class
  // that has one; d_map.end() when only pinned entries remain.
  typename Map::iterator pick_victim() {
    for (PriorityClass klass : detail::cache_eviction_order()) {
      auto victim = d_map.end();
      for (auto it = d_map.begin(); it != d_map.end(); ++it) {
        Entry* e = it->second.get();
        if (e->klass != klass || e->pins != 0) {
          continue;
        }
        if (victim == d_map.end() || e->recency < victim->second->recency) {
          victim = it;
        }
      }
      if (victim != d_map.end()) {
        return victim;
      }
    }
    return d_map.end();
  }

  // Evict until the incoming bytes fit or only pinned entries remain.
  void evict_to_fit(std::size_t incoming) {
    while (d_resident + incoming > d_budget) {
      auto victim = pick_victim();
      if (victim == d_map.end()) {
        break; // only pinned entries remain -> soft overshoot
      }
      d_resident -= victim->second->bytes;
      ++d_evictions;
      d_map.erase(victim); // value released inline (entry is unpinned)
    }
  }

  std::size_t d_budget;
  std::size_t d_resident{0};
  std::uint64_t d_tick{0};
  std::uint64_t d_hits{0};
  std::uint64_t d_misses{0};
  std::uint64_t d_evictions{0};
  Map d_map;
  std::vector<std::unique_ptr<Entry>> d_orphans;
};

template <class Key, class Value>
CacheHold<Value> KeyedStore<Key, Value>::insert(Key key, Value value,
                                                std::size_t bytes,
                                                PriorityClass klass) {
  if (auto existing = d_map.find(key); existing != d_map.end()) {
    detach(existing);
  }
  evict_to_fit(bytes);
  auto entry = std::make_unique<Entry>(
      Entry{std::move(value), bytes, klass, ++d_tick, 1u, false});
  Entry* raw = entry.get();
  d_resident += bytes;
  d_map.emplace(std::move(key), std::move(entry));
  return CacheHold<Value>(*this, raw);
}

template <class Key, class Value>
std::optional<CacheHold<Value>> KeyedStore<Key, Value>::lookup(const Key& key) {
  auto it = d_map.find(key);
  if (it == d_map.end()) {
    ++d_misses;
    return std::nullopt;
  }
  ++d_hits;
  Entry* e = it->second.get();
  e->recency = ++d_tick;
  ++e->pins;
  return CacheHold<Value>(*this, e);
}

template <class Key, class Value>
void KeyedStore<Key, Value>::reclassify(const Key& key, PriorityClass klass) {
  auto it = d_map.find(key);
  if (it == d_map.end()) {
    return;
  }
  it->second->klass = klass;
}

template <class Key, class Value>
void KeyedStore<Key, Value>::remove(const Key& key) {
  auto it = d_map.find(key);
  if (it == d_map.end()) {
    return;
  }
  detach(it);
}

} // namespace arbc
