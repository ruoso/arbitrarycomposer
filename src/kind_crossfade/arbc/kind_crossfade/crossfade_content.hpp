#pragma once

#include <arbc/contract/content.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace arbc {

class Backend; // L2 seam, reached through contract's transitive closure

// Crossfade position interpolation shape. Linear is the only v1 shape doc 13
// names (refinement Decision 3); unknown shapes fail closed so a later shape is
// an additive change.
enum class CrossfadeShape { Linear };

// Immutable crossfade transition params (refinement Decision 3). The position
// `w(t) = clamp((t - start) / duration, 0, 1)` ramps 0->1 across the half-open
// flick window [start, start + duration): before it `w == 0` (input 0), after
// it `w == 1` (input 1). A degenerate `duration <= 0` is a hard cut at `start`.
// Serialization-ready but not itself serialized here (Decision 1 / out of
// scope: serialize.kind_params, M7).
struct CrossfadeParams {
  CrossfadeShape shape{CrossfadeShape::Linear};
  Time start{};
  Time duration{};
};

// Reference kind org.arbc.crossfade (doc 13:168): the two-input temporal
// transition operator. It wraps two inputs (`from`, `to`) and, driven by a
// single time-evaluated position `w(t) in [0,1]`, presents a blend of the two
// as ordinary content across BOTH facets (doc 13:24-27): visually a source-over
// dissolve of input 1 over input 0 at opacity `w` (the exact seam fade uses),
// aurally a per-frame complementary-weight additive mix `s0*(1-w) + s1*w`
// (audio's native combination, doc 00:52-53). Its output extent is the UNION of
// its inputs' extents (doc 13:91-96). It is `Timed` even over two `Static`
// inputs, because `w(t)` depends on time (doc 13:93), and it declares
// `identity()` at the endpoints (`{0}` at `w == 0`, `{1}` at `w == 1`), so the
// compositor serves the corresponding input's cached tiles with no render
// outside the transition window (doc 13:59-65). It pulls each input ONLY through
// the injected PullService, never `input->render()`.
class CrossfadeContent final : public Content {
public:
  CrossfadeContent(ContentRef from, ContentRef to, CrossfadeParams params);

  // Attach seam (mirrors FadeContent::attach): crossfade borrows the pull
  // service and backend, owning neither. The runtime binds a live
  // `PullServiceImpl`/`Backend` here at instantiation
  // (`operators.crossfade_runtime_binding`); tests inject inline doubles.
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

  // Spatial/temporal metadata: bounds() and time_extent() return the UNION of
  // the two inputs' respective extents (constraint 3). stability() is `Timed`
  // regardless of the inputs (constraint 4).
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

  // Position value `w(t) in [0,1]` -- both facets evaluate this one function
  // (Decision 1). Exposed for the goldens and the boundary unit test. Exact 0.0
  // before the window and exact 1.0 at/after its end, so the endpoint identity
  // condition is a well-defined interval.
  double position(Time t) const;

  // The immutable construction params, exposed for serialization: the crossfade
  // codec (runtime.operator_codecs) reads `{ shape, start, duration }` back off the
  // live operator to emit its `params` JSON. Reconstructing these from sampled
  // `position()` values is lossy/non-invertible, so the raw struct is exposed
  // directly -- the additive const accessor move solid/tone made for
  // color()/frequency_hz(). Adds no dependency and changes no behavior.
  const CrossfadeParams& params() const { return d_params; }

  static constexpr const char* kind_id = "org.arbc.crossfade";

private:
  // Inner audio facet holding the same position evaluator as the visual path:
  // a per-frame complementary-weight additive mix of the two settled inputs.
  class CrossfadeAudioFacet final : public AudioFacet {
  public:
    explicit CrossfadeAudioFacet(CrossfadeContent* owner) : d_owner(owner) {}

    std::optional<TimeRange> audio_extent() const override;
    Stability audio_stability() const override;
    Time latency() const override; // pure mix: Time::zero()
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion> done) override;

  private:
    CrossfadeContent* d_owner;
  };

  std::array<ContentRef, 2> d_inputs; // stable storage backing inputs(): {from, to}
  CrossfadeParams d_params;
  PullService* d_pull{nullptr};
  Backend* d_backend{nullptr};
  CrossfadeAudioFacet d_audio_facet{this};
};

} // namespace arbc
