#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/media/pixel_traits.hpp> // WorkingPixel
#include <arbc/surface/surface.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arbc::image {

// `org.arbc.image` -- a still image decoded from an EXTERNAL asset URI, and the half of
// the pixel-persistence split that stores NOTHING in the document (doc 08 Principle 3).
//
// Two properties define it, and both are structural rather than conventional:
//
//   * It is READ-ONLY. `editable()` stays at its `nullptr` default (doc 03:256-268).
//     That single omission is what makes non-destructive editing structural: you cannot
//     paint on a photograph, so retouching one MUST stack an editable `org.arbc.raster`
//     above a referenced `org.arbc.image`. It is not an omission to be "fixed" later.
//   * It STORES NOTHING. Its `params` is exactly `{"source": "<authored-uri>"}` -- no
//     pixels, no tiles, no intrinsic size, no decoded cache. The pixels live in the file
//     the URI names; the document holds the reference and nothing else.
//
// It is the sibling of `org.arbc.imageseq` and, like it, ships OUTSIDE `libarbc` behind
// doc 17's codec line -- but only the DECODER crosses that line. The serialize codec that
// reads `params.source` lives in `runtime` (`src/runtime/codec_image.cpp`), because it
// parses our own JSON and a URI string and never an encoded image byte (doc 17 "The codec
// line is a decoder line"). Consequently THIS PLUGIN PERFORMS NO FILE I/O AT ALL: the
// core resolves the URI through `LoadContext` and fetches the bytes through the
// `AssetSource` hook, then hands the already-fetched bytes here through the opaque,
// kind-defined `ContentConfig` (kinds/image.md Decision 5).
//
// Contract shape:
//   * stability() == Static; time_extent() == nullopt; quantize_time keeps its nullopt
//     default; achieved_time stays nullopt; audio() == nullptr; inputs() empty.
//   * render() answers with a content-PROVIDED surface (doc 09:150-182) covering exactly
//     the requested region at the achieved scale -- never a whole decoded frame
//     (Decision 1). It is non-transient and refcounted; its release callback returns it
//     to a small plugin-owned free list.
//   * render_thread_safe() == true: a `Pyramid` object is IMMUTABLE FOR ITS ENTIRE LIFE, a
//     render holds an OWNING pin on the one it reads, and the `PyramidCache` that hands the
//     pin out is mutex-guarded. Those are the three legs (kinds.image_master_budget
//     Decision 3); what the flag no longer rests on is monotonicity of a content-owned
//     pointer, because the pixels are now BUDGETED DERIVED DATA that eviction may drop and a
//     re-decode may rebuild byte-identically.
//   * An UNAVAILABLE image (missing / unreadable / undecodable asset, or no `AssetSource`
//     installed) reports EMPTY bounds and renders nothing, keeping its authored URI
//     verbatim so the layer re-saves byte-identically (Decision 7). It is never a load
//     error (doc 08:126-134).
//   * A PENDING image -- one whose `AssetSource` has not ANSWERED yet -- is minted in
//     exactly the unavailable shape and GAINS its pixels later, through `install_asset()`
//     on the writer thread (kinds.image_async_pending). Pending and unavailable differ by
//     whether the source answered, never by the bytes being empty; the kind cannot tell
//     them apart and does not need to, because doc 08:135-144 already made an extent-less
//     image render nothing.

// An immutable mip pyramid over one decoded master. Level 0 is the decoded image in the
// working space (`Rgba32fLinearPremul`); each level above is a 2:1 Lanczos-3 half-band
// decimation of the one below, through `media`'s frozen kernel bank -- the SAME kernels
// `org.arbc.raster` uses, reached through `arbc::media` (L2), which is exactly where a
// plugin may reach. Built once at construction and never mutated: no CoW, no
// `StateHandle`, no `ChunkSource`, no pool backing, no versioning (Decision 4).
// Read-only implies immutable implies none of raster's tile machinery is needed -- and
// immutability is what makes `render_thread_safe()` an obvious `true` rather than a
// question.
class Pyramid {
public:
  // Decode `encoded` (an stb-class encoded still) into a fresh pyramid, or nullptr when
  // the bytes are absent or undecodable. Errors are values; nothing throws (Constraint 7).
  static std::shared_ptr<const Pyramid> decode(std::span<const unsigned char> encoded);

  int width() const noexcept { return d_width; }
  int height() const noexcept { return d_height; }
  std::size_t level_count() const noexcept { return d_levels.size(); }

  // The working-space pixel at level `level`, CLAMPED TO EDGE -- matching the convention
  // raster's `TileTable::pixel` uses, so both resample paths read a border rather than a
  // zero surround (which would darken every edge).
  WorkingPixel pixel(std::size_t level, int x, int y) const;

  // Resident bytes across every level -- the measuring instrument a later byte-budgeted
  // eviction (`kinds.image_master_budget`) needs.
  std::size_t resident_bytes() const noexcept;

private:
  struct Level {
    int width{0};
    int height{0};
    std::vector<float> px; // rgba32f, tightly packed, row-major
  };

  int d_width{0};
  int d_height{0};
  std::vector<Level> d_levels;
};

using PyramidPtr = std::shared_ptr<const Pyramid>;

class PyramidPin;

// The byte budget `default_pyramid_cache()` bounds its decoded pyramids by, overridable through
// `ARBC_IMAGE_PYRAMID_BUDGET_BYTES` (kinds.image_master_budget Decision 8). The named-constant
// shape follows `offline_sequence.hpp`'s `k_default_sequence_cache_budget`.
//
// A 24 MP master at rgba32f is ~384 MB and its mips add ~1/3, so 1 GiB is roughly two
// photographs resident -- a judgment call, and a safe one only because the budget is a SOFT
// target (doc 02:278-284): guessing low costs re-decodes, never correctness.
inline constexpr std::size_t k_default_pyramid_budget = std::size_t{1024} * 1024 * 1024;

// The plugin-side pyramid cache, keyed by RESOLVED URI (doc 08:116-122: cross-file sharing
// dedups by resolved identity, not by spelling). N layers whose authored refs resolve to
// one URI -- `bg.png` and `./bg.png` -- share ONE decoded pyramid and issue exactly ONE
// decode; `decodes_issued()` is the behavioral counter that pins it (doc 16:54-62), never a
// wall-clock assertion.
//
// It OWNS its pyramids STRONGLY and bounds them by a byte budget, evicting LRU and skipping
// PINNED entries (kinds.image_master_budget Decision 1, doc 02:255-284's tile-cache policy
// applied to a second population; doc 15's populations table names it). The ownership is the
// load-bearing half: while a content held the strong reference and the cache held a
// `weak_ptr`, evicting a map entry freed ZERO bytes for as long as the layer lived, and a
// byte budget over values the cache does not own is decoration. So the cache owns, and every
// other reference is a transient `PyramidPin` whose lifetime is one `render()` call.
//
// What eviction drops is the DERIVED data (the pyramid). The ENCODED bytes -- ~8 MB against a
// ~512 MB pyramid, a ~64:1 ratio -- are retained per entry and are NOT part of the budget:
// they are the source a re-decode reconstructs from, and this plugin performs no file I/O at
// all, so bytes it does not keep are bytes it can never get back (Decision 2).
//
// Mutex-guarded, and this task puts it on EVERY RENDER THREAD (a pin that misses decodes
// while holding the mutex, Decision 9 -- which is what preserves one-decode-per-key under
// concurrency for free). Deliberately NOT `arbc::cache`'s `KeyedStore`: doc 17:70 grants a
// kind only a `contract` edge and `cache` is not in its closure, and `KeyedStore` is
// documented not thread-safe (Decision 7). Its SHAPE and VOCABULARY are copied --
// `resident_bytes()`, `budget()`, `evictions()`, an RAII pin -- so the two caches read alike.
class PyramidCache {
public:
  // Unbudgeted unless it asks, exactly as `Journal` (`journal.hpp:56-61`) is: a
  // test-constructed cache never evicts, so a test opts INTO the budget rather than out of it.
  static constexpr std::size_t k_no_budget = std::numeric_limits<std::size_t>::max();

  explicit PyramidCache(std::size_t byte_budget = k_no_budget) noexcept : d_budget(byte_budget) {}

  // ADMIT `resolved_uri`: retain `encoded`, decode it on a miss, and return the pyramid.
  // nullptr when the bytes are absent or undecodable -- the UNAVAILABLE state, never an error.
  // An empty `resolved_uri` is never cached (it names no identity to dedup on), and EMPTY BYTES
  // ARE NEVER SATISFIED FROM THE CACHE even on a resident key: absence is a statement that the
  // caller's source did not answer, and answering it with another document's decode would make a
  // still-pending photograph render (see the impl). The returned pointer is an owning, TRANSIENT
  // hold: the caller reads the extent off it and drops it, leaving the cache the only persistent
  // owner.
  PyramidPtr resolve(std::string_view resolved_uri, std::span<const unsigned char> encoded);

  // A RESIDENCY PIN on `resolved_uri`'s pyramid, RE-DECODING from the retained encoded bytes
  // when the entry was evicted (doc 02:268-277). Empty when the URI was never admitted -- the
  // pending/unavailable state. While the pin is live the entry is never a victim, and because
  // the pin is an OWNING `shared_ptr` even a dropped entry's pixels outlive it: an eviction
  // racing a render can never free pixels the render is reading.
  PyramidPin pin(std::string_view resolved_uri);

  // One per genuine decode -- including a RE-decode after an eviction, which is exactly what
  // makes the counter the measuring instrument for the budget. Re-pulling a resident pyramid
  // bumps it zero; re-pulling an evicted one bumps it by exactly one.
  std::uint64_t decodes_issued() const noexcept;

  // Behavioral counters and the budget's accounting (the `KeyedStore` naming, doc 17:124-138).
  // `resident_bytes()` may exceed `budget()` when the PINNED set alone does: the budget is a
  // soft target and correctness outranks it (doc 02:278-284).
  std::uint64_t evictions() const noexcept;
  std::size_t resident_bytes() const noexcept;
  std::size_t budget() const noexcept;

  // Re-budget in place, trimming immediately to the new ceiling. The seam an in-process host
  // that links the impl archive directly configures through; a host that `dlopen`s the MODULE
  // reaches the process-wide instance through `ARBC_IMAGE_PYRAMID_BUDGET_BYTES` instead, there
  // being no plugin-wide policy channel in the registration ABI (Decision 8).
  void set_byte_budget(std::size_t byte_budget);

private:
  friend class PyramidPin;

  // Entries are NEVER erased -- an evicted entry keeps its encoded bytes so the next pin can
  // rebuild it -- which is what lets a pin hold a bare `Entry*` across a rehash.
  struct Entry {
    std::shared_ptr<const std::vector<unsigned char>> encoded; // the source (Decision 2)
    PyramidPtr resident;      // the DERIVED data the budget governs; null when evicted
    std::size_t bytes{0};     // `Pyramid::resident_bytes()` while resident, 0 when not
    std::uint64_t recency{0}; // LRU order, bumped on every pin and admit
    std::size_t pins{0};      // doc 02:268-277 -- never a victim while > 0
  };

  void unpin(Entry* entry) noexcept;
  // Decode into an evicted (or fresh) entry and account for its bytes. Under `d_mutex`.
  PyramidPtr refill_locked(Entry& entry);
  // Drop unpinned pyramids, least-recently-pinned first, until resident bytes are within
  // budget or only pinned entries remain (`keyed_store.hpp:251-261`'s `evict_to_fit`).
  void evict_to_fit_locked();
  Entry* pick_victim_locked();

  mutable std::mutex d_mutex;
  std::unordered_map<std::string, Entry> d_by_uri;
  std::size_t d_budget;
  std::size_t d_resident{0};
  std::uint64_t d_tick{0};
  std::uint64_t d_decodes_issued{0};
  std::uint64_t d_evictions{0};
};

// A move-only RAII residency pin: doc 02:268-277's pin, applied verbatim to a second cache.
// Constructing it excludes the entry from eviction candidacy; destroying it releases the
// exclusion and trims the cache back to budget.
//
// It is STRICTLY STRONGER than `KeyedStore`'s `CacheHold` (`keyed_store.hpp:78`), because it
// carries an OWNING `shared_ptr` rather than a bare index: even an entry the cache has already
// dropped keeps its pixels alive until the last render reading them finishes. That is the
// memory-safety leg `render_thread_safe()` now stands on.
class PyramidPin {
public:
  PyramidPin() noexcept = default;

  // An UNBUDGETED pin over a directly-owned pyramid: the un-keyed content path, which has no
  // cache identity to re-decode from and therefore owns its pixels un-evictably (Constraint 6).
  explicit PyramidPin(PyramidPtr owned) noexcept : d_pyramid(std::move(owned)) {}

  PyramidPin(const PyramidPin&) = delete;
  PyramidPin& operator=(const PyramidPin&) = delete;
  PyramidPin(PyramidPin&& other) noexcept
      : d_cache(other.d_cache), d_entry(other.d_entry), d_pyramid(std::move(other.d_pyramid)) {
    other.d_cache = nullptr;
    other.d_entry = nullptr;
  }
  PyramidPin& operator=(PyramidPin&& other) noexcept {
    if (this != &other) {
      release();
      d_cache = other.d_cache;
      d_entry = other.d_entry;
      d_pyramid = std::move(other.d_pyramid);
      other.d_cache = nullptr;
      other.d_entry = nullptr;
    }
    return *this;
  }
  ~PyramidPin() { release(); }

  const Pyramid* get() const noexcept { return d_pyramid.get(); }
  const Pyramid& operator*() const noexcept { return *d_pyramid; }
  const Pyramid* operator->() const noexcept { return d_pyramid.get(); }
  explicit operator bool() const noexcept { return d_pyramid != nullptr; }
  // The owning hold itself, for a caller that must outlive the pin.
  const PyramidPtr& shared() const noexcept { return d_pyramid; }

private:
  friend class PyramidCache;
  PyramidPin(PyramidCache* cache, PyramidCache::Entry* entry, PyramidPtr pyramid) noexcept
      : d_cache(cache), d_entry(entry), d_pyramid(std::move(pyramid)) {}

  // Unpin first, then drop the pixels -- so the (potentially ~512 MB) free happens OUTSIDE the
  // cache mutex, and only when this was the last hold.
  void release() noexcept {
    if (d_cache != nullptr) {
      d_cache->unpin(d_entry);
      d_cache = nullptr;
      d_entry = nullptr;
    }
    d_pyramid.reset();
  }

  PyramidCache* d_cache{nullptr};
  PyramidCache::Entry* d_entry{nullptr};
  PyramidPtr d_pyramid;
};

// The process-wide cache the registered `ContentFactory` resolves through, so two LAYERS
// of one document (each built by its own factory call) share a decode. Budgeted at
// `k_default_pyramid_budget` unless `ARBC_IMAGE_PYRAMID_BUDGET_BYTES` names another number.
// A test may pass its own cache to `ImageContent` directly to keep its counter isolated.
PyramidCache& default_pyramid_cache();

// A working-space CPU surface the plugin owns outright -- `std::vector<std::byte>` storage,
// format `k_working_rgba32f`, NO `Backend` edge (the `FrameSurface` shape imageseq
// established). A plugin allocates no surface through a `Backend`: `backend-cpu` is L3 and
// forbidden to it, and pixels route only through `arbc::media` (Constraint 1).
class TileSurface final : public Surface {
public:
  TileSurface(int width, int height);

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  SurfaceFormat format() const override { return d_format; }
  std::span<std::byte> cpu_bytes() override { return d_data; }
  std::span<const std::byte> cpu_bytes() const override { return d_data; }

private:
  int d_width;
  int d_height;
  SurfaceFormat d_format{k_working_rgba32f};
  std::vector<std::byte> d_data;
};

// The small plugin-owned free list a provided surface is returned to when the compositor
// releases it (Decision 1). Held by `shared_ptr` and captured by the release callback, so
// it outlives the content whenever a `SurfaceRef` does -- doc 09:176-182 pins the surface
// until the compositor has composited or copied it, and that may outlast the content.
class TileStore {
public:
  // A surface of exactly (width, height), recycled from the free list when one of that
  // size is resting there. Never null.
  std::shared_ptr<TileSurface> acquire(int width, int height);

  // Return a released surface to the free list (bounded; the surplus is simply dropped).
  void release(std::shared_ptr<TileSurface> surface);

  // Behavioral counter: how many surfaces were allocated rather than recycled.
  std::uint64_t allocations() const noexcept;

private:
  static constexpr std::size_t k_max_resting = 8;

  mutable std::mutex d_mutex;
  std::vector<std::shared_ptr<TileSurface>> d_free;
  std::uint64_t d_allocations{0};
};

class ImageContent final : public Content {
public:
  static constexpr const char* kind_id = "org.arbc.image";

  // `authored_uri` is the reference EXACTLY AS THE DOCUMENT SPELLED IT -- the thing that
  // round-trips (Constraint 5). `pyramid` may be null: that is the unavailable state (and
  // also the PENDING one, which is minted in the same shape), and it is a perfectly
  // ordinary content, not an error (Decision 7).
  //
  // The RESOLVED URI is kept too, and it is now the content's ONLY handle on its pixels: it
  // is the `PyramidCache` key every render pins through, and the key `install_asset` admits a
  // late arrival under -- which is what makes a pending-then-settled image cost exactly ONE
  // decode, and N contents on one URI exactly one decode between them
  // (kinds.image_async_pending Decision 6). The content holds NO persistent strong reference
  // to the keyed pyramid; the cache owns it and the budget bounds it
  // (kinds.image_master_budget Decision 1).
  //
  // The overload WITHOUT a resolved URI builds an UN-KEYED content: a caller handing over an
  // already-decoded pyramid, which has no cache identity, nothing to re-decode from, and
  // therefore keeps owning its pixels strongly and un-evictably (Constraint 6). Correct for a
  // test, and for any caller that will never defer.
  ImageContent(std::string authored_uri, PyramidPtr pyramid);
  ImageContent(std::string authored_uri, std::string resolved_uri, PyramidPtr pyramid,
               PyramidCache& cache = default_pyramid_cache());

  // --- Content (description) ---
  // The decoded master's extent, or an EMPTY rect when the asset is unavailable. Empty
  // rather than a fabricated placeholder rectangle: the intrinsic size is knowable only by
  // decoding, and Constraint 4 forbids caching it in the document, so there is literally no
  // rectangle to draw a placeholder over. Inventing one would let a MISSING file change the
  // composition's geometry (Decision 7, doc 08 Principle 3 as amended).
  //
  // It reads the RETAINED EXTENT, takes NO PIN, takes no lock and issues NO DECODE -- and
  // that is a memory-policy invariant, not an optimization (kinds.image_master_budget
  // Constraint 2). `bounds()` sits on the compositor's CULL path: if an evicted image reported
  // empty bounds it would cull ITSELF out of the composition, and a photograph would vanish
  // because memory got tight. Residency is not geometry. An EVICTED image is not an
  // UNAVAILABLE one.
  std::optional<Rect> bounds() const override;
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }

  // --- Content (render) ---
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;
  // The core may dispatch this leaf to workers (doc 00:203's leaf-only worker dispatch) --
  // the real win over imageseq, whose stateful decoder + LRU force per-content serialization.
  //
  // The flag has now been re-argued TWICE, and it is worth naming both retirements because the
  // current legs are the narrow ones that survive:
  //   * "the pyramid is immutable AFTER CONSTRUCTION" -- retired by kinds.image_async_pending
  //     Decision 6, which gave the kind a late install.
  //   * "the content's pyramid POINTER is monotonic (null -> non-null, never reverting)" --
  //     retired by kinds.image_master_budget Decision 3, which makes the pixels evictable
  //     derived data, so non-null -> null -> non-null is now a legal history.
  // What is left, and what actually buys the flag: (a) a `Pyramid` OBJECT is immutable for its
  // entire life and eviction never mutates one, it only drops a reference; (b) a render holds
  // an OWNING `PyramidPin`, so a concurrent eviction can never free pixels it is reading; and
  // (c) `PyramidCache` is mutex-guarded. A worker still observes exactly two self-consistent
  // states -- a fully-built pyramid it owns for the duration of the call, or no pixels -- and
  // never a third.
  //
  // What publishes ONCE and MONOTONICALLY is now the EXTENT (`d_extent`), which is the half
  // doc 03's obligation actually cares about, because it is the half the compositor culls on.
  bool render_thread_safe() const override { return true; }

  // --- Content (discovery) ---
  // `editable()` is deliberately NOT overridden (Constraint 2).
  std::string_view external_asset_ref() const override { return d_uri; }

  // The pixels of a PENDING asset, arriving after construction on the writer thread. The bytes
  // are admitted to the cache under this content's resolved URI, and the decoded EXTENT is
  // published -- once, atomically, and never cleared thereafter, by eviction or by anything
  // else. A second arrival for a content that already has an extent installs nothing and
  // returns true. Empty or undecodable bytes leave the content unavailable and return false --
  // a value, never a throw.
  bool install_asset(std::string_view encoded) override;

  // --- plugin-local observers ---
  // "This content has a KNOWN EXTENT", NOT "its pyramid is resident" (Constraint 2). An
  // evicted image is still available: it has bounds and it renders (re-decoding on the pull).
  // Only a genuinely unavailable one -- no bytes, or undecodable bytes -- is not.
  bool available() const noexcept { return d_extent.load(std::memory_order_acquire) != 0; }

  // An OWNING residency pin on this content's pixels, live for as long as the caller holds it:
  // through the cache for a keyed content (re-decoding when the entry was evicted), or over the
  // directly-owned pyramid for an un-keyed one. This is the ONE accessor both paths resolve
  // through.
  PyramidPin pixels() const;

  // The pinned pyramid as a bare owning pointer -- a convenience over `pixels()`, and the shape
  // that predates the pin. Null when the asset is unavailable OR, for a keyed content, when
  // the cache cannot rebuild it.
  PyramidPtr pyramid() const { return pixels().shared(); }

  // The resolved identity the pyramid cache dedups this content's decode on. Empty when the
  // content was built without one (see the two-argument constructor).
  const std::string& resolved_ref() const noexcept { return d_resolved; }
  // How many surfaces the free list had to allocate rather than recycle.
  std::uint64_t tile_allocations() const noexcept { return d_tiles->allocations(); }

private:
  // Publish the decoded extent, exactly once (a CAS off zero: a redundant install, or two
  // racing ones, stores nothing the second time).
  void publish_extent(const Pyramid& pyramid) noexcept;

  std::string d_uri;
  std::string d_resolved;
  PyramidCache* d_cache;
  // The un-keyed path's pyramid (Constraint 6): set at construction, never reassigned, outside
  // the budget. Null for every keyed content -- the cache owns those, and a content that held a
  // persistent strong reference to a budgeted pyramid would make the budget a lie.
  PyramidPtr d_owned;
  // The decoded extent, packed as (width << 32 | height) and published ONCE, monotonically:
  // zero means "not yet known", and a decode can never yield 0x0 (it rejects w <= 0 / h <= 0).
  // This is the half of the old `d_pyramid` pointer that MUST stay monotonic (doc 03's
  // install_asset obligation, as amended by kinds.image_master_budget Decision 3) -- because
  // `bounds()` reads it on the cull path and eviction must be invisible there. The other half,
  // the pixels, moved into the cache and became budgeted derived data.
  std::atomic<std::uint64_t> d_extent{0};
  std::shared_ptr<TileStore> d_tiles;
};

// The `ContentConfig` framing the core's serialize codec hands to this factory
// (Decision 5). `ContentConfig` is explicitly an opaque, kind-defined `string_view`
// (`registry.hpp:35`), so a length-delimited frame needs no ABI change:
//
//     "<authored-uri>\n<resolved-uri>\n<encoded-bytes>"
//
// split at the FIRST two newlines. A URI cannot contain a raw newline and `normalize_uri`
// is purely lexical, so the framing is unambiguous. Both URIs are carried because they
// answer different questions: the AUTHORED one is what round-trips into `params.source`
// (Constraint 5), while the RESOLVED one is the identity the pyramid cache dedups on (doc
// 08:116-122) -- `bg.png` and `./bg.png` are two authored refs and one decode.
//
// EMPTY bytes after the second `\n` mean the asset is unavailable: the factory still
// yields a content (URI kept, no pixels), because a missing external file is a condition of
// the environment, never a read error (Constraint 6). Only a MALFORMED FRAME -- a config
// that is not two-newline-delimited at all, which is a caller bug, not user data -- is an
// error value.
expected<std::unique_ptr<Content>, std::string> make_image_content(ContentConfig config);

// Build the `ContentConfig` frame above. Exposed so the codec in `runtime` and the tests
// agree on the framing by construction rather than by comment.
std::string image_config(std::string_view authored_uri, std::string_view resolved_uri,
                         std::string_view encoded_bytes);

} // namespace arbc::image
