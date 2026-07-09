// org.arbc.fade contract conformance (refinement Acceptance "Conformance"):
// wires a testing::ContentFactory over a FadeContent fronted by an inline
// PullService + CpuBackend, then runs the umbrella contract_tests with
// operator_graph on plus the granular operator families
// check_operator_damage_covers / check_operator_identity_faithful. Because
// factory()->audio() != nullptr the umbrella also runs the audio families
// (a fade always exposes a gain facet); a tone-input fixture exercises the
// audio-facet gain path directly. Mirrors tests/nested_conformance.t.cpp.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

namespace {

using namespace arbc;

// Route both facets to the wrapped input, exactly as nested_conformance's
// InlineAudioPull: pull() renders the input's visual facet, pull_audio() its
// audio facet (null-facet inputs settle unavailable -> fade shows silence).
class InlineAudioPull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    AudioFacet* af = input != nullptr ? input->audio() : nullptr;
    if (af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> r = af->render_audio(request, done);
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }
};

// A symmetric fade with a plateau (E == 1) over [10s, 100s] so identity() is a
// well-defined interval, the audible extent is bounded (both windows close it),
// and the audio consistency window (anchored at the extent start) rides the
// fade-in ramp across many frames.
constexpr std::int64_t k_fps = Time::flicks_per_second;
FadeParams fade_params() {
  return FadeParams{FadeShape::Linear, FadeWindow{Time{0}, Time{10 * k_fps}},
                    FadeWindow{Time{100 * k_fps}, Time{110 * k_fps}}};
}
const Time k_identity_time{50 * k_fps}; // on the plateau: E == 1

// Fade over an org.arbc.solid: the visual + operator families. The solid is
// UNBOUNDED: SolidContent fills its whole target and trusts the compositor to
// request only in-bounds regions, so a bounded solid does not self-clip and
// fails the suite's out-of-bounds transparency check. Fade is bounds-honest iff
// its input is; that passthrough is asserted directly in the kind's unit test.
struct SolidFixture {
  CpuBackend backend;
  InlineAudioPull pull;
  SolidContent solid{Rgba{0.50F, 0.25F, 0.125F, 1.0F}};

  testing::ContentFactory factory() {
    return [this]() -> std::unique_ptr<Content> {
      auto fade = std::make_unique<FadeContent>(&solid, fade_params());
      fade->attach(pull, backend);
      return fade;
    };
  }
};

// Fade over an org.arbc.tone: the audio-facet gain path over real samples.
struct ToneFixture {
  CpuBackend backend;
  InlineAudioPull pull;
  ToneContent tone{440, 0.5F};

  testing::ContentFactory factory() {
    return [this]() -> std::unique_ptr<Content> {
      auto fade = std::make_unique<FadeContent>(&tone, fade_params());
      fade->attach(pull, backend);
      return fade;
    };
  }
};

} // namespace

TEST_CASE("org.arbc.fade passes the contract conformance suite") {
  SolidFixture fx;
  testing::Options options;
  // The wrapped solid is not editable, so fade's pixels do not vary with the
  // snapshot handle. operator_graph is on (fade is a non-leaf single-input
  // operator; the umbrella auto-skips the leaf check for its non-empty inputs()).
  options.snapshot_sensitive = false;
  options.operator_graph = true;
  arbc::contract_tests(fx.factory(), options);
}

TEST_CASE("org.arbc.fade is a single-input operator") {
  SolidFixture fx;
  const auto fade = fx.factory()();
  REQUIRE_FALSE(fade->inputs().empty());
  REQUIRE(fade->inputs().size() == 1);
}

// enforces: 03-layer-plugin-interface#operator-damage-covers
TEST_CASE("org.arbc.fade covers its input damage") {
  SolidFixture fx;
  // The solid input is snapshot-insensitive, so the before/after render is
  // byte-identical and no output pixel changes -- covering holds vacuously over
  // the (identity) mapped rect. The mapping itself is asserted in the kind's
  // unit test; here the conformance family exercises it over the real operator.
  testing::OperatorDamageCase edit;
  edit.input = 0;
  edit.before = StateHandle{};
  edit.before.slot = 1;
  edit.after = StateHandle{};
  edit.after.slot = 2;
  edit.input_damage = Rect{0.0, 0.0, 4.0, 4.0};
  testing::check_operator_damage_covers(fx.factory(), edit);
}

// enforces: 03-layer-plugin-interface#operator-identity-faithful
TEST_CASE("org.arbc.fade is identity-faithful where its envelope is exactly 1") {
  SolidFixture fx;
  // At a plateau time the envelope is exactly 1, so identity() returns input 0
  // and fade's rendered output is byte-identical to the solid's own output.
  testing::check_operator_identity_faithful(fx.factory(), k_identity_time);
}

// enforces: 13-effects-as-operators#fade-attenuates-both-facets
TEST_CASE("org.arbc.fade passes the audio-facet families over a tone input") {
  ToneFixture fx;
  // factory()->audio() != nullptr, so the audio families apply. Run them over
  // the real fade-of-tone facet: the per-frame gain multiply is deterministic,
  // block-continuous, rate-honest, and silent outside the envelope's extent.
  testing::Options options;
  testing::check_audio_facet_consistency(fx.factory(), options);
  testing::check_audio_async(fx.factory(), options);
}
