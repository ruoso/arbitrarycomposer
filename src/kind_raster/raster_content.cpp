#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/image_resampler.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/pool/big_block_pool.hpp>
#include <arbc/surface/typed_span.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

namespace arbc {
namespace {

// --- decode the codec-free input buffer to working-linear premultiplied floats

template <PixelFormat F>
void decode_into(const std::vector<std::byte>& bytes, int count, std::vector<WorkingPixel>& out) {
  using Traits = PixelTraits<F>;
  using Storage = typename Traits::Storage;
  const auto* p = reinterpret_cast<const Storage*>(bytes.data());
  for (int i = 0; i < count; ++i) {
    out[static_cast<std::size_t>(i)] =
        Traits::decode(p + static_cast<std::size_t>(i) * Traits::channels);
  }
}

std::vector<WorkingPixel> decode_image(const DecodedImage& image) {
  const int count = image.width * image.height;
  std::vector<WorkingPixel> out(static_cast<std::size_t>(std::max(0, count)));
  switch (image.format.pixel_format) {
  case PixelFormat::Rgba32fLinearPremul:
    decode_into<PixelFormat::Rgba32fLinearPremul>(image.bytes, count, out);
    break;
  case PixelFormat::Rgba16fLinearPremul:
    decode_into<PixelFormat::Rgba16fLinearPremul>(image.bytes, count, out);
    break;
  case PixelFormat::Rgba8Srgb:
    decode_into<PixelFormat::Rgba8Srgb>(image.bytes, count, out);
    break;
  }
  return out;
}

// --- tiling / level helpers -------------------------------------------------

int tiles_across(int extent, int edge) { return (extent + edge - 1) / edge; }

// The logical byte size of one `edge*edge` working-float tile blob. For the
// default 256-edge tile this is exactly 1 MiB -- an exact pool size-class rung
// (doc 15:19-20, `big_block_pool.md`).
std::size_t blob_bytes(int edge) {
  return static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge) * k_tile_channels *
         sizeof(float);
}

// Read the working pixel at level-logical (x, y) through the pool: locate the
// owning tile blob and `peek` it (zero-refcount, immutable-after-fill read).
WorkingPixel level_pixel(const Level& lvl, const BigBlockPool& pool, int edge, int x, int y) {
  const int tx = x / edge;
  const int ty = y / edge;
  const int ix = x % edge;
  const int iy = y % edge;
  const BlockSlotRef ref =
      lvl.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(lvl.tiles_x) +
                static_cast<std::size_t>(tx)];
  const std::span<const std::byte> raw = pool.peek(ref);
  const auto* px = reinterpret_cast<const float*>(raw.data());
  const std::size_t o = (static_cast<std::size_t>(iy) * static_cast<std::size_t>(edge) +
                         static_cast<std::size_t>(ix)) *
                        k_tile_channels;
  return {px[o], px[o + 1], px[o + 2], px[o + 3]};
}

// Allocate a fresh transparent tile blob from the pool (edge*edge*4 zeros). The
// returned owning `BlockRef` keeps the allocate-count alive until the caller has
// handed the blob's `BlockSlotRef` to a TileTable (whose ctor retains it); the
// caller drops the BlockRef afterward so the table holds the sole count.
BlockRef new_blob(BigBlockPool& pool, int edge) {
  BlockRef ref = *pool.allocate(blob_bytes(edge));
  std::memset(ref.data(), 0, ref.size());
  return ref;
}

void put(float* px, int edge, int ix, int iy, const WorkingPixel& c) {
  const std::size_t o = (static_cast<std::size_t>(iy) * static_cast<std::size_t>(edge) +
                         static_cast<std::size_t>(ix)) *
                        k_tile_channels;
  px[o] = c[0];
  px[o + 1] = c[1];
  px[o + 2] = c[2];
  px[o + 3] = c[3];
}

// The ONE decimation kernel of the pyramid: parent pixel (cpx, cpy) of the rung
// above `child`, through media's frozen 6-tap Lanczos-3 half-band bank. Both the
// full pyramid build and the paint-time incremental recompute go through this --
// the box kernel they replace was copy-pasted between them, and two divergent
// copies of a byte-exact filter is the defect this must not recreate.
//
// Taps outside the child level CLAMP TO EDGE, matching `TileTable::pixel`'s
// convention (a zero surround would darken every level border instead).
WorkingPixel decimate_parent(const Level& child, const BigBlockPool& pool, int edge, int cpx,
                             int cpy) {
  return decimate_half_band(cpx, cpy, [&](int x, int y) {
    return level_pixel(child, pool, edge, std::clamp(x, 0, child.width - 1),
                       std::clamp(y, 0, child.height - 1));
  });
}

// An empty level-0 grid of the geometry `w x h` at `edge` implies: the tile counts and
// the (as yet unfilled) `BlockSlotRef` row-major table. Shared by both level-0 routes so
// neither can drift on tile ordering -- `tiles[ty * tiles_x + tx]` is the flat index the
// codec's `blobs` array and its hash memo are both positionally keyed on.
Level level0_shape(int w, int h, int edge) {
  Level l0;
  l0.width = w;
  l0.height = h;
  l0.tiles_x = tiles_across(w, edge);
  l0.tiles_y = tiles_across(h, edge);
  l0.tiles.resize(static_cast<std::size_t>(l0.tiles_x) * static_cast<std::size_t>(l0.tiles_y));
  return l0;
}

// Level 0 from a dense decoded grid: one fresh blob per tile, in-bounds pixels copied in
// and the right/bottom padding left at `new_blob`'s zeros.
Level build_level0_from_grid(const std::vector<WorkingPixel>& grid, int w, int h, int edge,
                             BigBlockPool& pool, std::vector<BlockRef>& keep) {
  Level l0 = level0_shape(w, h, edge);
  for (int ty = 0; ty < l0.tiles_y; ++ty) {
    for (int tx = 0; tx < l0.tiles_x; ++tx) {
      BlockRef blob = new_blob(pool, edge);
      auto* px = reinterpret_cast<float*>(blob.data());
      for (int iy = 0; iy < edge; ++iy) {
        for (int ix = 0; ix < edge; ++ix) {
          const int gx = tx * edge + ix;
          const int gy = ty * edge + iy;
          if (gx < w && gy < h) {
            put(px, edge, ix, iy,
                grid[static_cast<std::size_t>(gy) * static_cast<std::size_t>(w) +
                     static_cast<std::size_t>(gx)]);
          }
        }
      }
      l0.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(l0.tiles_x) +
               static_cast<std::size_t>(tx)] = blob.slot();
      keep.push_back(std::move(blob));
    }
  }
  return l0;
}

// Level 0 filled TILEWISE (kinds.raster_tilewise_load): the caller writes each tile
// straight into its pool blob's own memory, so exactly ONE tile's pixels are live at a
// time and no dense `w*h` buffer exists at any point. The blob is handed over WHOLESALE
// -- all `edge*edge*4` samples including the padding -- because a persisted tile's
// padding is inside its content hash (Decision 3).
//
// `false` on a declined fill. The in-flight blob dies with this frame and the `keep`
// vector the caller owns drops the rest, so an abandoned build strands nothing: the pool
// reclaims every slot by refcount and no TileTable is ever constructed.
bool build_level0_from_fill(const TileFill& fill, int w, int h, int edge, BigBlockPool& pool,
                            std::vector<BlockRef>& keep, Level& out) {
  Level l0 = level0_shape(w, h, edge);
  const std::size_t samples =
      static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge) * k_tile_channels;
  for (std::size_t t = 0; t < l0.tiles.size(); ++t) {
    BlockRef blob = new_blob(pool, edge);
    auto* px = reinterpret_cast<float*>(blob.data());
    if (!fill(t, std::span<float>{px, samples})) {
      return false;
    }
    l0.tiles[t] = blob.slot();
    keep.push_back(std::move(blob));
  }
  out = std::move(l0);
  return true;
}

// The pyramid ABOVE level 0: a deterministic 2:1 Lanczos-3 half-band decimation chain
// down to a single 1x1 pixel (doc 14:219 scale rungs). Both build routes -- the dense one
// and the tilewise one -- call THIS, unmoved. Copy-pasting it for the second route is the
// defect `decimate_parent`'s comment above already records: two divergent copies of a
// byte-exact filter is exactly what
// `14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild` and
// `08-serialization#raster-mips-are-not-persisted` both rest on there NOT being.
void append_higher_levels(std::vector<Level>& levels, int edge, BigBlockPool& pool,
                          std::vector<BlockRef>& keep) {
  while (levels.back().width > 1 || levels.back().height > 1) {
    const Level& child = levels.back();
    Level up;
    up.width = std::max(1, (child.width + 1) / 2);
    up.height = std::max(1, (child.height + 1) / 2);
    up.tiles_x = tiles_across(up.width, edge);
    up.tiles_y = tiles_across(up.height, edge);
    up.tiles.resize(static_cast<std::size_t>(up.tiles_x) * static_cast<std::size_t>(up.tiles_y));
    for (int ty = 0; ty < up.tiles_y; ++ty) {
      for (int tx = 0; tx < up.tiles_x; ++tx) {
        BlockRef blob = new_blob(pool, edge);
        auto* px = reinterpret_cast<float*>(blob.data());
        for (int iy = 0; iy < edge; ++iy) {
          for (int ix = 0; ix < edge; ++ix) {
            const int cpx = tx * edge + ix;
            const int cpy = ty * edge + iy;
            if (cpx < up.width && cpy < up.height) {
              put(px, edge, ix, iy, decimate_parent(child, pool, edge, cpx, cpy));
            }
          }
        }
        up.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(up.tiles_x) +
                 static_cast<std::size_t>(tx)] = blob.slot();
        keep.push_back(std::move(blob));
      }
    }
    levels.push_back(std::move(up));
  }
}

// Fresh blobs are appended to `keep` (owning BlockRefs) so their allocate-counts survive
// until the TileTable that will own them is constructed.
std::vector<Level> build_levels(const std::vector<WorkingPixel>& grid, int w, int h, int edge,
                                BigBlockPool& pool, std::vector<BlockRef>& keep) {
  std::vector<Level> levels;
  levels.push_back(build_level0_from_grid(grid, w, h, edge, pool, keep));
  append_higher_levels(levels, edge, pool, keep);
  return levels;
}

bool center_inside(const Rect& r, int x, int y) {
  const double cx = static_cast<double>(x) + 0.5;
  const double cy = static_cast<double>(y) + 0.5;
  return cx >= r.x0 && cx < r.x1 && cy >= r.y0 && cy < r.y1;
}

bool tile_overlaps(int tx, int ty, int edge, const Rect& region) {
  const Rect tile{static_cast<double>(tx * edge), static_cast<double>(ty * edge),
                  static_cast<double>((tx + 1) * edge), static_cast<double>((ty + 1) * edge)};
  return !tile.intersect(region).empty();
}

int level_for_scale(double achieved, int max_level) {
  if (achieved >= 1.0) {
    return 0;
  }
  int level = 0;
  double t = 1.0 / achieved;
  while (t >= 2.0 - 1e-9 && level < max_level) {
    t *= 0.5;
    ++level;
  }
  return level;
}

} // namespace

// --- TileTable --------------------------------------------------------------

WorkingPixel TileTable::pixel(std::size_t l, int x, int y) const {
  const Level& lvl = d_levels[l];
  const int cx = std::clamp(x, 0, lvl.width - 1);
  const int cy = std::clamp(y, 0, lvl.height - 1);
  return level_pixel(lvl, *d_pool, d_edge, cx, cy);
}

std::vector<float> TileTable::level_pixels(std::size_t l) const {
  const Level& lvl = d_levels[l];
  std::vector<float> out(static_cast<std::size_t>(lvl.width) *
                         static_cast<std::size_t>(lvl.height) * k_tile_channels);
  for (int y = 0; y < lvl.height; ++y) {
    for (int x = 0; x < lvl.width; ++x) {
      const WorkingPixel p = level_pixel(lvl, *d_pool, d_edge, x, y);
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(lvl.width) +
                             static_cast<std::size_t>(x)) *
                            k_tile_channels;
      out[o] = p[0];
      out[o + 1] = p[1];
      out[o + 2] = p[2];
      out[o + 3] = p[3];
    }
  }
  return out;
}

std::size_t TileTable::byte_cost() const {
  std::size_t total = 0;
  for (const Level& lvl : d_levels) {
    for (const BlockSlotRef& ref : lvl.tiles) {
      total += ref.size();
    }
  }
  return total;
}

// --- RasterStore ------------------------------------------------------------

RasterStore::RasterStore()
    : d_owned_source(std::make_unique<AnonymousChunkSource>()), d_pool(*d_owned_source) {}

RasterStore::RasterStore(ChunkSource& source) : d_owned_source(nullptr), d_pool(source) {}

StateHandle RasterStore::intern(TileTablePtr table) {
  std::lock_guard<std::mutex> lock(d_mutex);
  SlotIndex slot;
  if (!d_free.empty()) {
    slot = d_free.back();
    d_free.pop_back();
    d_versions[slot] = Version{std::move(table), 0};
  } else {
    slot = static_cast<SlotIndex>(d_versions.size());
    d_versions.push_back(Version{std::move(table), 0});
  }
  return StateHandle{slot};
}

StateHandle RasterStore::build(const DecodedImage& image, int edge) {
  const std::vector<WorkingPixel> grid = decode_image(image);
  // `keep` holds the fresh blobs' allocate-counts alive until the TileTable
  // constructor retains each ref; it drops at scope exit so the table owns the
  // sole count on every fresh blob.
  std::vector<BlockRef> keep;
  std::vector<Level> levels = build_levels(grid, image.width, image.height, edge, d_pool, keep);
  auto table = std::make_shared<const TileTable>(image.width, image.height, edge, std::move(levels),
                                                 &d_pool);
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_base_table = table;
  }
  return intern(std::move(table));
}

std::optional<StateHandle> RasterStore::build_from_tiles(int width, int height, int edge,
                                                         const TileFill& fill) {
  // `keep` is the whole abandoned-build story (Constraint 5): it holds the fresh blobs'
  // allocate-counts until the TileTable ctor retains them, and on a declined fill it goes
  // out of scope with NO table ever built -- so every blob the partial build allocated
  // drops to count zero and its slot returns to the pool's class free list.
  std::vector<BlockRef> keep;
  std::vector<Level> levels;
  Level l0;
  if (!build_level0_from_fill(fill, width, height, edge, d_pool, keep, l0)) {
    return std::nullopt;
  }
  levels.push_back(std::move(l0));
  append_higher_levels(levels, edge, d_pool, keep);

  auto table = std::make_shared<const TileTable>(width, height, edge, std::move(levels), &d_pool);
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_base_table = table;
  }
  return intern(std::move(table));
}

TileTablePtr RasterStore::resolve(StateHandle handle) const {
  if (!handle.has_state()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(d_mutex);
  if (handle.slot >= d_versions.size()) {
    return nullptr;
  }
  return d_versions[handle.slot].table;
}

TileTablePtr RasterStore::base_table() const {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_base_table;
}

void RasterStore::set_base(TileTablePtr table) {
  std::lock_guard<std::mutex> lock(d_mutex);
  d_base_table = std::move(table);
}

StateHandle RasterStore::paint(StateHandle base, const Rect& region, const WorkingPixel& color,
                               Rect& touched_out) {
  TileTablePtr baseTable = resolve(base);
  if (!baseTable) {
    baseTable = base_table();
  }
  const int edge = baseTable->edge();
  const int w = baseTable->width();
  const int h = baseTable->height();

  // `keep` holds fresh blobs' allocate-counts until the new TileTable retains
  // them (see build()).
  std::vector<BlockRef> keep;

  // Copy the level list. BlockSlotRef copies are position-independent bit-copies
  // that carry no count; the new TileTable ctor retains every ref, so untouched
  // (shared) tiles gain a count against the new version while fresh tiles are
  // held alive by `keep` until then.
  std::vector<Level> levels = baseTable->levels();

  // Level 0: replace only the tiles the region touches.
  Rect touched{};
  {
    Level& l0 = levels[0];
    for (int ty = 0; ty < l0.tiles_y; ++ty) {
      for (int tx = 0; tx < l0.tiles_x; ++tx) {
        if (!tile_overlaps(tx, ty, edge, region)) {
          continue;
        }
        const std::size_t idx =
            static_cast<std::size_t>(ty) * static_cast<std::size_t>(l0.tiles_x) +
            static_cast<std::size_t>(tx);
        BlockRef blob = new_blob(d_pool, edge);
        // Copy the predecessor's blob bytes (including tile padding) so untouched
        // pixels stay byte-identical, then mutate only the touched pixels.
        const std::span<const std::byte> src = d_pool.peek(l0.tiles[idx]);
        std::memcpy(blob.data(), src.data(), blob.size());
        auto* px = reinterpret_cast<float*>(blob.data());
        for (int iy = 0; iy < edge; ++iy) {
          for (int ix = 0; ix < edge; ++ix) {
            const int gx = tx * edge + ix;
            const int gy = ty * edge + iy;
            if (gx < w && gy < h && center_inside(region, gx, gy)) {
              put(px, edge, ix, iy, color);
            }
          }
        }
        l0.tiles[idx] = blob.slot();
        keep.push_back(std::move(blob));
        const Rect tile{static_cast<double>(tx * edge), static_cast<double>(ty * edge),
                        std::min(static_cast<double>((tx + 1) * edge), static_cast<double>(w)),
                        std::min(static_cast<double>((ty + 1) * edge), static_cast<double>(h))};
        touched = rect_union(touched, tile);
      }
    }
  }
  touched_out = touched;

  // Higher levels: recompute only the parent tiles above the touched region,
  // reading the already-updated child level (geometric sum above the region).
  //
  // The decimation kernel's support is WIDER than the 2 child pixels it reduces
  // (6 taps: child `2x-2 .. 2x+3`), so a parent pixel is dirtied by any changed
  // child pixel within `k_decimate_radius` of its 2:1 footprint. The propagated
  // region must therefore be DILATED by that radius at each rung before selecting
  // parent tiles. Left undilated (the exact `touched * 0.5` a box filter allowed),
  // a paint leaves a stale filtered band around the touched region -- a silent,
  // zoom-dependent corruption. Dilating keeps the incremental recompute
  // indistinguishable from a full rebuild (doc 14); over-covering is free, since a
  // recomputed parent pixel whose support saw no change recomputes to itself.
  //
  // An empty touch propagates nothing: dilating an empty rect would manufacture a
  // non-empty one and rebuild rungs no paint dirtied.
  if (!touched.empty()) {
    constexpr double k_dilate = static_cast<double>(k_decimate_radius);
    Rect propagate = touched;
    for (std::size_t L = 1; L < levels.size(); ++L) {
      const Level& child = levels[L - 1];
      Level& up = levels[L];
      const Rect parent_rect{
          std::floor((propagate.x0 - k_dilate) * 0.5), std::floor((propagate.y0 - k_dilate) * 0.5),
          std::ceil((propagate.x1 + k_dilate) * 0.5), std::ceil((propagate.y1 + k_dilate) * 0.5)};
      for (int ty = 0; ty < up.tiles_y; ++ty) {
        for (int tx = 0; tx < up.tiles_x; ++tx) {
          if (!tile_overlaps(tx, ty, edge, parent_rect)) {
            continue;
          }
          const std::size_t idx =
              static_cast<std::size_t>(ty) * static_cast<std::size_t>(up.tiles_x) +
              static_cast<std::size_t>(tx);
          BlockRef blob = new_blob(d_pool, edge);
          auto* px = reinterpret_cast<float*>(blob.data());
          for (int iy = 0; iy < edge; ++iy) {
            for (int ix = 0; ix < edge; ++ix) {
              const int cpx = tx * edge + ix;
              const int cpy = ty * edge + iy;
              if (cpx < up.width && cpy < up.height) {
                put(px, edge, ix, iy, decimate_parent(child, d_pool, edge, cpx, cpy));
              }
            }
          }
          up.tiles[idx] = blob.slot();
          keep.push_back(std::move(blob));
        }
      }
      propagate = parent_rect;
    }
  }

  auto table = std::make_shared<const TileTable>(w, h, edge, std::move(levels), &d_pool);
  return intern(std::move(table));
}

void RasterStore::retain_version(StateHandle handle) {
  if (!handle.has_state()) {
    return;
  }
  std::lock_guard<std::mutex> lock(d_mutex);
  if (handle.slot < d_versions.size() && d_versions[handle.slot].table) {
    ++d_versions[handle.slot].refcount;
  }
}

void RasterStore::release_version(StateHandle handle) {
  if (!handle.has_state()) {
    return;
  }
  std::lock_guard<std::mutex> lock(d_mutex);
  if (handle.slot >= d_versions.size()) {
    return;
  }
  Version& v = d_versions[handle.slot];
  if (v.refcount == 0) {
    return;
  }
  if (--v.refcount == 0) {
    // Drop the version's blobs; a no-longer-shared tile-pixel blob reclaims by
    // the pool refcount here (doc 14:173-176). The base table keeps its own pin.
    v.table.reset();
    d_free.push_back(handle.slot);
  }
}

std::size_t RasterStore::state_cost(StateHandle handle) const {
  TileTablePtr t = resolve(handle);
  return t ? t->byte_cost() : 0;
}

std::uint64_t RasterStore::blobs_allocated() const { return d_pool.blobs_allocated(); }

std::size_t RasterStore::live_versions() const {
  std::lock_guard<std::mutex> lock(d_mutex);
  std::size_t n = 0;
  for (const Version& v : d_versions) {
    if (v.table) {
      ++n;
    }
  }
  return n;
}

std::uint32_t RasterStore::version_refcount(StateHandle handle) const {
  std::lock_guard<std::mutex> lock(d_mutex);
  if (!handle.has_state() || handle.slot >= d_versions.size()) {
    return 0;
  }
  return d_versions[handle.slot].refcount;
}

// --- RasterContent ----------------------------------------------------------

RasterContent::RasterContent(DecodedImage image, int tile_edge)
    : d_bounds{0.0, 0.0, static_cast<double>(image.width), static_cast<double>(image.height)} {
  d_base = d_store.build(image, tile_edge);
}

RasterContent::RasterContent(int width, int height, int tile_edge, const TileFill& fill, bool& ok)
    : d_bounds{0.0, 0.0, static_cast<double>(width), static_cast<double>(height)} {
  const std::optional<StateHandle> built = d_store.build_from_tiles(width, height, tile_edge, fill);
  ok = built.has_value();
  d_base = ok ? *built : StateHandle{k_state_none};
}

std::unique_ptr<RasterContent> RasterContent::from_tiles(int width, int height, int tile_edge,
                                                         const TileFill& fill) {
  bool ok = false;
  // The store is built IN PLACE inside the content: `RasterStore` is not movable and must
  // not become so -- it owns a BigBlockPool, a mutex and a ChunkSource, and every live
  // TileTable holds a raw `BigBlockPool*`, so moving the store would dangle every table it
  // has minted (Decision 2, rejected alternative).
  auto content =
      std::unique_ptr<RasterContent>(new RasterContent(width, height, tile_edge, fill, ok));
  if (!ok) {
    return nullptr; // a declined fill destroys the half-built content here; none escapes
  }
  return content;
}

std::optional<Rect> RasterContent::bounds() const { return d_bounds; }

Stability RasterContent::stability() const { return Stability::Static; }

std::optional<TimeRange> RasterContent::time_extent() const { return std::nullopt; }

TileTablePtr RasterContent::resolve_for_render(StateHandle snapshot) const {
  TileTablePtr t;
  if (snapshot.has_state()) {
    t = d_store.resolve(snapshot);
  }
  if (!t) {
    t = d_store.base_table();
  }
  return t;
}

std::optional<RenderResult> RasterContent::render(const RenderRequest& request,
                                                  std::shared_ptr<RenderCompletion>) {
  const TileTablePtr table = resolve_for_render(request.snapshot);
  const int max_level = static_cast<int>(table->level_count()) - 1;
  const int w = table->width();
  const int h = table->height();

  // Bounded scale: an Exact request renders faithfully at the requested scale
  // (bicubic-upsampling past native); a BestEffort request clamps at native and
  // reports achieved_scale < request.scale honestly (doc 03:53-55,142-145,
  // refinement Constraint 5 reconciled with the enforced #render-scale-honest
  // claim: achieved < request is never exact).
  const double s = request.scale;
  const double achieved = (request.exactness == Exactness::Exact) ? s : std::min(s, 1.0);
  const bool exact = (achieved == s);

  const int tw = request.target.width();
  visit_surface(request.target, [&](auto typed) {
    using Traits = PixelTraits<decltype(typed)::format>;
    for (std::size_t i = 0; i + Traits::channels <= typed.data.size(); i += Traits::channels) {
      const int idx = static_cast<int>(i / Traits::channels);
      const int dx = tw > 0 ? idx % tw : 0;
      const int dy = tw > 0 ? idx / tw : 0;
      const double lx = request.region.x0 + (static_cast<double>(dx) + 0.5) / achieved;
      const double ly = request.region.y0 + (static_cast<double>(dy) + 0.5) / achieved;
      WorkingPixel sample{0.0F, 0.0F, 0.0F, 0.0F};
      if (lx >= 0.0 && lx < static_cast<double>(w) && ly >= 0.0 && ly < static_cast<double>(h)) {
        const int level = level_for_scale(achieved, max_level);
        const double ls = static_cast<double>(1 << level);
        const double u = lx / ls - 0.5;
        const double v = ly / ls - 0.5;
        const int x0 = static_cast<int>(std::floor(u));
        const int y0 = static_cast<int>(std::floor(v));
        const auto fx = static_cast<float>(u - static_cast<double>(x0));
        const auto fy = static_cast<float>(v - static_cast<double>(y0));
        const std::size_t l = static_cast<std::size_t>(level);
        // Interpolating Catmull-Rom: at integer phase its weights are exactly
        // (0, 1, 0, 0), so an on-rung or native-scale fetch reproduces the level
        // pixel bit-for-bit (the guard the surviving goldens encode).
        // `TileTable::pixel` clamps, so the wider tap footprint reads the border,
        // never tile padding.
        sample =
            sample_bicubic(x0, y0, fx, fy, [&](int sx, int sy) { return table->pixel(l, sx, sy); });
      }
      Traits::encode(sample, &typed.data[i]);
    }
  });

  RenderResult result{achieved, exact, std::nullopt};
  return result;
}

StateHandle RasterContent::capture() { return d_base; }

void RasterContent::restore(StateHandle state) {
  TileTablePtr t = d_store.resolve(state);
  if (t) {
    d_base = state;
    d_store.set_base(std::move(t));
  }
}

std::size_t RasterContent::state_cost(StateHandle state) const { return d_store.state_cost(state); }

void RasterContent::retain(StateHandle state) { d_store.retain_version(state); }

void RasterContent::release(StateHandle state) { d_store.release_version(state); }

void RasterContent::paint(Model::Transaction& txn, ObjectId self, const Rect& region,
                          const WorkingPixel& color) {
  Rect touched{};
  const StateHandle after = d_store.paint(d_base, region, color, touched);
  txn.set_content_state(self, after);
  Damage dmg;
  dmg.object = self;
  dmg.rect = touched;
  dmg.range = TimeRange::all(); // a Static image's pixels change at every instant
  txn.add_damage(dmg);
  d_base = after;
  d_store.set_base(d_store.resolve(after));
}

} // namespace arbc
