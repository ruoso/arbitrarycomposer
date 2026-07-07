#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <unordered_map>

// org.arbc.nested contract conformance (doc 16: content kinds run the public
// arbc-testing suite). The factory produces a fresh NestedContent wired -- via
// the attach seam -- to a fixed small child composition (two org.arbc.solid
// layers) over an inline `PullService` + `CpuBackend`. Cross-component (pulls
// kind_nested + kind_solid + backend-cpu + arbc-testing), so it lives here;
// arbc-testing precedes `arbc` on the link line (static-archive link order).

namespace {

using namespace arbc;

// The inline honoring of the abstract PullService contract (content.hpp:333):
// render `input` into the request's target and settle `done` as Content::render
// does. The fixed child is snapshot-insensitive solids, so this suffices for the
// black-box suite.
class InlinePull final : public PullService {
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
};

// A fixed two-solid child composition plus its content binding, held for the
// whole run so the resolver's `Content*`s and the pinned `DocRoot` outlive every
// factory-produced instance.
struct Fixture {
  Model model;
  CpuBackend backend;
  InlinePull pull;
  SolidContent solid_a{Rgba{0.50F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent solid_b{Rgba{0.20F, 0.40F, 0.10F, 0.75F}, Rect{0.0, 0.0, 8.0, 8.0}};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  DocStatePtr doc;

  Fixture() {
    auto tx = model.transact("child");
    comp = tx.add_composition(8.0, 8.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::translation(1.0, 1.0));
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &solid_a;
    binding[cb] = &solid_b;
    doc = model.current();
  }

  NestedResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }

  testing::ContentFactory factory() {
    return [this]() -> std::unique_ptr<Content> {
      auto nested = std::make_unique<NestedContent>(comp);
      nested->attach(pull, backend, resolver(), *doc);
      return nested;
    };
  }
};

// An audio-routing PullService double: routes `pull_audio` to the child's
// AudioFacet and settles inline (the audio twin of `InlinePull`). The audio
// conformance families over nested reach the child tones through this seam,
// exactly as the mix engine's real `pull_audio` will.
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

// A fixed two-tone child composition: nested-over-tones exposes a real (non-silent)
// aggregated audio facet, so the audio conformance families exercise actual mixing
// rather than trivial silence. The tones are Static, so the aggregate is Static /
// unbounded-extent (coherent for the consistency family).
struct ToneFixture {
  Model model;
  CpuBackend backend;
  InlineAudioPull pull;
  ToneContent tone_a{440, 0.5F};
  ToneContent tone_b{660, 0.25F};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  DocStatePtr doc;

  ToneFixture() {
    auto tx = model.transact("tone child");
    comp = tx.add_composition(0.0, 0.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::identity());
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &tone_a;
    binding[cb] = &tone_b;
    doc = model.current();
  }

  NestedResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }

  testing::ContentFactory factory() {
    return [this]() -> std::unique_ptr<Content> {
      auto nested = std::make_unique<NestedContent>(comp);
      nested->attach(pull, backend, resolver(), *doc);
      return nested;
    };
  }
};

} // namespace

TEST_CASE("org.arbc.nested passes the contract conformance suite") {
  Fixture fx;
  testing::Options options;
  // The fixed child is two org.arbc.solid layers -- not editable -- so nested's
  // pixels do not vary with the snapshot handle (snapshot_sensitive stays false);
  // operator_graph is on (the umbrella auto-skips the leaf check for nested's
  // non-empty inputs()).
  options.snapshot_sensitive = false;
  options.operator_graph = true;
  arbc::contract_tests(fx.factory(), options);
}

TEST_CASE("org.arbc.nested is a non-leaf operator over its child layers") {
  Fixture fx;
  // check_leaf_no_operator_graph is NOT applicable (nested is a non-leaf); assert
  // non-empty inputs() instead (refinement Acceptance criteria).
  const auto nested = fx.factory()();
  REQUIRE_FALSE(nested->inputs().empty());
  REQUIRE(nested->inputs().size() == 2);
}

// enforces: 05-recursive-composition#operator-damage-routes-through-map-input-damage
TEST_CASE("org.arbc.nested operator damage routes through map_input_damage") {
  Fixture fx;
  // The child solids are snapshot-insensitive, so the before/after render is
  // byte-identical and no output pixel changes -- the covering property holds
  // vacuously over the mapped rect. The mapping itself is asserted directly in
  // the kind's own unit test (nested_meta.t.cpp); here the conformance family
  // exercises it over the real operator graph.
  testing::OperatorDamageCase edit;
  edit.input = 0;
  edit.before = StateHandle{};
  edit.before.slot = 1;
  edit.after = StateHandle{};
  edit.after.slot = 2;
  edit.input_damage = Rect{0.0, 0.0, 4.0, 4.0};
  testing::check_operator_damage_covers(fx.factory(), edit);
}

// enforces: 03-layer-plugin-interface#audio-facet-optional
// enforces: 03-layer-plugin-interface#audio-facet-consistent
TEST_CASE("org.arbc.nested passes the audio-facet conformance families over a tone child") {
  ToneFixture fx;
  // Overriding audio() makes factory()->audio() != nullptr, so the audio families
  // apply. Run them directly over the real nested-of-tones facet (aggregated
  // metadata + additive mix through the injected pull_audio); the visual families
  // are covered by the solid-child suite above.
  testing::Options options;
  testing::check_audio_facet_consistency(fx.factory(), options);
  testing::check_audio_async(fx.factory(), options);
}
