#include <arbc/kind_raster/raster_content.hpp>

#include <arbc/media/pixel_format.hpp>
#include <arbc/surface/typed_span.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
    out[static_cast<std::size_t>(i)] = Traits::decode(p + static_cast<std::size_t>(i) * Traits::channels);
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

WorkingPixel level_pixel(const Level& lvl, int edge, int x, int y) {
  const int tx = x / edge;
  const int ty = y / edge;
  const int ix = x % edge;
  const int iy = y % edge;
  const TileBlob& blob = *lvl.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(lvl.tiles_x) +
                                   static_cast<std::size_t>(tx)];
  const std::size_t o =
      (static_cast<std::size_t>(iy) * static_cast<std::size_t>(edge) + static_cast<std::size_t>(ix)) *
      k_tile_channels;
  return {blob.px[o], blob.px[o + 1], blob.px[o + 2], blob.px[o + 3]};
}

// A fresh transparent tile blob (edge*edge*4 zeros).
std::shared_ptr<TileBlob> new_blob(int edge, std::uint64_t& alloc) {
  ++alloc;
  auto blob = std::make_shared<TileBlob>();
  blob->px.assign(static_cast<std::size_t>(edge) * static_cast<std::size_t>(edge) * k_tile_channels, 0.0F);
  return blob;
}

void put(std::shared_ptr<TileBlob>& blob, int edge, int ix, int iy, const WorkingPixel& c) {
  const std::size_t o =
      (static_cast<std::size_t>(iy) * static_cast<std::size_t>(edge) + static_cast<std::size_t>(ix)) *
      k_tile_channels;
  blob->px[o] = c[0];
  blob->px[o + 1] = c[1];
  blob->px[o + 2] = c[2];
  blob->px[o + 3] = c[3];
}

// Build level 0 from the decoded grid, then a deterministic 2x box-downsample
// chain down to a single 1x1 pixel (doc 14:219 scale rungs).
std::vector<Level> build_levels(const std::vector<WorkingPixel>& grid, int w, int h, int edge,
                                std::uint64_t& alloc) {
  std::vector<Level> levels;

  // Level 0.
  {
    Level l0;
    l0.width = w;
    l0.height = h;
    l0.tiles_x = tiles_across(w, edge);
    l0.tiles_y = tiles_across(h, edge);
    l0.tiles.resize(static_cast<std::size_t>(l0.tiles_x) * static_cast<std::size_t>(l0.tiles_y));
    for (int ty = 0; ty < l0.tiles_y; ++ty) {
      for (int tx = 0; tx < l0.tiles_x; ++tx) {
        auto blob = new_blob(edge, alloc);
        for (int iy = 0; iy < edge; ++iy) {
          for (int ix = 0; ix < edge; ++ix) {
            const int gx = tx * edge + ix;
            const int gy = ty * edge + iy;
            if (gx < w && gy < h) {
              put(blob, edge, ix, iy,
                  grid[static_cast<std::size_t>(gy) * static_cast<std::size_t>(w) +
                       static_cast<std::size_t>(gx)]);
            }
          }
        }
        l0.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(l0.tiles_x) +
                 static_cast<std::size_t>(tx)] = std::move(blob);
      }
    }
    levels.push_back(std::move(l0));
  }

  // Higher levels: 2x2 box average of the level below, clamping at odd edges.
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
        auto blob = new_blob(edge, alloc);
        for (int iy = 0; iy < edge; ++iy) {
          for (int ix = 0; ix < edge; ++ix) {
            const int px = tx * edge + ix;
            const int py = ty * edge + iy;
            if (px < up.width && py < up.height) {
              const int cx0 = std::min(2 * px, child.width - 1);
              const int cy0 = std::min(2 * py, child.height - 1);
              const int cx1 = std::min(2 * px + 1, child.width - 1);
              const int cy1 = std::min(2 * py + 1, child.height - 1);
              const WorkingPixel a = level_pixel(child, edge, cx0, cy0);
              const WorkingPixel b = level_pixel(child, edge, cx1, cy0);
              const WorkingPixel c = level_pixel(child, edge, cx0, cy1);
              const WorkingPixel d = level_pixel(child, edge, cx1, cy1);
              const WorkingPixel avg{(a[0] + b[0] + c[0] + d[0]) * 0.25F,
                                     (a[1] + b[1] + c[1] + d[1]) * 0.25F,
                                     (a[2] + b[2] + c[2] + d[2]) * 0.25F,
                                     (a[3] + b[3] + c[3] + d[3]) * 0.25F};
              put(blob, edge, ix, iy, avg);
            }
          }
        }
        up.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(up.tiles_x) +
                 static_cast<std::size_t>(tx)] = std::move(blob);
      }
    }
    levels.push_back(std::move(up));
  }
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

WorkingPixel bilerp(const WorkingPixel& a, const WorkingPixel& b, const WorkingPixel& c,
                    const WorkingPixel& d, float fx, float fy) {
  WorkingPixel out{};
  for (std::size_t k = 0; k < k_tile_channels; ++k) {
    const float top = a[k] + (b[k] - a[k]) * fx;
    const float bot = c[k] + (d[k] - c[k]) * fx;
    out[k] = top + (bot - top) * fy;
  }
  return out;
}

} // namespace

// --- TileTable --------------------------------------------------------------

WorkingPixel TileTable::pixel(std::size_t l, int x, int y) const {
  const Level& lvl = d_levels[l];
  const int cx = std::clamp(x, 0, lvl.width - 1);
  const int cy = std::clamp(y, 0, lvl.height - 1);
  return level_pixel(lvl, d_edge, cx, cy);
}

std::vector<float> TileTable::level_pixels(std::size_t l) const {
  const Level& lvl = d_levels[l];
  std::vector<float> out(static_cast<std::size_t>(lvl.width) * static_cast<std::size_t>(lvl.height) *
                         k_tile_channels);
  for (int y = 0; y < lvl.height; ++y) {
    for (int x = 0; x < lvl.width; ++x) {
      const WorkingPixel p = level_pixel(lvl, d_edge, x, y);
      const std::size_t o =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(lvl.width) +
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
    for (const TileBlobPtr& t : lvl.tiles) {
      total += t->px.size() * sizeof(float);
    }
  }
  return total;
}

// --- RasterStore ------------------------------------------------------------

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
  std::uint64_t alloc = 0;
  std::vector<Level> levels = build_levels(grid, image.width, image.height, edge, alloc);
  auto table = std::make_shared<const TileTable>(image.width, image.height, edge, std::move(levels));
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_blobs_allocated += alloc;
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

  std::uint64_t alloc = 0;

  // Copy the level list (shared_ptr copies keep untouched blobs shared).
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
        auto blob = std::make_shared<TileBlob>(*l0.tiles[static_cast<std::size_t>(ty) *
                                                             static_cast<std::size_t>(l0.tiles_x) +
                                                         static_cast<std::size_t>(tx)]);
        ++alloc;
        for (int iy = 0; iy < edge; ++iy) {
          for (int ix = 0; ix < edge; ++ix) {
            const int gx = tx * edge + ix;
            const int gy = ty * edge + iy;
            if (gx < w && gy < h && center_inside(region, gx, gy)) {
              put(blob, edge, ix, iy, color);
            }
          }
        }
        l0.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(l0.tiles_x) +
                 static_cast<std::size_t>(tx)] = std::move(blob);
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
  Rect propagate = touched;
  for (std::size_t L = 1; L < levels.size(); ++L) {
    const Level& child = levels[L - 1];
    Level& up = levels[L];
    const Rect parent_rect{propagate.x0 * 0.5, propagate.y0 * 0.5, propagate.x1 * 0.5,
                           propagate.y1 * 0.5};
    for (int ty = 0; ty < up.tiles_y; ++ty) {
      for (int tx = 0; tx < up.tiles_x; ++tx) {
        if (!tile_overlaps(tx, ty, edge, parent_rect)) {
          continue;
        }
        auto blob = new_blob(edge, alloc);
        for (int iy = 0; iy < edge; ++iy) {
          for (int ix = 0; ix < edge; ++ix) {
            const int px = tx * edge + ix;
            const int py = ty * edge + iy;
            if (px < up.width && py < up.height) {
              const int cx0 = std::min(2 * px, child.width - 1);
              const int cy0 = std::min(2 * py, child.height - 1);
              const int cx1 = std::min(2 * px + 1, child.width - 1);
              const int cy1 = std::min(2 * py + 1, child.height - 1);
              const WorkingPixel a = level_pixel(child, edge, cx0, cy0);
              const WorkingPixel b = level_pixel(child, edge, cx1, cy0);
              const WorkingPixel c = level_pixel(child, edge, cx0, cy1);
              const WorkingPixel d = level_pixel(child, edge, cx1, cy1);
              const WorkingPixel avg{(a[0] + b[0] + c[0] + d[0]) * 0.25F,
                                     (a[1] + b[1] + c[1] + d[1]) * 0.25F,
                                     (a[2] + b[2] + c[2] + d[2]) * 0.25F,
                                     (a[3] + b[3] + c[3] + d[3]) * 0.25F};
              put(blob, edge, ix, iy, avg);
            }
          }
        }
        up.tiles[static_cast<std::size_t>(ty) * static_cast<std::size_t>(up.tiles_x) +
                 static_cast<std::size_t>(tx)] = std::move(blob);
      }
    }
    propagate = parent_rect;
  }

  auto table = std::make_shared<const TileTable>(w, h, edge, std::move(levels));
  {
    std::lock_guard<std::mutex> lock(d_mutex);
    d_blobs_allocated += alloc;
  }
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
    // refcount here (doc 14:173-176). The base table keeps its own pin.
    v.table.reset();
    d_free.push_back(handle.slot);
  }
}

std::size_t RasterStore::state_cost(StateHandle handle) const {
  TileTablePtr t = resolve(handle);
  return t ? t->byte_cost() : 0;
}

std::uint64_t RasterStore::blobs_allocated() const {
  std::lock_guard<std::mutex> lock(d_mutex);
  return d_blobs_allocated;
}

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
    : d_bounds{0.0, 0.0, static_cast<double>(image.width), static_cast<double>(image.height)},
      d_edge(tile_edge) {
  d_base = d_store.build(image, tile_edge);
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
  // (bilinear-upsampling past native); a BestEffort request clamps at native and
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
        sample = bilerp(table->pixel(l, x0, y0), table->pixel(l, x0 + 1, y0),
                        table->pixel(l, x0, y0 + 1), table->pixel(l, x0 + 1, y0 + 1), fx, fy);
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
