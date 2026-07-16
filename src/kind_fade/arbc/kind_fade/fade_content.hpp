#pragma once

#include <arbc/arbc_api.h>
#include <arbc/contract/content.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace arbc {

class Backend; // L2 seam, reached through contract's transitive closure

// Fade envelope interpolation shape. Linear is the only v1 shape doc 13 names
// (refinement Decision 3); unknown shapes fail closed so a later shape is an
// additive change.
enum class FadeShape { Linear };

// One monotonic fade ramp over absolute content-local time. `end > start`; the
// ramp spans the half-open flick interval [start, end).
struct FadeWindow {
  Time start{};
  Time end{};
};

// Immutable fade envelope params, the in-memory shape of doc 13:145-153's
// `{ shape, in, out }` (refinement Decision 2/8). `in` ramps the envelope
// 0->1 across its window; `out` ramps 1->0 across its window; either may be
// absent. Serialization-ready but not itself serialized here (Decision 8).
struct FadeParams {
  FadeShape shape{FadeShape::Linear};
  std::optional<FadeWindow> in{};
  std::optional<FadeWindow> out{};
};

// Reference kind org.arbc.fade (doc 13:167): a single-input operator that
// attenuates its input on BOTH facets by one time-evaluated envelope
// `E(t) in [0,1]` -- visual alpha (a premultiplied-RGBA scale) and audio gain
// fade coherently through one node (doc 13:24-27). Fade is `Timed` even over a
// `Static` input, because its envelope depends on time (doc 13:93-95), and it
// declares `identity()` on the visual facet exactly where `E == 1`, so the
// compositor serves the input's cached tiles with no render outside the fade
// window (doc 13:59-65, 128). It pulls its input ONLY through the injected
// PullService, never `input->render()`.
class ARBC_API FadeContent final : public Content {
public:
  FadeContent(ContentRef input, FadeParams params);

  // Attach seam (mirrors NestedContent::attach): fade borrows the pull service
  // and backend, owning neither. Production wiring is `operators.fade_runtime_binding`
  // (the runtime binds a live `PullServiceImpl`/`Backend` here at instantiation);
  // tests inject inline doubles.
  void attach(PullService& pull, Backend& backend);

  // Teardown twin of `attach` (Constraint 3): clear the borrowed pointers so no
  // render after the binding scope ends dereferences a released service. Called by
  // the runtime binder on scope exit; a subsequent `render`/`render_audio` asserts
  // (unattached) rather than touching a dangling service.
  void detach() noexcept;

  // Whether a live service is currently borrowed (both pointers set). Observability
  // for the runtime binding's teardown assertion; adds no dependency, changes no
  // behavior.
  bool attached() const noexcept;

  // Spatial/temporal identity: bounds() and time_extent() pass the input's
  // through unchanged. stability() is `Timed` regardless of the input.
  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  AudioFacet* audio() override;

  // Operator-graph members (doc 13:39-67).
  std::span<const ContentRef> inputs() const override;
  Rect map_input_damage(std::size_t input, const Rect& rect) const override;
  std::optional<std::size_t> identity(const RenderRequest& request) const override;

  // Envelope value `E(t) in [0,1]` -- both facets evaluate this one function
  // (Decision 2). Exposed for the goldens and the boundary unit test.
  double envelope(Time t) const;

  // The immutable construction params, exposed for serialization: the fade codec
  // (runtime.operator_codecs) reads `{ shape, in, out }` back off the live operator
  // to emit its `params` JSON. Reconstructing these from sampled `envelope()` values
  // is lossy/non-invertible, so the raw struct is exposed directly -- the additive
  // const accessor move solid/tone made for color()/frequency_hz(). Adds no
  // dependency and changes no behavior.
  const FadeParams& params() const { return d_params; }

  static constexpr const char* kind_id = "org.arbc.fade";

private:
  // Inner audio facet holding the same envelope evaluator as the visual path
  // (Decision 9): a per-frame gain multiply in place after the input settles.
  class FadeAudioFacet final : public AudioFacet {
  public:
    explicit FadeAudioFacet(FadeContent* owner) : d_owner(owner) {}

    std::optional<TimeRange> audio_extent() const override;
    Stability audio_stability() const override;
    Time latency() const override; // pure gain: Time::zero()
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion> done) override;

  private:
    FadeContent* d_owner;
  };

  ContentRef d_input;
  FadeParams d_params;
  std::array<ContentRef, 1> d_inputs; // stable storage backing inputs()
  PullService* d_pull{nullptr};
  Backend* d_backend{nullptr};
  FadeAudioFacet d_audio_facet{this};
};

} // namespace arbc
