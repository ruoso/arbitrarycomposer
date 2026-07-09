// Unit tests for org.arbc.fade's envelope evaluator, identity() short-circuit,
// and Timed-over-Static metadata aggregation (refinement Acceptance criteria).
// These are pure metadata/structure checks -- no rendering, no backend.

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

using namespace arbc;

namespace {

// A minimal Static leaf input: it exists only so fade has something to wrap and
// to aggregate metadata from. Its bounds/time_extent are distinctive so the
// pass-through assertions can witness them unchanged.
class StubInput final : public Content {
public:
  std::optional<Rect> bounds() const override { return Rect{1.0, 2.0, 9.0, 10.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
};

// A do-nothing surface: identity() takes a RenderRequest, which carries a
// Surface& it never touches. This satisfies the reference without a backend.
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

// Fade with a symmetric fade-in [0, 1000) and fade-out [3000, 4000); plateau
// (E == 1) over [1000, 3000].
FadeParams sym_params() {
  return FadeParams{FadeShape::Linear, FadeWindow{Time{0}, Time{1000}},
                    FadeWindow{Time{3000}, Time{4000}}};
}

std::optional<std::size_t> identity_at(FadeContent& fade, DummySurface& surf, Time t) {
  const RenderRequest r{Rect::from_size(1.0, 1.0), 1.0, t, StateHandle{}, surf, Exactness::Exact,
                        Deadline::none()};
  return fade.identity(r);
}

} // namespace

// enforces: 13-effects-as-operators#fade-identity-at-open-envelope
TEST_CASE("org.arbc.fade envelope ramps linearly and plateaus at 1") {
  StubInput input;
  FadeContent fade(&input, sym_params());

  CHECK(fade.envelope(Time{-500}) == 0.0); // before the fade-in: fully closed
  CHECK(fade.envelope(Time{0}) == 0.0);    // fade-in start
  CHECK(fade.envelope(Time{500}) == 0.5);  // fade-in midpoint
  CHECK(fade.envelope(Time{1000}) == 1.0); // fade-in end: open
  CHECK(fade.envelope(Time{2000}) == 1.0); // plateau
  CHECK(fade.envelope(Time{3000}) == 1.0); // fade-out start: still open
  CHECK(fade.envelope(Time{3500}) == 0.5); // fade-out midpoint
  CHECK(fade.envelope(Time{4000}) == 0.0); // fade-out end: closed
  CHECK(fade.envelope(Time{5000}) == 0.0); // after the fade-out: closed
}

// enforces: 13-effects-as-operators#fade-identity-at-open-envelope
TEST_CASE("org.arbc.fade identity() returns input 0 iff the envelope is exactly 1") {
  StubInput input;
  FadeContent fade(&input, sym_params());
  DummySurface surf;

  // Exactly on the plateau: identity to input 0.
  CHECK(identity_at(fade, surf, Time{1000}) == std::optional<std::size_t>{0});
  CHECK(identity_at(fade, surf, Time{2000}) == std::optional<std::size_t>{0});
  CHECK(identity_at(fade, surf, Time{3000}) == std::optional<std::size_t>{0});

  // Partial and fully-closed envelope values are NOT identity (the output is an
  // attenuation / transparency, not equal to input 0).
  CHECK(identity_at(fade, surf, Time{500}) == std::nullopt);
  CHECK(identity_at(fade, surf, Time{3500}) == std::nullopt);
  CHECK(identity_at(fade, surf, Time{0}) == std::nullopt);
  CHECK(identity_at(fade, surf, Time{4000}) == std::nullopt);
}

// enforces: 13-effects-as-operators#fade-timed-over-static
TEST_CASE("org.arbc.fade is Timed on both facets even over a Static input") {
  StubInput input;
  REQUIRE(input.stability() == Stability::Static);

  FadeContent fade(&input, sym_params());
  CHECK(fade.stability() == Stability::Timed); // visual: Timed over Static

  AudioFacet* af = fade.audio();
  REQUIRE(af != nullptr);
  CHECK(af->audio_stability() == Stability::Timed); // audio: Timed over Static
  CHECK(af->latency() == Time::zero());             // pure gain: no delay
}

TEST_CASE("org.arbc.fade is spatial/temporal identity in its metadata") {
  StubInput input;
  FadeContent fade(&input, sym_params());

  // bounds()/time_extent() pass the input's through unchanged.
  CHECK(fade.bounds() == input.bounds());
  CHECK(fade.time_extent() == input.time_extent());

  // A single input edge, exposed through inputs().
  REQUIRE(fade.inputs().size() == 1);
  CHECK(fade.inputs()[0] == &input);

  // map_input_damage neither moves nor inflates: identity on the rect.
  const Rect damage{2.0, 3.0, 5.0, 7.0};
  CHECK(fade.map_input_damage(0, damage) == damage);
}
