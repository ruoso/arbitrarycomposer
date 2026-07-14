#include <arbc/base/hash_mix.hpp> // mix64 (a hash-table mixer, which is all it claims to be)
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/serialize/tile_blob.hpp>

#include <span>
#include <utility>
#include <vector>

namespace arbc {
namespace {

// The memo key. `BlockSlotRef` has no `std::hash`, and its release-build identity is
// exactly `{index, size}` -- which is the hazard the owning pin exists to close, not a
// convenience. The storage format is part of the key because the hash is over STORAGE
// bytes: the same pixels at `rgba16f` and at `rgba32f` are different content and must
// not alias.
std::uint64_t pack(std::uint32_t index, std::uint32_t size) {
  return (static_cast<std::uint64_t>(index) << 32) | static_cast<std::uint64_t>(size);
}

} // namespace

std::size_t RasterTileStore::KeyHash::operator()(const Key& k) const noexcept {
  // `mix64` is the right tool HERE and the wrong one for the blob name: this is a
  // bucket index over a trusted 96-bit key, not a content identity over untrusted bytes.
  // The content hash is SHA-256 (Decision 3); this is not, and its own comment says so.
  const std::uint64_t mixed =
      mix64(pack(k.index, k.size) ^ (static_cast<std::uint64_t>(k.storage) + 0x9E3779B9ULL));
  return static_cast<std::size_t>(mixed);
}

void RasterTileStore::begin_pass(PixelFormat storage) {
  const std::lock_guard<std::mutex> lock(d_mutex);
  d_storage = storage;
  d_pass.clear();
  d_in_pass = true;
}

void RasterTileStore::end_pass() {
  const std::lock_guard<std::mutex> lock(d_mutex);
  if (!d_in_pass) {
    return;
  }
  // The swap IS the trim: `d_live`'s old entries drop here, and with them their `BlockRef`
  // pins -- releasing exactly the tiles no longer in the version we just saved. What
  // survives is the last-saved document's tile set and nothing else.
  d_live = std::move(d_pass);
  d_pass.clear();
  d_in_pass = false;
}

std::string RasterTileStore::hash_of(BigBlockPool& pool, BlockSlotRef ref) {
  const std::lock_guard<std::mutex> lock(d_mutex);
  const Key key{ref.index(), ref.size(), d_storage};

  // Already carried into this pass: two layers sharing a tile, or one tile named many
  // times in the same grid -- which is the all-empty layer, where every slot IS the same
  // slot and the whole level collapses to one blob.
  if (const auto it = d_pass.find(key); it != d_pass.end()) {
    return it->second.hash;
  }

  // A hit against the last completed pass: carry the hash AND the pin forward without
  // touching a byte of the tile. This is the whole incremental-save win, and it is sound
  // only because the pin proves the slot behind the key was never recycled.
  if (const auto it = d_live.find(key); it != d_live.end()) {
    Entry carried = it->second; // copies the BlockRef -> retains
    std::string hash = carried.hash;
    d_pass.emplace(key, std::move(carried));
    return hash;
  }

  // A miss. Read the blob (zero-refcount `peek`, any thread -- the caller's pinned
  // `TileTablePtr` is what keeps it alive), convert to the document's storage format, and
  // hash THOSE bytes: never the working bytes, never the compressed frame.
  const std::span<const std::byte> raw = pool.peek(ref);
  const std::span<const float> working{reinterpret_cast<const float*>(raw.data()),
                                       raw.size() / sizeof(float)};
  const std::vector<std::byte> storage_bytes = to_storage_bytes(working, d_storage);
  std::string hash = hash_tile(storage_bytes);
  d_tiles_hashed.fetch_add(1, std::memory_order_relaxed);

  // Take our OWN count. The caller already holds one -- the pinned `TileTablePtr` retains
  // every slot it names -- so this is always "add a count to something already >= 1" and
  // can never resurrect a dead slot (Constraint 10).
  if (expected<BlockRef, RefError> pin = pool.resolve(ref); pin) {
    d_pass.emplace(key, Entry{hash, std::move(*pin)});
  }
  // A pin we could not take (a count at its 2^32 ceiling) simply means no memo entry:
  // correct, just not memoized. Never a stale hash.
  return hash;
}

void RasterTileStore::seed(BigBlockPool& pool, BlockSlotRef ref, PixelFormat storage,
                           const std::string& hash) {
  const std::lock_guard<std::mutex> lock(d_mutex);
  const Key key{ref.index(), ref.size(), storage};
  if (d_live.find(key) != d_live.end()) {
    return;
  }
  if (expected<BlockRef, RefError> pin = pool.resolve(ref); pin) {
    d_live.emplace(key, Entry{hash, std::move(*pin)});
  }
}

std::size_t RasterTileStore::memoized() const {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_live.size();
}

} // namespace arbc
