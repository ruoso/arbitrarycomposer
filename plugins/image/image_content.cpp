#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/image_resampler.hpp> // decimate_half_band / sample_bicubic (media, L2)
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>

#include <imdec.h> // the PRIVATE decode dep (arbc-plugin-imdec), never in libarbc

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace arbc::image {

namespace {

// The pyramid rung an achieved scale reads. Identical to `kind_raster`'s
// `level_for_scale` and deliberately so: the two kinds resample the same way through the
// same `media` kernels, and a divergent rung selection would make an `org.arbc.raster`
// retouch layer sample a different mip than the `org.arbc.image` it sits above.
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

// --- Pyramid ----------------------------------------------------------------

WorkingPixel Pyramid::pixel(std::size_t level, int x, int y) const {
  const Level& lvl = d_levels[level];
  const int cx = std::clamp(x, 0, lvl.width - 1);
  const int cy = std::clamp(y, 0, lvl.height - 1);
  const std::size_t o =
      (static_cast<std::size_t>(cy) * static_cast<std::size_t>(lvl.width) +
       static_cast<std::size_t>(cx)) *
      4U;
  return {lvl.px[o], lvl.px[o + 1], lvl.px[o + 2], lvl.px[o + 3]};
}

std::size_t Pyramid::resident_bytes() const noexcept {
  std::size_t total = 0;
  for (const Level& lvl : d_levels) {
    total += lvl.px.size() * sizeof(float);
  }
  return total;
}

std::shared_ptr<const Pyramid> Pyramid::decode(std::span<const unsigned char> encoded) {
  // Absence and corruption are the SAME outcome here -- the unavailable state (Decision 7).
  // Neither is an error channel: a nullptr pyramid is a content with no pixels, and the
  // document that references it still loads (Constraint 6).
  if (encoded.empty()) {
    return nullptr;
  }
  int w = 0;
  int h = 0;
  unsigned char* rgba = imdec_load_from_memory(encoded.data(), encoded.size(), &w, &h);
  if (rgba == nullptr || w <= 0 || h <= 0) {
    imdec_free(rgba); // free(nullptr) is well-defined; a corrupt file is a value, never UB
    return nullptr;
  }

  auto pyramid = std::make_shared<Pyramid>();
  pyramid->d_width = w;
  pyramid->d_height = h;

  // Level 0: the decoded master, converted to the working space AT DECODE (the settled
  // predecessor decision, parking-lot 2026-07-12 triage): `Rgba8Srgb` -> `WorkingPixel` ->
  // `Rgba32fLinearPremul`. No foreign tag ever reaches the compositor from this kind, so
  // convert-at-composite must not be built on its account.
  {
    Level l0;
    l0.width = w;
    l0.height = h;
    l0.px.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U);
    const std::size_t pixels = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    for (std::size_t i = 0; i < pixels; ++i) {
      const WorkingPixel wp = PixelTraits<PixelFormat::Rgba8Srgb>::decode(&rgba[i * 4]);
      PixelTraits<PixelFormat::Rgba32fLinearPremul>::encode(wp, &l0.px[i * 4]);
    }
    pyramid->d_levels.push_back(std::move(l0));
  }
  imdec_free(rgba);

  // Higher rungs: a deterministic 2:1 Lanczos-3 half-band decimation chain down to 1x1,
  // through `media`'s frozen bank. Taps outside the child level clamp to edge, matching
  // `Pyramid::pixel`. Same rung arithmetic as raster's `build_levels`, so the two kinds'
  // mips agree rung-for-rung.
  while (pyramid->d_levels.back().width > 1 || pyramid->d_levels.back().height > 1) {
    const Level& child = pyramid->d_levels.back();
    const std::size_t child_index = pyramid->d_levels.size() - 1;
    Level up;
    up.width = std::max(1, (child.width + 1) / 2);
    up.height = std::max(1, (child.height + 1) / 2);
    up.px.resize(static_cast<std::size_t>(up.width) * static_cast<std::size_t>(up.height) * 4U);
    for (int y = 0; y < up.height; ++y) {
      for (int x = 0; x < up.width; ++x) {
        const WorkingPixel c = decimate_half_band(x, y, [&](int sx, int sy) {
          return pyramid->pixel(child_index, sx, sy); // clamps to edge
        });
        const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(up.width) +
                               static_cast<std::size_t>(x)) *
                              4U;
        up.px[o] = c[0];
        up.px[o + 1] = c[1];
        up.px[o + 2] = c[2];
        up.px[o + 3] = c[3];
      }
    }
    pyramid->d_levels.push_back(std::move(up));
  }
  return pyramid;
}

// --- PyramidCache -----------------------------------------------------------

PyramidPtr PyramidCache::resolve(std::string_view resolved_uri,
                                 std::span<const unsigned char> encoded) {
  // An empty resolved URI names no identity to dedup on (an absent/mistyped `params.source`).
  // Decode nothing and cache nothing: the content is simply unavailable.
  if (resolved_uri.empty()) {
    return nullptr;
  }
  const std::lock_guard<std::mutex> lock(d_mutex);
  const std::string key(resolved_uri);
  if (const auto it = d_by_uri.find(key); it != d_by_uri.end()) {
    if (PyramidPtr resident = it->second.lock()) {
      return resident; // the dedup: one decode per RESOLVED identity (doc 08:116-122)
    }
    d_by_uri.erase(it); // the last content holding it died; the weak entry is dead
  }
  PyramidPtr fresh = Pyramid::decode(encoded);
  if (!fresh) {
    return nullptr; // undecodable: unavailable, and nothing to remember
  }
  ++d_decodes_issued;
  d_by_uri.emplace(key, fresh);
  return fresh;
}

std::uint64_t PyramidCache::decodes_issued() const noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_decodes_issued;
}

PyramidCache& default_pyramid_cache() {
  static PyramidCache cache;
  return cache;
}

// --- TileSurface / TileStore ------------------------------------------------

TileSurface::TileSurface(int width, int height)
    : d_width(width), d_height(height),
      d_data(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
             bytes_per_pixel(d_format.pixel_format)) {}

std::shared_ptr<TileSurface> TileStore::acquire(int width, int height) {
  {
    const std::lock_guard<std::mutex> lock(d_mutex);
    for (auto it = d_free.begin(); it != d_free.end(); ++it) {
      if ((*it)->width() == width && (*it)->height() == height) {
        std::shared_ptr<TileSurface> reused = std::move(*it);
        d_free.erase(it);
        return reused;
      }
    }
    ++d_allocations;
  }
  return std::make_shared<TileSurface>(width, height);
}

void TileStore::release(std::shared_ptr<TileSurface> surface) {
  const std::lock_guard<std::mutex> lock(d_mutex);
  if (d_free.size() < k_max_resting) {
    d_free.push_back(std::move(surface));
  }
  // Beyond the cap the surplus surface is simply dropped -- the free list is an
  // allocation damper, not a pool with a residency contract.
}

std::uint64_t TileStore::allocations() const noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_allocations;
}

// --- ImageContent -----------------------------------------------------------

ImageContent::ImageContent(std::string authored_uri, PyramidPtr pyramid)
    : d_uri(std::move(authored_uri)), d_pyramid(std::move(pyramid)),
      d_tiles(std::make_shared<TileStore>()) {}

std::optional<Rect> ImageContent::bounds() const {
  if (!d_pyramid) {
    return Rect{}; // EMPTY, not nullopt: nullopt means UNBOUNDED (doc 03:76-78)
  }
  return Rect{0.0, 0.0, static_cast<double>(d_pyramid->width()),
              static_cast<double>(d_pyramid->height())};
}

std::optional<RenderResult> ImageContent::render(const RenderRequest& request,
                                                 std::shared_ptr<RenderCompletion> done) {
  if (!d_pyramid) {
    // Unavailable: no pixels to answer with. A value, never UB and never a throw
    // (Constraint 7). In practice the empty `bounds()` culls this content before the
    // compositor ever asks.
    if (done) {
      done->fail(RenderError::ResourceUnavailable);
    }
    return std::nullopt;
  }

  // Bounded scale, honestly reported (the raster rule, `raster_content.cpp:508-514`): an
  // `Exact` request renders faithfully at the requested scale, bicubic-magnifying past
  // native; a `BestEffort` request CLAMPS AT NATIVE and says `achieved_scale <
  // request.scale`, which makes `exact` false. `achieved < requested` is never exact.
  const double s = request.scale;
  const double achieved = (request.exactness == Exactness::Exact) ? s : std::min(s, 1.0);
  const bool exact = (achieved == s);

  const int w = d_pyramid->width();
  const int h = d_pyramid->height();
  const auto max_level = static_cast<int>(d_pyramid->level_count()) - 1;

  // Decision 1: the provided surface covers EXACTLY THE REQUESTED REGION AT THE ACHIEVED
  // SCALE (doc 09:157-160) -- the target's extent -- never the whole decoded frame. This is
  // the constraint that bounds every cache copy to one tile: handing back a 24 MP master
  // would make each of the ~100+ tile pulls copy ~384 MB into the cache.
  const int tw = request.target.width();
  const int th = request.target.height();
  const std::shared_ptr<TileStore> store = d_tiles;
  const std::shared_ptr<TileSurface> tile = store->acquire(tw, th);

  const std::span<float> dst = tile->span<PixelFormat::Rgba32fLinearPremul>();
  for (int dy = 0; dy < th; ++dy) {
    for (int dx = 0; dx < tw; ++dx) {
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
        const auto l = static_cast<std::size_t>(level);
        // Interpolating Catmull-Rom: at integer phase its weights are exactly (0, 1, 0, 0),
        // so an on-rung or native-scale fetch reproduces the level pixel BIT-FOR-BIT.
        sample = sample_bicubic(x0, y0, fx, fy,
                                [&](int sx, int sy) { return d_pyramid->pixel(l, sx, sy); });
      }
      const std::size_t o = (static_cast<std::size_t>(dy) * static_cast<std::size_t>(tw) +
                             static_cast<std::size_t>(dx)) *
                            4U;
      PixelTraits<PixelFormat::Rgba32fLinearPremul>::encode(sample, &dst[o]);
    }
  }

  RenderResult result;
  result.achieved_scale = achieved;
  result.exact = exact;
  // achieved_time stays nullopt: Static content contributes no time dimension to the cache
  // key (doc 11:138-143, Constraint 3).

  // Non-transient and refcounted (doc 09:176-182): the compositor may composite from it
  // INLINE (zero copy) or copy it into cache, and it is pinned until it has. The release
  // callback captures BOTH the store and the surface, so the free list outlives this
  // content whenever a `SurfaceRef` does. The surface carries `k_working_rgba32f` -- the
  // composition working-space tag the CPU backend asserts on (doc 09:219-230).
  result.provided.emplace(
      *tile, [store, tile]() { store->release(tile); }, /*transient=*/false);
  return result;
}

// --- factory ----------------------------------------------------------------

std::string image_config(std::string_view authored_uri, std::string_view resolved_uri,
                         std::string_view encoded_bytes) {
  std::string config;
  config.reserve(authored_uri.size() + resolved_uri.size() + encoded_bytes.size() + 2);
  config.append(authored_uri);
  config.push_back('\n');
  config.append(resolved_uri);
  config.push_back('\n');
  config.append(encoded_bytes);
  return config;
}

expected<std::unique_ptr<Content>, std::string> make_image_content(ContentConfig config) {
  const std::string_view frame(config);
  const std::size_t first = frame.find('\n');
  if (first == std::string_view::npos) {
    return unexpected<std::string>("image: malformed config (no authored-URI delimiter)");
  }
  const std::size_t second = frame.find('\n', first + 1);
  if (second == std::string_view::npos) {
    return unexpected<std::string>("image: malformed config (no resolved-URI delimiter)");
  }
  const std::string_view authored = frame.substr(0, first);
  const std::string_view resolved = frame.substr(first + 1, second - first - 1);
  const std::string_view bytes = frame.substr(second + 1);

  // Empty bytes == absence (`load_context.hpp:35-38`), and absence is the unavailable
  // state, NOT an error: the URI is kept verbatim, the content reports empty bounds, and
  // the document loads (Constraint 6, Decision 7).
  const std::span<const unsigned char> encoded(reinterpret_cast<const unsigned char*>(bytes.data()),
                                               bytes.size());
  PyramidPtr pyramid = default_pyramid_cache().resolve(resolved, encoded);
  return std::unique_ptr<Content>(
      std::make_unique<ImageContent>(std::string(authored), std::move(pyramid)));
}

} // namespace arbc::image
