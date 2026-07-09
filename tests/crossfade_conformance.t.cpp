// org.arbc.crossfade contract conformance (refinement Acceptance "Conformance"):
// wires a testing::ContentFactory over a CrossfadeContent fronted by an inline
// PullService + CpuBackend, then runs the umbrella contract_tests with
// operator_graph on plus the granular operator families
// check_operator_damage_covers / check_operator_identity_faithful. Because
// factory()->audio() != nullptr the umbrella also runs the audio families, and a
// Timed audio facet must declare a bounded audio_extent -- crossfade's extent is
// the pure union of its inputs' (no intrinsic window like fade), so the inputs
// carry bounded audio extents (BoundedAudioLeaf). Mirrors tests/fade_conformance.
//
// Crossfade is the reference TWO-input operator, so the identity leg is taken at
// a time where w == 0 (the endpoint pass-through to input 0), the only sound
// place to claim identity (constraint 5).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace {

using namespace arbc;

constexpr std::int64_t k_fps = Time::flicks_per_second;
// The inputs' audible extent [0, 3s): bounded so crossfade's union extent is
// bounded (the Timed-audio requirement), and past-the-end silent so the suite's
// extent-honesty leg holds.
constexpr std::int64_t k_audio_end = 3 * k_fps;

// A leaf with a real opaque visual (an embedded org.arbc.solid fill) and a
// bounded Timed audio signal. The audio is a per-input DC value inside [0,
// k_audio_end) and silence outside -- a pure function of absolute frame time, so
// it is deterministic and block-continuous, and it declares a bounded extent so
// crossfade's union audio_extent is bounded. Unbounded visual bounds (nullopt)
// so it fills the target and does not self-clip, exactly as SolidContent does.
class BoundedAudioLeaf final : public Content {
public:
  BoundedAudioLeaf(Rgba color, float audio_value) : d_solid(color), d_audio(audio_value) {}

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    return d_solid.render(request, done);
  }
  AudioFacet* audio() override { return &d_audio; }

private:
  class Audio final : public AudioFacet {
  public:
    explicit Audio(float value) : d_value(value) {}
    std::optional<TimeRange> audio_extent() const override {
      return TimeRange{Time::zero(), Time{k_audio_end}};
    }
    Stability audio_stability() const override { return Stability::Timed; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const std::int64_t t = request.window.start.flicks + static_cast<std::int64_t>(f) * fpf;
        const float v = (t >= 0 && t < k_audio_end) ? d_value : 0.0F; // silent past the extent
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    float d_value;
  };

  SolidContent d_solid;
  Audio d_audio;
};

// Route both facets to the wrapped input, exactly as fade_conformance's
// InlineAudioPull: pull() renders the input's visual facet, pull_audio() its
// audio facet.
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

// A crossfade transition over [10s, 20s): the identity leg's time (5s) sits
// before the window (w == 0, endpoint pass-through to input 0), and interior
// request times exercise the dissolve (temp + composite) render path.
CrossfadeParams crossfade_params() {
  return CrossfadeParams{CrossfadeShape::Linear, Time{10 * k_fps}, Time{10 * k_fps}};
}
const Time k_identity_time{5 * k_fps}; // before the window: w == 0

// Crossfade over two distinctly-colored BoundedAudioLeafs: the visual + operator
// + audio families over the real two-input operator, fronted by an inline
// PullService + CpuBackend.
struct Fixture {
  CpuBackend backend;
  InlineAudioPull pull;
  BoundedAudioLeaf from{Rgba{0.50F, 0.25F, 0.125F, 1.0F}, 0.25F};
  BoundedAudioLeaf to{Rgba{0.125F, 0.375F, 0.750F, 1.0F}, -0.50F};

  testing::ContentFactory factory() {
    return [this]() -> std::unique_ptr<Content> {
      auto xf = std::make_unique<CrossfadeContent>(&from, &to, crossfade_params());
      xf->attach(pull, backend);
      return xf;
    };
  }
};

} // namespace

TEST_CASE("org.arbc.crossfade passes the contract conformance suite") {
  Fixture fx;
  testing::Options options;
  // The wrapped inputs are not editable, so crossfade's pixels do not vary with
  // the snapshot handle. operator_graph is on (crossfade is a non-leaf two-input
  // operator; the umbrella auto-skips the leaf check for its non-empty inputs()).
  options.snapshot_sensitive = false;
  options.operator_graph = true;
  arbc::contract_tests(fx.factory(), options);
}

TEST_CASE("org.arbc.crossfade is a two-input operator") {
  Fixture fx;
  const auto xf = fx.factory()();
  REQUIRE(xf->inputs().size() == 2);
}

// enforces: 03-layer-plugin-interface#operator-damage-covers
TEST_CASE("org.arbc.crossfade covers its input damage") {
  Fixture fx;
  // The inputs are snapshot-insensitive, so the before/after render is
  // byte-identical and covering holds over crossfade's (identity) mapped rect.
  testing::OperatorDamageCase edit;
  edit.input = 1;
  edit.before = StateHandle{};
  edit.before.slot = 1;
  edit.after = StateHandle{};
  edit.after.slot = 2;
  edit.input_damage = Rect{0.0, 0.0, 4.0, 4.0};
  testing::check_operator_damage_covers(fx.factory(), edit);
}

// enforces: 03-layer-plugin-interface#operator-identity-faithful
TEST_CASE("org.arbc.crossfade is identity-faithful where w == 0") {
  Fixture fx;
  // Before the window w == 0 exactly, so identity() returns input 0 and
  // crossfade's rendered output is byte-identical to input 0's own output.
  testing::check_operator_identity_faithful(fx.factory(), k_identity_time);
}

// enforces: 13-effects-as-operators#crossfade-mixes-both-facets
TEST_CASE("org.arbc.crossfade passes the audio-facet families over bounded-audio inputs") {
  Fixture fx;
  // factory()->audio() != nullptr, so the audio families apply. The per-frame
  // complementary-weight mix of two block-continuous inputs is deterministic,
  // block-continuous, rate-honest, and silent past the union extent.
  testing::Options options;
  testing::check_audio_facet_consistency(fx.factory(), options);
  testing::check_audio_async(fx.factory(), options);
}
