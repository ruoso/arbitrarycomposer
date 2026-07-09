#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/surface/surface.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

// The concrete cache key/value shapes (doc 02 Tile cache, doc 11 time axis,
// doc 12 audio blocks). `cache.keyed_store` delivered the generic
// `KeyedStore<Key, Value>`; this header instantiates it. Per doc 17 the cache
// is engine-agnostic -- "tiles and blocks are the same machinery with
// different key shapes" -- so both key structs live in this one L3 component,
// even though the audio block *value* and its `KeyedStore<BlockKey, ...>`
// instantiation belong to the audio-engine stream (doc 12).
//
// Levelization note (doc 17:54): fields are expressible in `base` + `surface`
// (+ transitive `media`) vocabulary only. `revision` is a bare
// `std::uint64_t` opaque token, not model's `DocRoot::revision()` type (an L2
// concept the L3 cache may not depend on); the compositor (L4, sees both)
// projects it into the key with a plain copy. `TileMeta` mirrors
// `contract::RenderResult`'s fields rather than reusing it (a forbidden L3->L3
// edge). Scale rung, tile coords, block index, and rate are opaque integers
// the compositor / audio-engine compute and hand down; the cache only hashes
// and compares them.

namespace arbc {

// The scale-ladder rung a tile grid is aligned to (doc 02:57-60: "a fixed
// local-space-aligned tile grid per scale rung"). Cache-local integer newtype
// -- base geometry (`Vec2`/`Rect`) is double-based and supplies no integer
// grid coord; the compositor owns the ladder and hands the rung down.
struct ScaleRung {
  std::int32_t index{0};

  friend constexpr bool operator==(const ScaleRung&, const ScaleRung&) = default;
};

// A tile's integer grid coordinate, meaningful only relative to a `ScaleRung`
// (each rung has its own tile grid, doc 02:57-60). Cache-local newtype for the
// same reason as `ScaleRung`.
struct TileCoord {
  std::int32_t col{0};
  std::int32_t row{0};

  friend constexpr bool operator==(const TileCoord&, const TileCoord&) = default;
};

// The 2D tile cache key (doc 02:89 base tuple + doc 11:138-143 time extension):
// `(content, revision, scale rung, tile coords[, achieved_time])`.
//
// `achieved_time` is `std::optional<Time>`, **absent for `Static` content** so
// clock advance grows no cache for stills (doc 11:139-140); present for `Timed`
// content, carrying the local time actually rendered (the compositor's
// achieved-time quantization is its read, not the store's). A Static key
// (nullopt) and a Timed key are always distinct -- including a Timed key at
// `Time::zero()` (the `flicks == 0` collision guard).
struct TileKey {
  ObjectId content;
  std::uint64_t revision{0};
  ScaleRung rung;
  TileCoord coord;
  std::optional<Time> achieved_time;

  // Every member is equality-comparable (`ObjectId`, `ScaleRung`, `TileCoord`
  // provide `==`; `std::optional<Time>` compares present-vs-absent and by
  // value), so equality is member-wise -- two keys differing in exactly one
  // field are unequal.
  friend bool operator==(const TileKey&, const TileKey&) = default;
};

// The 1D audio block cache key (doc 12:249-254):
// `(content, revision, block index, rate, spatial-context digest)` -- "the block
// cache is the tile cache with 1D keys". `rate` is the audio working `sample_rate`
// (`std::uint32_t`). Channel layout is deliberately **not** a key field (doc
// 12: it is per-composition working format, not a key discriminator).
//
// `spatial_digest` is an **opaque** 64-bit scalar disambiguating a block whose
// `render_audio` output depends on the spatial context under which it renders (a
// nested composition mixed under two distinct listeners -- doc 12:249-254,
// spatial_blockkey_disambiguation). It is `0` exactly when the request is Flat, so a
// Flat/leaf-host scene keys byte-identically to the pre-digest key. Like `revision`
// and the tile-key rung/coords, the cache never interprets it: the L4 mix/lookahead
// engine (which holds the `Spatialization`) reduces it via
// `contract::spatial_context_digest` and hands it down -- the `cache`->`contract`
// levelization edge (doc 17:40-44) is never crossed, exactly as `TileMeta` mirrors
// rather than reuses contract's fields. Trailing and defaulted (`{0}`) so every
// existing `BlockKey{...}` aggregate initializer stays valid.
struct BlockKey {
  ObjectId content;
  std::uint64_t revision{0};
  std::int64_t block_index{0};
  std::uint32_t rate{0};
  std::uint64_t spatial_digest{0};

  friend bool operator==(const BlockKey&, const BlockKey&) = default;
};

// The tile cache value's metadata (doc 02:90-91: "actual scale achieved, exact
// vs best-effort flag"). Defined here in `cache`, mirroring
// `contract::RenderResult`'s `{achieved_scale, exact}` -- it may **not** reuse
// the contract type (a forbidden same-level L3 edge, doc 17:40-44); the
// compositor copies the two fields when it fills a tile from a `RenderResult`.
// The consumer reads these (exact-scale / current-revision qualification, doc
// 02:64-65,:82-85); the cache itself never inspects them.
struct TileMeta {
  double achieved_scale{1.0};
  bool exact{true};
};

// The tile cache's value (doc 02:90-91): an owning backend surface plus its
// metadata. Move-only via the `unique_ptr` (the store's only `Value`
// requirement); its destructor releases the backend surface. This is a
// distinct backend surface the cache owns for as long as it is resident -- not
// a `surfaces::PooledSurface` (the temp pool has its own lifecycle and budget
// and is **not** the tile cache, surface_pool.md:100-107).
struct TileValue {
  std::unique_ptr<Surface> surface;
  TileMeta meta;
};

// Byte cost of caching `surface` (doc 02:90 value + doc 15 memory model):
// `width * height * bytes_per_pixel(format().pixel_format)`. `Surface` exposes
// no size-in-bytes accessor, so the caller computes cost here and passes the
// result to `KeyedStore::insert(..., bytes, ...)` -- the store never
// introspects `TileValue` (keyed_store.md decision).
inline std::size_t tile_byte_cost(const Surface& surface) {
  const std::size_t w = static_cast<std::size_t>(surface.width());
  const std::size_t h = static_cast<std::size_t>(surface.height());
  return w * h * bytes_per_pixel(surface.format().pixel_format);
}

// The concrete tile cache the compositor (L4) plans requests against. The
// audio-engine analogously instantiates `KeyedStore<BlockKey, ...>` over its
// own block value when the block pipeline lands (doc 12; not this task).
using TileCache = KeyedStore<TileKey, TileValue>;

namespace detail {

// Boost-style 64-bit hash mixer. Combines an already-accumulated `seed` with
// the next field's hash so each field's contribution reaches the result;
// distinct from the keyed_store detail namespace's contents.
inline std::size_t key_hash_combine(std::size_t seed, std::size_t value) noexcept {
  return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
}

} // namespace detail

} // namespace arbc

template <> struct std::hash<arbc::TileKey> {
  std::size_t operator()(const arbc::TileKey& key) const noexcept {
    std::size_t h = std::hash<arbc::ObjectId>{}(key.content);
    h = arbc::detail::key_hash_combine(h, std::hash<std::uint64_t>{}(key.revision));
    h = arbc::detail::key_hash_combine(h, std::hash<std::int32_t>{}(key.rung.index));
    h = arbc::detail::key_hash_combine(h, std::hash<std::int32_t>{}(key.coord.col));
    h = arbc::detail::key_hash_combine(h, std::hash<std::int32_t>{}(key.coord.row));
    // Distinct combine for present vs absent so a Static key (nullopt) never
    // hash-collides with a Timed key at `flicks == 0` (doc 11:139-140).
    if (key.achieved_time.has_value()) {
      h = arbc::detail::key_hash_combine(h, std::hash<std::int64_t>{}(key.achieved_time->flicks));
      h = arbc::detail::key_hash_combine(h, 0x9ddfea08eb382d69ULL);
    } else {
      h = arbc::detail::key_hash_combine(h, 0x2545f4914f6cdd1dULL);
    }
    return h;
  }
};

template <> struct std::hash<arbc::BlockKey> {
  std::size_t operator()(const arbc::BlockKey& key) const noexcept {
    std::size_t h = std::hash<arbc::ObjectId>{}(key.content);
    h = arbc::detail::key_hash_combine(h, std::hash<std::uint64_t>{}(key.revision));
    h = arbc::detail::key_hash_combine(h, std::hash<std::int64_t>{}(key.block_index));
    h = arbc::detail::key_hash_combine(h, std::hash<std::uint32_t>{}(key.rate));
    // The opaque spatial-context digest (doc 12:249-254). Folded uniformly (Flat's
    // `0` included) so that equal keys hash equal and a digest-only difference
    // disperses to a different bucket; the raw hash value is not itself goldened.
    h = arbc::detail::key_hash_combine(h, std::hash<std::uint64_t>{}(key.spatial_digest));
    return h;
  }
};
