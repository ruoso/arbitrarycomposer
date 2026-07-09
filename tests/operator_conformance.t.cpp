// Canonical operator conformance driver (refinement
// operators.operator_conformance, Acceptance "Canonical operator conformance
// driver"): the operator-agnostic collection point every reference operator kind
// plugs into. For each reference operator factory (fade today; crossfade appends
// its own factory line in its own task) it runs the three suite-level checks
// doc 13's operator contract promises:
//   1. identity honesty  -- check_operator_identity_faithful  (arbc-testing)
//   2. damage-mapping honesty -- check_operator_damage_covers (arbc-testing)
//   3. pull routing      -- check_operator_pulls_via_service  (this task's L4
//                           helper + new claim; the one net-new property)
// plus the leaf negative case check_leaf_no_operator_graph. Legs 1-2 re-assert
// the already-shipped arbc-testing families over the reference operators (they
// do NOT re-register or re-implement them, Decision 5); leg 3 is the new claim.
// Minor overlap with tests/fade_conformance.t.cpp is deliberate: that driver is
// fade-task-local coverage, this one is the operator-agnostic home (Decision 4).

#include "operator_conformance.hpp"

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <span>

namespace {

using namespace arbc;

// Route both facets to the wrapped input, exactly as fade_conformance's
// InlineAudioPull: the identity/damage legs drive the real fade over a real
// input, so its pull must actually render the input (unlike the pull-routing
// leg's canned RecordingPull, which must NOT touch the input).
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

// A symmetric fade with a plateau (E == 1) over [10s, 100s]: identity() is a
// well-defined interval and the mid-fade pull-routing time (5s) sits on the
// ramp, so both the isolation and integration facets exercise fade's partial
// (temp + composite) render path. Matches fade_conformance's parameters.
constexpr std::int64_t k_fps = Time::flicks_per_second;
FadeParams fade_params() {
  return FadeParams{FadeShape::Linear, FadeWindow{Time{0}, Time{10 * k_fps}},
                    FadeWindow{Time{100 * k_fps}, Time{110 * k_fps}}};
}
const Time k_identity_time{50 * k_fps}; // on the plateau: E == 1

// The identity/damage legs need a real fade over a real input fronted by an
// inline PullService + CpuBackend (mirrors fade_conformance's SolidFixture). The
// solid is UNBOUNDED so it does not self-clip; fade is bounds-honest iff its
// input is (asserted in the fade kind's own unit test, not re-checked here).
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

// The pull-routing leg builds the operator over the helper's poison-leaf inputs,
// attached to whichever service (RecordingPull or the live PullServiceImpl) the
// helper supplies. Operator-agnostic shape: crossfade appends an analogous
// factory (wrapping inputs[0]/inputs[1]) in its own task.
optest::OperatorPullFactory fade_pull_factory() {
  return [](std::span<const ContentRef> inputs, PullService& pull,
            Backend& backend) -> std::unique_ptr<Content> {
    auto fade = std::make_unique<FadeContent>(inputs[0], fade_params());
    fade->attach(pull, backend);
    return fade;
  };
}

} // namespace

// enforces: 03-layer-plugin-interface#operator-identity-faithful
TEST_CASE("operator conformance: org.arbc.fade is identity-faithful") {
  SolidFixture fx;
  // At a plateau time the envelope is exactly 1, so identity() returns input 0
  // and fade's output is byte-identical to the input's own output.
  testing::check_operator_identity_faithful(fx.factory(), k_identity_time);
}

// enforces: 03-layer-plugin-interface#operator-damage-covers
TEST_CASE("operator conformance: org.arbc.fade covers its input damage") {
  SolidFixture fx;
  // The solid input is snapshot-insensitive, so the before/after render is
  // byte-identical and covering holds over fade's (identity) mapped rect.
  testing::OperatorDamageCase edit;
  edit.input = 0;
  edit.before = StateHandle{};
  edit.before.slot = 1;
  edit.after = StateHandle{};
  edit.after.slot = 2;
  edit.input_damage = Rect{0.0, 0.0, 4.0, 4.0};
  testing::check_operator_damage_covers(fx.factory(), edit);
}

// enforces: 13-effects-as-operators#operator-pulls-only-via-pull-service
TEST_CASE("operator conformance: org.arbc.fade pulls its input only via PullService") {
  // Both facets, both the isolation (RecordingPull + PoisonLeaf) and integration
  // (live PullServiceImpl + CompositorCounters) proofs. Fade exposes audio(), so
  // the audio (pull_audio / audio_dispatches) leg runs too.
  optest::check_operator_pulls_via_service(fade_pull_factory());
}

// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
TEST_CASE("operator conformance: a leaf content has no operator graph") {
  // Pin the negative case: an org.arbc.solid leaf exposes an empty inputs() span,
  // nullopt identity() for every request, and the identity map_input_damage.
  const testing::ContentFactory leaf = []() -> std::unique_ptr<Content> {
    return std::make_unique<SolidContent>(Rgba{0.25F, 0.50F, 0.75F, 1.0F});
  };
  testing::check_leaf_no_operator_graph(leaf);
}
