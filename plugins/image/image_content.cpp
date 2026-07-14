#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/image_resampler.hpp> // decimate_half_band / sample_bicubic (media, L2)
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>

#include <imdec.h> // the PRIVATE decode dep (arbc-plugin-imdec), never in libarbc

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
  const std::size_t o = (static_cast<std::size_t>(cy) * static_cast<std::size_t>(lvl.width) +
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

PyramidCache::Entry* PyramidCache::pick_victim_locked() {
  // The least-recently-pinned RESIDENT, UNPINNED entry. A pinned one is never a victim
  // (doc 02:268-277) and a non-resident one has nothing left to free -- it is already evicted
  // and is holding only its encoded source.
  Entry* victim = nullptr;
  for (auto& slot : d_by_uri) {
    Entry& entry = slot.second;
    if (entry.resident == nullptr || entry.pins != 0) {
      continue;
    }
    if (victim == nullptr || entry.recency < victim->recency) {
      victim = &entry;
    }
  }
  return victim;
}

void PyramidCache::evict_to_fit_locked() {
  while (d_resident > d_budget) {
    Entry* victim = pick_victim_locked();
    if (victim == nullptr) {
      break; // only pinned entries remain -> SOFT OVERSHOOT (doc 02:278-284)
    }
    d_resident -= victim->bytes;
    victim->bytes = 0;
    victim->resident.reset(); // the pixels; the ENCODED bytes stay, so a pin can rebuild them
    ++d_evictions;
  }
}

PyramidPtr PyramidCache::refill_locked(Entry& entry) {
  // Decoding UNDER THE MUTEX (Decision 9). It costs a ~24 MP re-decode holding a global lock,
  // which is a latency defect and is registered as `kinds.image_decode_in_flight` -- but it
  // preserves ONE DECODE PER KEY under concurrency by construction, and that invariant gets
  // harder, not easier, once misses can originate on N render workers rather than one load
  // thread.
  PyramidPtr fresh = Pyramid::decode(std::span<const unsigned char>(*entry.encoded));
  if (!fresh) {
    return nullptr; // bytes that decoded once cannot stop decoding; defensive, not expected
  }
  ++d_decodes_issued;
  entry.bytes = fresh->resident_bytes();
  entry.resident = fresh;
  d_resident += entry.bytes;
  return fresh;
}

PyramidPtr PyramidCache::resolve(std::string_view resolved_uri,
                                 std::span<const unsigned char> encoded) {
  // An empty resolved URI names no identity to dedup on (an absent/mistyped `params.source`).
  // Decode nothing and cache nothing: the content is simply unavailable.
  if (resolved_uri.empty()) {
    return nullptr;
  }
  // EMPTY BYTES ARE ABSENCE (`load_context.hpp:35-38`), and absence is never satisfied from the
  // cache -- not even when the cache holds this very URI because another document already loaded
  // the same photograph. A caller with no bytes is a PENDING or UNAVAILABLE content stating that
  // its source has not answered, and answering it with someone else's decode would collapse doc
  // 08 Principle 3's three load states into "did anyone else open this file first". The weak
  // cache got this right only by accident (its entry died with the other document); an owning
  // cache has to say it.
  if (encoded.empty()) {
    return nullptr;
  }
  const std::lock_guard<std::mutex> lock(d_mutex);
  const std::string key(resolved_uri);
  if (const auto it = d_by_uri.find(key); it != d_by_uri.end()) {
    Entry& entry = it->second;
    entry.recency = ++d_tick;
    // The dedup: one decode per RESOLVED identity (doc 08:116-122). An entry whose pixels were
    // evicted rebuilds from its OWN retained bytes, never from the caller's -- the identity is
    // the URI, and a second caller's bytes for one URI are the same bytes by construction.
    PyramidPtr resident = entry.resident ? entry.resident : refill_locked(entry);
    evict_to_fit_locked();
    return resident;
  }
  PyramidPtr fresh = Pyramid::decode(encoded);
  if (!fresh) {
    return nullptr; // undecodable: unavailable, and nothing to remember
  }
  ++d_decodes_issued;
  Entry entry;
  // Retained, and NOT part of the budget: ~8 MB of source against a ~512 MB pyramid, and this
  // plugin performs no file I/O at all, so bytes it drops are bytes it can never get back
  // (Decision 2).
  entry.encoded =
      std::make_shared<const std::vector<unsigned char>>(encoded.begin(), encoded.end());
  entry.bytes = fresh->resident_bytes();
  entry.resident = fresh;
  entry.recency = ++d_tick;
  d_resident += entry.bytes;
  d_by_uri.emplace(key, std::move(entry));
  // The admit is unpinned, so a budget too small to hold it drops it again immediately. That is
  // correct and it is the point: the pyramid is derived data, the next pull rebuilds it
  // byte-identically, and the caller already holds `fresh` for as long as it needs it.
  evict_to_fit_locked();
  return fresh;
}

PyramidPin PyramidCache::pin(std::string_view resolved_uri) {
  if (resolved_uri.empty()) {
    return {};
  }
  const std::lock_guard<std::mutex> lock(d_mutex);
  const auto it = d_by_uri.find(std::string(resolved_uri));
  if (it == d_by_uri.end()) {
    return {}; // never admitted: the pending / unavailable state, and never an error
  }
  Entry& entry = it->second;
  PyramidPtr resident = entry.resident ? entry.resident : refill_locked(entry);
  if (!resident) {
    return {};
  }
  // PIN BEFORE TRIMMING. A freshly-refilled entry larger than the entire budget would otherwise
  // be its own victim, and the pull that asked for it would get pixels the cache had already
  // dropped. Correctness outranks the budget (doc 02:278-284), so the pinned working set is
  // never trimmed and resident bytes are simply allowed to overshoot.
  ++entry.pins;
  entry.recency = ++d_tick; // recency is by PIN, not by insert: an LRU of what renders read
  evict_to_fit_locked();
  return PyramidPin(this, &entry, std::move(resident));
}

void PyramidCache::unpin(Entry* entry) noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  if (entry->pins > 0) {
    --entry->pins;
  }
  if (entry->pins == 0) {
    // The render that needed it is done, so it is now an eviction candidate like any other.
    // This is what makes a budget the cache genuinely cannot honor (a 1-byte budget, say) drop
    // every pyramid the moment nothing is reading it -- and re-decode on the next pull.
    evict_to_fit_locked();
  }
}

void PyramidCache::set_byte_budget(std::size_t byte_budget) {
  const std::lock_guard<std::mutex> lock(d_mutex);
  d_budget = byte_budget;
  evict_to_fit_locked();
}

std::uint64_t PyramidCache::decodes_issued() const noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_decodes_issued;
}

std::uint64_t PyramidCache::evictions() const noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_evictions;
}

std::size_t PyramidCache::resident_bytes() const noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_resident;
}

std::size_t PyramidCache::budget() const noexcept {
  const std::lock_guard<std::mutex> lock(d_mutex);
  return d_budget;
}

namespace {

// `ARBC_IMAGE_PYRAMID_BUDGET_BYTES`, the one seam a host that `dlopen`s the MODULE can reach
// (Decision 8): the registration surface is `registry.add(kind_id, factory, KindMetadata)` and
// the per-content channel is the opaque `ContentConfig` frame -- NEITHER carries plugin-wide
// policy, and widening the factory signature to configure one number in one plugin is an ABI
// change to `contract` affecting every kind. `ARBC_PLUGIN_PATH` is the house precedent for
// exactly this shape of problem. Unset, empty, or unparseable falls back to the default.
std::size_t configured_pyramid_budget() {
  // MSVC deprecates getenv in favour of _dupenv_s; suppress the warning since getenv is both
  // correct and portable for a read-only environment lookup (`plugin_host.cpp:186-195`).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* env = std::getenv("ARBC_IMAGE_PYRAMID_BUDGET_BYTES");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
  if (env == nullptr || env[0] == '\0') {
    return k_default_pyramid_budget;
  }
  const std::string_view text(env);
  std::size_t budget = 0;
  const char* const end = text.data() + text.size();
  const std::from_chars_result parsed = std::from_chars(text.data(), end, budget);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return k_default_pyramid_budget; // a malformed override is not a reason to fail a load
  }
  return budget;
}

} // namespace

PyramidCache& default_pyramid_cache() {
  static PyramidCache cache(configured_pyramid_budget());
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
    : d_uri(std::move(authored_uri)), d_cache(&default_pyramid_cache()),
      d_owned(std::move(pyramid)), d_tiles(std::make_shared<TileStore>()) {
  // UN-KEYED (Constraint 6): no cache identity, so nothing to re-decode from -- it owns its
  // pyramid strongly and un-evictably, and the budget never governs it.
  if (d_owned) {
    publish_extent(*d_owned);
  }
}

ImageContent::ImageContent(std::string authored_uri, std::string resolved_uri, PyramidPtr pyramid,
                           PyramidCache& cache)
    : d_uri(std::move(authored_uri)), d_resolved(std::move(resolved_uri)), d_cache(&cache),
      d_tiles(std::make_shared<TileStore>()) {
  // KEYED: `pyramid` is what the caller's `resolve()` just admitted, and the content reads its
  // EXTENT off it and then LETS IT GO (Decision 1). Holding it would defeat the budget --
  // while any content holds a strong reference, evicting the cache entry frees zero bytes, and
  // a layer lives for as long as the document is open, which is the entire span the budget is
  // supposed to govern. Pixels are re-acquired per render through `pixels()`.
  if (pyramid) {
    publish_extent(*pyramid);
  }
}

void ImageContent::publish_extent(const Pyramid& pyramid) noexcept {
  const auto packed =
      (static_cast<std::uint64_t>(static_cast<std::uint32_t>(pyramid.width())) << 32U) |
      static_cast<std::uint32_t>(pyramid.height());
  std::uint64_t unpublished = 0;
  // EXACTLY ONCE, and never cleared: two racing installs, or a redundant one, store nothing the
  // second time. This is the monotonic publish doc 03's `install_asset` contract demands, moved
  // off the pixels and onto the extent (kinds.image_master_budget Decision 3).
  d_extent.compare_exchange_strong(unpublished, packed, std::memory_order_release,
                                   std::memory_order_relaxed);
}

PyramidPin ImageContent::pixels() const {
  // THE EXTENT GATES THE PIXELS, and the cache's residency never does. A content with no extent
  // is PENDING or UNAVAILABLE and it has NO PIXELS -- even when the cache happens to hold an
  // entry under its resolved URI, which it does whenever another document already loaded the same
  // photograph. Resolving those pixels would make a pending image render the picture its source
  // has not answered with yet, collapsing the distinction doc 08 Principle 3 draws between the
  // three load states into "did some other layer decode this file first". Pending and unavailable
  // are pixel-identical, and they stay that way.
  if (!available()) {
    return {};
  }
  if (d_owned) {
    return PyramidPin(d_owned); // un-keyed: an unbudgeted pin over the owned pyramid
  }
  // Keyed: through the cache, which re-decodes from its retained encoded bytes when the entry
  // was evicted. This is the ONLY path by which an evicted image gets its pixels back.
  return d_cache->pin(d_resolved);
}

bool ImageContent::install_asset(std::string_view encoded) {
  // PUBLISH ONCE, MONOTONICALLY -- the EXTENT (Decision 3). A content that already knows its
  // extent keeps it: a duplicate arrival must not re-publish it, and eviction must never clear
  // it, because `bounds()` reads it on the compositor's cull path and an image that culled
  // itself out of the composition when memory got tight would simply VANISH. The PIXELS carry
  // no such obligation any more -- they are budgeted derived data, evictable and re-derivable
  // byte-identically from the bytes the cache retained.
  if (available()) {
    return true;
  }

  // Through the SAME cache the inline path resolves through, keyed on the SAME resolved
  // URI the content was constructed with -- which is what makes a pending-then-settled
  // image cost exactly one decode, and what makes N contents deferring on one URI share it.
  // Empty or undecodable bytes yield nullptr: unavailable, a value, never a throw
  // (Constraint 11).
  const std::span<const unsigned char> bytes(reinterpret_cast<const unsigned char*>(encoded.data()),
                                             encoded.size());
  const PyramidPtr decoded = d_cache->resolve(d_resolved, bytes);
  if (!decoded) {
    return false;
  }
  publish_extent(*decoded);
  return true;
}

std::optional<Rect> ImageContent::bounds() const {
  // The RETAINED EXTENT -- no pin, no lock, no decode (Constraint 2). Residency is not
  // geometry: an evicted image keeps exactly the bounds it had while resident, so eviction is
  // invisible to the cull path and a photograph never disappears because memory got tight.
  const std::uint64_t extent = d_extent.load(std::memory_order_acquire);
  if (extent == 0) {
    // Unavailable -- and PENDING, which is minted in the same shape and is culled for the
    // same reason (doc 08:135-144: fabricating an extent would let a not-yet-arrived file
    // change the composition's geometry, and it would change AGAIN at the install). This is
    // the ONLY state with empty bounds; an evicted image is emphatically not in it.
    return Rect{}; // EMPTY, not nullopt: nullopt means UNBOUNDED (doc 03:76-78)
  }
  return Rect{0.0, 0.0, static_cast<double>(static_cast<std::uint32_t>(extent >> 32U)),
              static_cast<double>(static_cast<std::uint32_t>(extent))};
}

std::optional<RenderResult> ImageContent::render(const RenderRequest& request,
                                                 std::shared_ptr<RenderCompletion> done) {
  // ONE pin for the whole render, held to the last pixel write. It is the memory-safety rule of
  // the byte budget (doc 02:268-277): the entry a render is about to composite from must not be
  // evicted mid-render, and because the pin is an OWNING `shared_ptr`, even an entry the cache
  // drops keeps its pixels alive until this call finishes. Taking it once also means a single
  // render never straddles a transition -- a writer-thread install, or an eviction, lands
  // wholly before or wholly after it.
  //
  // This is where the kind now DECODES: a pull of an evicted image rebuilds it here, on a
  // compositor worker, where allocation is already routine (`TileStore::acquire` below
  // allocates surfaces in `render` today). There is no audio facet, so no RT thread is ever in
  // this call.
  const PyramidPin pin = pixels();
  const Pyramid* master = pin.get();
  if (master == nullptr) {
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

  const int w = master->width();
  const int h = master->height();
  const auto max_level = static_cast<int>(master->level_count()) - 1;

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
                                [&](int sx, int sy) { return master->pixel(l, sx, sy); });
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
  result.provided.emplace(*tile, [store, tile]() { store->release(tile); }, /*transient=*/false);
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

  // Empty bytes == absence (`load_context.hpp:35-38`), which is the unavailable state and
  // NOT an error: the URI is kept verbatim, the content reports empty bounds, and the
  // document loads (Constraint 6, Decision 7). It is ALSO the shape a PENDING image is
  // minted in -- the kind cannot tell the two apart from the frame, and does not need to.
  // Which one it is, is a fact about the LOAD (did the source answer?), and it lives in the
  // core's `PendingExternalLoads`, never here.
  //
  // The RESOLVED URI rides into the content, not just into this decode: it is the identity
  // a late `install_asset` re-keys the same cache entry on.
  const std::span<const unsigned char> encoded(reinterpret_cast<const unsigned char*>(bytes.data()),
                                               bytes.size());
  PyramidPtr pyramid = default_pyramid_cache().resolve(resolved, encoded);
  return std::unique_ptr<Content>(std::make_unique<ImageContent>(
      std::string(authored), std::string(resolved), std::move(pyramid)));
}

} // namespace arbc::image
