#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>

// Byte-exact cross-component golden for the progressive-refinement loop (doc
// 16:47-53, doc 02:69-71 step 6). The pure ring/queue logic is unit-tested in
// `src/compositor/t/refinement.t.cpp`; here we drive the end-to-end
// resolve+composite path through the CPU backend and pin the coarse-then-refine
// promise:
//   1. A layer whose content answers async records its misses pending (frame 1
//      shows the fallback); settling the completions + polling inserts the
//      arrivals and emits damage; a follow-up frame re-plans them Fresh and
//      composites a target BYTE-IDENTICAL to an all-inline (synchronous) render
//      of the same scene -- no seam, no double-blend (doc 02:69-71).
//   2. A quiescent frame after refinement drains schedules nothing: the poll
//      emits zero damage and the re-plan issues zero misses (doc 02:51; the
//      behavioral-counter class, doc 16:54-62).

namespace {

// A Content that renders a solid fill into the request target but answers
// asynchronously: it returns `nullopt` (leaving the driver-supplied
// `RenderCompletion` live for the caller to record), so the test models the
// worker's arrival by completing that pending completion out of band. It counts
// its render calls so a quiescent frame's zero-miss property is observable (the
// driver calls `render` only inside `if (tile.is_miss)`).
class AsyncSolidContent : public arbc::Content {
public:
  AsyncSolidContent(arbc::Rgba color, std::optional<arbc::Rect> bounds)
      : d_solid(color, std::move(bounds)) {}

  std::optional<arbc::Rect> bounds() const override { return d_solid.bounds(); }
  arbc::Stability stability() const override { return d_solid.stability(); }
  std::optional<arbc::TimeRange> time_extent() const override { return d_solid.time_extent(); }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion> done) override {
    ++d_renders;
    // Fill the target now (as a worker eventually would), but answer async: the
    // driver records `done` as pending, and the test settles it to model the
    // arrival. The throwaway completion absorbs the synchronous solid settle.
    auto sink = std::make_shared<arbc::RenderCompletion>();
    d_solid.render(request, sink);
    (void)done;
    return std::nullopt;
  }

  int renders() const { return d_renders; }

private:
  arbc::SolidContent d_solid;
  int d_renders{0};
};

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

// The all-inline reference: render the same scene with a *synchronous* solid
// content. This is what the refined follow-up frame must reproduce byte-for-byte.
std::unique_ptr<arbc::Surface> render_inline_reference(arbc::CpuBackend& backend,
                                                       const arbc::Viewport& viewport,
                                                       arbc::Rgba color, arbc::Rect bounds) {
  arbc::Document document;
  const arbc::ObjectId content =
      document.add_content(std::make_shared<arbc::SolidContent>(color, bounds));
  document.add_layer(content, arbc::Affine::identity());
  const arbc::DocStatePtr state = document.pin();
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(viewport.width, viewport.height, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::render_frame_interactive(
      *state, [&document](arbc::ObjectId id) { return document.resolve(id); }, viewport, cache,
      backend, pool, **target, arbc::Deadline::none(), std::nullopt);
  return std::move(*target);
}

} // namespace

// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("refinement golden: async arrivals refine to a byte-identical inline render") {
  // Identity camera, unit-scale layer -> composed scale 1.0 -> rung 0. A 512x512
  // content splits into a 2x2 grid of 256^2 tiles at a power-of-two, axis-aligned
  // scale.
  const arbc::Rgba color{0.25F, 0.5F, 0.75F, 1.0F};
  const arbc::Rect bounds{0.0, 0.0, 512.0, 512.0};

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};

  const std::unique_ptr<arbc::Surface> reference =
      render_inline_reference(backend, viewport, color, bounds);

  // The async scene.
  auto async = std::make_shared<AsyncSolidContent>(color, bounds);
  arbc::Document document;
  const arbc::ObjectId content = document.add_content(async);
  document.add_layer(content, arbc::Affine::identity());
  const arbc::DocStatePtr state = document.pin();
  const auto resolver = [&document](arbc::ObjectId id) { return document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);
  arbc::RefinementQueue queue;

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame1 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame2 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(frame1.has_value());
  REQUIRE(frame2.has_value());

  // Frame 1: every tile is an async miss -> recorded pending, composited as its
  // (empty-cache) placeholder fallback. Not yet the sharp result.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame1,
                                 arbc::Deadline::none(), std::nullopt, &queue);
  CHECK(async->renders() == 4);
  CHECK(queue.tiles.size() == 4);
  CHECK_FALSE(byte_identical(*reference, **frame1)); // fallback != the sharp render

  // A poll before anything settles schedules no follow-up frame and keeps the
  // pending tiles.
  CHECK(arbc::poll_refinements(queue, cache).empty());
  CHECK(queue.tiles.size() == 4);

  // The arrivals land: settle each completion (the worker's out-of-band settle).
  for (arbc::PendingTile& pending : queue.tiles) {
    pending.done->complete(arbc::RenderResult{});
  }

  // The poll inserts each arrival under Visible and emits one damage per tile,
  // draining the queue.
  const std::vector<arbc::Damage> damage = arbc::poll_refinements(queue, cache);
  CHECK(damage.size() == 4);
  CHECK(queue.tiles.empty());

  // Frame 2: the now-resident tiles re-plan Fresh (zero new renders) and
  // composite the sharp result -- byte-identical to the all-inline reference.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame2,
                                 arbc::Deadline::none(), std::nullopt, &queue);
  CHECK(async->renders() == 4); // no new misses: all Fresh
  CHECK(queue.tiles.empty());   // no new async records
  REQUIRE(byte_identical(*reference, **frame2));

  // enforces: 02-architecture#quiescent-refinement-schedules-no-frame
  // Frame 3 over the drained, unchanged scene: zero misses, and the poll emits
  // zero damage -- no follow-up frame is scheduled.
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame3 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(frame3.has_value());
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame3,
                                 arbc::Deadline::none(), std::nullopt, &queue);
  CHECK(async->renders() == 4); // still zero new misses
  CHECK(arbc::poll_refinements(queue, cache).empty());
  CHECK(queue.tiles.empty());
}
