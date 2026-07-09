// Unit tests for org.arbc.crossfade's position evaluator, endpoint identity()
// short-circuit, Timed-over-Static metadata, and the union-of-inputs extent
// (refinement Acceptance criteria / claims crossfade-identity-at-endpoints,
// crossfade-timed-over-static, crossfade-extent-union). These are pure
// metadata/structure checks -- no rendering, no backend.

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

using namespace arbc;

namespace {

// A configurable Static leaf: distinctive bounds / time-extent / audio-extent so
// the union assertions can witness both inputs contributing. An absent audio
// extent makes the facet Static (Timed otherwise), so the same stub serves both
// the Timed-over-Static and the extent-union cases.
class StubAudio final : public AudioFacet {
public:
  explicit StubAudio(std::optional<TimeRange> extent) : d_extent(extent) {}
  std::optional<TimeRange> audio_extent() const override { return d_extent; }
  Stability audio_stability() const override {
    return d_extent.has_value() ? Stability::Timed : Stability::Static;
  }
  std::optional<AudioResult> render_audio(const AudioRequest& request,
                                          std::shared_ptr<AudioCompletion>) override {
    return AudioResult{request.sample_rate, true};
  }

private:
  std::optional<TimeRange> d_extent;
};

class StubInput final : public Content {
public:
  StubInput(std::optional<Rect> bounds, std::optional<TimeRange> time_extent,
            std::optional<TimeRange> audio_extent)
      : d_bounds(bounds), d_time(time_extent), d_audio(audio_extent) {}
  std::optional<Rect> bounds() const override { return d_bounds; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return d_time; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
  AudioFacet* audio() override { return &d_audio; }

private:
  std::optional<Rect> d_bounds;
  std::optional<TimeRange> d_time;
  StubAudio d_audio;
};

// A do-nothing surface: identity() carries a Surface& it never touches.
class DummySurface final : public Surface {
public:
  int width() const override { return 1; }
  int height() const override { return 1; }
  SurfaceFormat format() const override { return k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return {d_bytes, sizeof(d_bytes)}; }
  std::span<const std::byte> cpu_bytes() const override { return {d_bytes, sizeof(d_bytes)}; }

private:
  std::byte d_bytes[16]{};
};

// A crossfade transition over the window [1000, 2000): w == 0 before, ramps
// 0->1 across it, w == 1 at/after 2000.
CrossfadeParams window_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{1000}, Time{1000}};
}

std::optional<std::size_t> identity_at(CrossfadeContent& xf, DummySurface& surf, Time t) {
  const RenderRequest r{Rect::from_size(1.0, 1.0), 1.0, t, StateHandle{}, surf, Exactness::Exact,
                        Deadline::none()};
  return xf.identity(r);
}

} // namespace

// enforces: 13-effects-as-operators#crossfade-identity-at-endpoints
TEST_CASE("org.arbc.crossfade position ramps linearly and clamps at the endpoints") {
  StubInput a{std::nullopt, std::nullopt, std::nullopt};
  StubInput b{std::nullopt, std::nullopt, std::nullopt};
  CrossfadeContent xf{&a, &b, window_params()};

  CHECK(xf.position(Time{0}) == 0.0);    // before the window: input 0
  CHECK(xf.position(Time{1000}) == 0.0); // window start: still input 0 (exact 0)
  CHECK(xf.position(Time{1250}) == 0.25);
  CHECK(xf.position(Time{1500}) == 0.5); // midpoint
  CHECK(xf.position(Time{1750}) == 0.75);
  CHECK(xf.position(Time{2000}) == 1.0); // window end: input 1 (exact 1)
  CHECK(xf.position(Time{5000}) == 1.0); // after the window: input 1
}

// enforces: 13-effects-as-operators#crossfade-identity-at-endpoints
TEST_CASE("org.arbc.crossfade identity() returns {0} at w==0, {1} at w==1, nullopt between") {
  StubInput a{std::nullopt, std::nullopt, std::nullopt};
  StubInput b{std::nullopt, std::nullopt, std::nullopt};
  CrossfadeContent xf{&a, &b, window_params()};
  DummySurface surf;

  // w == 0 exactly: identity to input 0 (before and at the window start).
  CHECK(identity_at(xf, surf, Time{0}) == std::optional<std::size_t>{0});
  CHECK(identity_at(xf, surf, Time{1000}) == std::optional<std::size_t>{0});

  // w == 1 exactly: identity to input 1 (at and after the window end).
  CHECK(identity_at(xf, surf, Time{2000}) == std::optional<std::size_t>{1});
  CHECK(identity_at(xf, surf, Time{5000}) == std::optional<std::size_t>{1});

  // Interior: a genuine blend of both, equal to neither -- never identity.
  CHECK(identity_at(xf, surf, Time{1250}) == std::nullopt);
  CHECK(identity_at(xf, surf, Time{1500}) == std::nullopt);
  CHECK(identity_at(xf, surf, Time{1750}) == std::nullopt);
}

// enforces: 13-effects-as-operators#crossfade-timed-over-static
TEST_CASE("org.arbc.crossfade is Timed on both facets even over two Static inputs") {
  StubInput a{std::nullopt, std::nullopt, std::nullopt};
  StubInput b{std::nullopt, std::nullopt, std::nullopt};
  REQUIRE(a.stability() == Stability::Static);
  REQUIRE(b.stability() == Stability::Static);

  CrossfadeContent xf{&a, &b, window_params()};
  CHECK(xf.stability() == Stability::Timed); // visual: Timed over two Static

  AudioFacet* af = xf.audio();
  REQUIRE(af != nullptr);
  CHECK(af->audio_stability() == Stability::Timed); // audio: Timed over two Static
  CHECK(af->latency() == Time::zero());             // pure mix: no delay
}

// enforces: 13-effects-as-operators#crossfade-extent-union
TEST_CASE("org.arbc.crossfade bounds/time_extent/audio_extent are the union of both inputs") {
  // Two inputs with disjoint bounds, time extents, and audio extents.
  StubInput a{Rect{0.0, 0.0, 10.0, 10.0}, TimeRange{Time{0}, Time{100}},
              TimeRange{Time{0}, Time{100}}};
  StubInput b{Rect{20.0, 20.0, 30.0, 40.0}, TimeRange{Time{200}, Time{300}},
              TimeRange{Time{200}, Time{300}}};
  CrossfadeContent xf{&a, &b, window_params()};

  // bounds(): the bounding union of the two footprints, never an intersection.
  CHECK(xf.bounds() == std::optional<Rect>{Rect{0.0, 0.0, 30.0, 40.0}});
  // time_extent(): min start, max end.
  CHECK(xf.time_extent() == std::optional<TimeRange>{TimeRange{Time{0}, Time{300}}});
  // audio_extent(): the same union one dimension down (bounded because both are).
  AudioFacet* af = xf.audio();
  REQUIRE(af != nullptr);
  CHECK(af->audio_extent() == std::optional<TimeRange>{TimeRange{Time{0}, Time{300}}});
}

TEST_CASE("org.arbc.crossfade exposes two input edges and identity damage mapping") {
  StubInput a{std::nullopt, std::nullopt, std::nullopt};
  StubInput b{std::nullopt, std::nullopt, std::nullopt};
  CrossfadeContent xf{&a, &b, window_params()};

  // Two input edges in declared order {from, to}.
  REQUIRE(xf.inputs().size() == 2);
  CHECK(xf.inputs()[0] == &a);
  CHECK(xf.inputs()[1] == &b);

  // map_input_damage neither moves nor inflates: identity on the rect for both
  // inputs (both blend in the shared output coordinate space).
  const Rect damage{2.0, 3.0, 5.0, 7.0};
  CHECK(xf.map_input_damage(0, damage) == damage);
  CHECK(xf.map_input_damage(1, damage) == damage);
}
