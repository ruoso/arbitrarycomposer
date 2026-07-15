#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>
#include <arbc/surface/testing/counting_backend.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>

// Cross-component goldens + behavioral counters for content-provided surfaces
// (doc 09 §Content-provided surfaces, `surfaces.provided_surfaces`). Content
// answers a render by returning its OWN surface via `RenderResult.provided`
// instead of filling the compositor's target; the compositor composites
// directly from it inline (zero copy) and copies it into the tile cache on the
// cache path, releasing it the instant it is done.

namespace {

// Content that answers every render by returning its OWN framebuffer as a
// `provided` surface, never filling `request.target`. The framebuffer is
// allocated through a SEPARATE backend (`fb_backend`) so a test's compositor-
// side CountingBackend counts only compositor allocations, not the content's.
// It is reused across renders (an engine reusing its target), and a behavioral
// counter tracks provides vs releases so a test proves within-frame release.
class ProvidingContent final : public arbc::Content {
public:
  ProvidingContent(arbc::Backend& fb_backend, arbc::Rgba color, std::optional<arbc::Rect> bounds,
                   bool transient)
      : d_fb_backend(fb_backend), d_color(color), d_bounds(std::move(bounds)),
        d_transient(transient) {}

  std::optional<arbc::Rect> bounds() const override { return d_bounds; }
  // Static so a warm second frame is a deterministic cache hit (the independence
  // probe reads the cached COPY, not a re-render). The `transient` discipline
  // under test lives on the provided handle, independent of stability -- v1
  // copies to cache regardless of the flag (doc 09:328-340).
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    const int w = request.target.width();
    const int h = request.target.height();
    if (!d_fb || d_fb->width() != w || d_fb->height() != h ||
        d_fb->format() != request.target.format()) {
      arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> made =
          d_fb_backend.make_surface(w, h, request.target.format());
      d_fb = std::move(*made);
    }
    d_fb_backend.clear(*d_fb, d_color.r, d_color.g, d_color.b, d_color.a);
    ++d_renders;
    // Answer with our own surface (doc 09:87-100): the compositor honors it in
    // place of the never-touched `request.target`. Release bumps a counter.
    arbc::RenderResult result{request.scale, true};
    result.provided.emplace(*d_fb, [this] { ++d_releases; }, d_transient);
    return result;
  }

  // Mutate the content's framebuffer AFTER a frame -- proves the cached tile is
  // an independent copy (a copy is unaffected; an adoption would follow this).
  void overwrite(arbc::Rgba color) {
    if (d_fb) {
      d_fb_backend.clear(*d_fb, color.r, color.g, color.b, color.a);
    }
  }

  int renders() const { return d_renders; }
  int releases() const { return d_releases; }

private:
  arbc::Backend& d_fb_backend;
  arbc::Rgba d_color;
  std::optional<arbc::Rect> d_bounds;
  bool d_transient;
  std::unique_ptr<arbc::Surface> d_fb;
  int d_renders{0};
  int d_releases{0};
};

std::array<float, 4> pixel(const arbc::Surface& surface, int x, int y) {
  const std::span<const float> data =
      std::as_const(surface).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::size_t at =
      4 * (static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width()) +
           static_cast<std::size_t>(x));
  return {data[at], data[at + 1], data[at + 2], data[at + 3]};
}

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

constexpr arbc::Rgba k_red{1.0F, 0.0F, 0.0F, 1.0F};
constexpr arbc::Rect k_unit{0.0, 0.0, 1.0, 1.0};

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 09-surfaces-and-backends#content-provided-surface-honored
TEST_CASE("inline composite from a provided surface equals the same content filling the target") {
  // A provided-surface render composites BYTE-IDENTICALLY to an ordinary
  // target-filling render of the same solid at the same placement (doc 09:80,
  // 122-124): the provided path only changes where the pixels come from.
  const arbc::Affine placement =
      compose(arbc::Affine::translation(2.0, 2.0), arbc::Affine::scaling(4.0, 4.0));
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity()};

  arbc::CpuBackend fb_backend; // the content's own framebuffer allocator
  arbc::Document provided_doc;
  const arbc::ObjectId provided_layer =
      provided_doc.add_layer(provided_doc.add_content(std::make_shared<ProvidingContent>(
                                 fb_backend, k_red, k_unit, /*transient=*/false)),
                             placement);
  // Attach to a composition so the composition-scoped walk draws it; the offline
  // driver sources this (single) composition as the root (doc 05:28-36).
  provided_doc.attach_layer(provided_doc.add_composition(8.0, 8.0), provided_layer);

  arbc::Document solid_doc;
  const arbc::ObjectId solid_layer = solid_doc.add_layer(
      solid_doc.add_content(std::make_shared<arbc::SolidContent>(k_red, k_unit)), placement);
  solid_doc.attach_layer(solid_doc.add_composition(8.0, 8.0), solid_layer);

  arbc::CpuBackend backend;
  const auto provided_out = render_offline(provided_doc, viewport, backend);
  const auto solid_out = render_offline(solid_doc, viewport, backend);
  REQUIRE(provided_out.has_value());
  REQUIRE(solid_out.has_value());
  REQUIRE(byte_identical(**solid_out, **provided_out));
}

// enforces: 09-surfaces-and-backends#content-provided-surface-honored
// enforces: 09-surfaces-and-backends#provided-surface-released-after-consume
TEST_CASE("inline provided path allocates no copy and releases the surface within the frame") {
  arbc::CpuBackend fb_backend;
  auto providing =
      std::make_shared<ProvidingContent>(fb_backend, k_red, k_unit, /*transient=*/false);
  arbc::Document document;
  const arbc::ObjectId layer = document.add_layer(
      document.add_content(providing),
      compose(arbc::Affine::translation(2.0, 2.0), arbc::Affine::scaling(4.0, 4.0)));
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  document.attach_layer(comp, layer);

  arbc::CpuBackend cpu;
  arbc::testing::CountingBackend backend(cpu);
  // Anchor the direct frame walk at the scene's composition (doc 05:28-36).
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity(), comp};
  const arbc::DocStatePtr state = document.pin();
  const auto resolve = [&document](arbc::ObjectId id) { return document.resolve(id); };
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(8, 8, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::SurfacePool pool(backend);

  // Warm the pool so the per-layer temp is recycled on the measured frame; then
  // any compositor make_surface would be a COPY of the provided surface.
  render_frame(*state, resolve, viewport, backend, pool, **target);
  REQUIRE(providing->renders() == 1);
  REQUIRE(providing->releases() == 1); // released within the first frame already

  backend.make_surface_calls = 0;
  render_frame(*state, resolve, viewport, backend, pool, **target);
  // Zero-copy inline composite (doc 09:122-124): the temp recycles and the
  // provided surface is composited directly -- no copy allocation at all.
  CHECK(backend.make_surface_calls == 0);
  // The provided surface's release callback fired within the frame: provides and
  // releases stay balanced, so the compositor holds no reference across frames.
  CHECK(providing->releases() == providing->renders());
  CHECK(providing->releases() == 2);
}

// enforces: 09-surfaces-and-backends#transient-provided-copied-not-cached
// enforces: 09-surfaces-and-backends#provided-surface-released-after-consume
TEST_CASE("a transient provided surface is copied into the cache, independent of the content") {
  arbc::CpuBackend fb_backend;
  // A 256x256 content at identity scale -> rung 0 -> a single 256^2 tile.
  auto providing = std::make_shared<ProvidingContent>(
      fb_backend, k_red, arbc::Rect{0.0, 0.0, 256.0, 256.0}, /*transient=*/true);
  arbc::Document document;
  const arbc::ObjectId layer =
      document.add_layer(document.add_content(providing), arbc::Affine::identity());
  const arbc::ObjectId comp = document.add_composition(256.0, 256.0);
  document.attach_layer(comp, layer);

  arbc::CpuBackend cpu;
  arbc::testing::CountingBackend backend(cpu);
  // Anchor the direct interactive walk at the scene's composition (doc 05:28-36).
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity(), comp};
  const arbc::DocStatePtr state = document.pin();
  const auto resolve = [&document](arbc::ObjectId id) { return document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame1 =
      backend.make_surface(256, 256, arbc::k_working_rgba32f);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame2 =
      backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(frame1.has_value());
  REQUIRE(frame2.has_value());

  backend.make_surface_calls = 0;
  arbc::render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **frame1,
                                 arbc::Deadline::none(), std::nullopt);
  // Exactly one make_surface for the cache copy per cached tile (doc 09:328-340):
  // the single cache-owned tile surface the provided pixels are copied into --
  // no second allocation for the copy itself.
  CHECK(backend.make_surface_calls == 1);
  CHECK(providing->renders() == 1);
  // Released within the frame -- the compositor retains no reference (doc 09:106-112).
  CHECK(providing->releases() == 1);
  REQUIRE(pixel(**frame1, 8, 8) == std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});

  // Independence: mutate the content's framebuffer AFTER the frame; the cached
  // tile is a COPY, so a warm second frame (cache hit -> no re-render) still
  // composites the original red -- an adoption would follow the content to blue.
  providing->overwrite(arbc::Rgba{0.0F, 0.0F, 1.0F, 1.0F});
  arbc::render_frame_interactive(*state, resolve, viewport, cache, backend, pool, **frame2,
                                 arbc::Deadline::none(), std::nullopt);
  CHECK(providing->renders() == 1); // warm hit: no new render off the mutated framebuffer
  REQUIRE(pixel(**frame2, 8, 8) == std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});
  REQUIRE(byte_identical(**frame1, **frame2));
}
