#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>

// Byte-exact cross-component golden for achieved-time coalescing (doc 16:47-53,
// doc 11:110-129) -- the temporal analog of the tile_planning warm-frame and
// damage quiescent-frame goldens. The pure keying change is unit-tested in
// `src/compositor/t/temporal_coalescing.t.cpp`; here we drive the end-to-end
// resolve+composite path through the CPU backend and pin: a clock advance that
// stays inside one native frame period re-plans to all-fresh cache hits and
// reproduces the first frame BYTE-FOR-BYTE with zero re-decode, while an advance
// across a native-frame boundary renders a genuinely different frame.

namespace {

// A `Timed` 24 fps content that paints a solid fill whose color is a function of
// the quantized native frame, so two output frames in the SAME native period are
// byte-identical and two in DIFFERENT periods are not -- proving the reuse is
// native-frame-specific, not merely time-invariant. `render` counts its calls so
// the zero-render property is directly observable.
class TimedFill : public arbc::Content {
public:
  static constexpr std::int64_t k_frame = arbc::Time::flicks_per_second / 24; // 24 fps

  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  arbc::Stability stability() const override { return arbc::Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return arbc::TimeRange{arbc::Time::zero(), arbc::Time{k_frame * 48}};
  }
  std::optional<arbc::Time> quantize_time(arbc::Time t) const override {
    return arbc::Time{(t.flicks / k_frame) * k_frame};
  }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    ++d_renders;
    const std::int64_t frame = request.time.flicks / k_frame; // native frame index
    const float base = 0.05F * static_cast<float>(frame + 1);
    const std::span<float> px = request.target.span<arbc::PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i + 4 <= px.size(); i += 4) {
      px[i] = base;             // r
      px[i + 1] = base * 0.5F;  // g
      px[i + 2] = base * 0.25F; // b
      px[i + 3] = 1.0F;         // a (premultiplied)
    }
    arbc::RenderResult result{request.scale, /*exact=*/true};
    result.achieved_time = arbc::Time{frame * k_frame};
    return result;
  }

  int renders() const { return d_renders; }

private:
  int d_renders{0};
};

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

// Native frame 7 interior, one 1/60 s output step (still frame 7), next frame.
constexpr arbc::Time k_t0{210'000'000};
constexpr arbc::Time k_t_same{210'000'000 + arbc::Time::flicks_per_second / 60}; // 221'760'000
constexpr arbc::Time k_t_next{240'000'000};                                      // frame 8

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders
TEST_CASE("temporal golden: a coalesced frame is byte-identical to the rendered frame") {
  auto timed = std::make_shared<TimedFill>();
  arbc::Document document;
  const arbc::ObjectId content = document.add_content(timed);
  const arbc::ObjectId layer = document.add_layer(content, arbc::Affine::identity());
  const arbc::ObjectId comp = document.add_composition(512.0, 512.0);
  document.attach_layer(comp, layer);

  arbc::CpuBackend backend;
  // Anchor the direct frame walk at the scene's composition (doc 05:28-36).
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), comp};
  const arbc::DocStatePtr state = document.pin();
  const auto resolver = [&document](arbc::ObjectId id) { return document.resolve(id); };
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64u * 1024 * 1024);

  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame1 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame2 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame3 =
      backend.make_surface(512, 512, arbc::k_working_rgba32f);
  REQUIRE(frame1.has_value());
  REQUIRE(frame2.has_value());
  REQUIRE(frame3.has_value());

  arbc::CompositorCounters counters;

  // Frame 1 at t0: cold cache, renders native frame 7 into a persisted target.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame1,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, nullptr,
                                 k_t0);
  const int after_first = timed->renders();
  CHECK(after_first == 4); // a 512x512 scene is a 2x2 grid of 256^2 tiles

  // Frame 2 at t0 + 1/60 s (same native frame): the coalesced keys are warm, so
  // zero re-decode -- and the composited output is byte-identical to frame 1.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame2,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, nullptr,
                                 k_t_same);
  CHECK(timed->renders() == after_first); // zero renders on the coalesced frame
  REQUIRE(byte_identical(**frame1, **frame2));

  // Frame 3 at t2 in the next native frame: a distinct key, cold -> the content
  // renders frame 8, a genuinely different image. The byte-identity above was
  // native-frame reuse, not a time-invariant fill.
  arbc::render_frame_interactive(*state, resolver, viewport, cache, backend, pool, **frame3,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, nullptr,
                                 k_t_next);
  CHECK(timed->renders() == after_first + 4); // the boundary crossing re-renders
  CHECK_FALSE(byte_identical(**frame1, **frame3));
}
