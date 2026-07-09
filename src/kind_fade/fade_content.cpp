#include <arbc/kind_fade/fade_content.hpp>

#include <arbc/base/transform.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

namespace arbc {

FadeContent::FadeContent(ContentRef input, FadeParams params)
    : d_input(input), d_params(params), d_inputs{input} {}

void FadeContent::attach(PullService& pull, Backend& backend) {
  d_pull = &pull;
  d_backend = &backend;
}

// Spatial/temporal identity (constraint 8): the fade neither moves nor scales
// pixels nor shifts time, so bounds() and time_extent() pass the input's
// through unchanged.
std::optional<Rect> FadeContent::bounds() const {
  return d_input != nullptr ? d_input->bounds() : std::nullopt;
}

std::optional<TimeRange> FadeContent::time_extent() const {
  return d_input != nullptr ? d_input->time_extent() : std::nullopt;
}

// Timed even over a Static input: the envelope depends on time (doc 13:93-95).
Stability FadeContent::stability() const { return Stability::Timed; }

std::span<const ContentRef> FadeContent::inputs() const { return d_inputs; }

// The fade neither moves nor inflates pixels, so damage on its input maps to the
// identical output rect (constraint 6, the reciprocal of "pulls the same
// region"). Over-approximation is sound; identity is exact here.
Rect FadeContent::map_input_damage(std::size_t /*input*/, const Rect& rect) const { return rect; }

// Visual-facet identity is exact-`E == 1` (Decision 6): iff the envelope
// evaluates to exactly 1.0 at the request time, the output equals input 0's
// output and the compositor may serve it directly. At E == 0 the output is
// transparent (NOT equal to input 0), so it is not identity.
std::optional<std::size_t> FadeContent::identity(const RenderRequest& request) const {
  return envelope(request.time) == 1.0 ? std::optional<std::size_t>{0} : std::nullopt;
}

// The envelope `E(t) in [0,1]` both facets evaluate (Decision 2). The `in`
// window ramps 0->1, the `out` window ramps 1->0, and the two factors multiply:
// before/after the outer edges E is 0, between the windows E is 1. Linear is the
// only v1 shape. All boundary values (E == 1 on the plateau) are exact, so the
// identity condition is a well-defined interval.
double FadeContent::envelope(Time t) const {
  double e = 1.0;
  if (d_params.in.has_value()) {
    const FadeWindow& w = *d_params.in;
    if (t < w.start) {
      e = 0.0; // before the fade-in: fully closed
    } else if (t < w.end) {
      e = static_cast<double>(t.flicks - w.start.flicks) /
          static_cast<double>(w.end.flicks - w.start.flicks); // ramp 0->1
    }
    // else: t >= in.end -> e stays 1.0 (exact)
  }
  if (d_params.out.has_value()) {
    const FadeWindow& w = *d_params.out;
    if (t < w.start) {
      // before the fade-out: factor 1.0, leave e unchanged
    } else if (t < w.end) {
      e *= 1.0 - static_cast<double>(t.flicks - w.start.flicks) /
                     static_cast<double>(w.end.flicks - w.start.flicks); // ramp 1->0
    } else {
      e = 0.0; // after the fade-out: fully closed
    }
  }
  return e;
}

std::optional<RenderResult> FadeContent::render(const RenderRequest& request,
                                                std::shared_ptr<RenderCompletion> /*done*/) {
  assert(d_pull != nullptr && d_backend != nullptr && "FadeContent rendered before attach");
  Backend& backend = *d_backend;
  Surface& target = request.target;

  const double e = envelope(request.time);

  // Fully-open envelope: the output IS input 0's output (Decision 6). Pull the
  // input straight into the caller's target -- a bit-identical pass-through with
  // no temp and no composite, keeping render() faithful to identity() when the
  // compositor renders it anyway. The sub-request carries snapshot, exactness,
  // and deadline VERBATIM (constraint 2, doc 05:96-100); only the target is ours.
  if (e == 1.0) {
    const RenderRequest sub{request.region,   request.scale,     request.time,    request.snapshot,
                            target,           request.exactness, request.deadline};
    auto done = std::make_shared<RenderCompletion>();
    d_pull->pull(d_input, sub, done);
    if (!done->settled()) {
      done->cancel(); // worker-dispatched miss: placeholder for this frame
      backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
      return RenderResult{request.scale, true, request.time};
    }
    const std::optional<expected<RenderResult, RenderError>> settled = done->take();
    if (!settled.has_value() || !settled->has_value()) {
      backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
      return RenderResult{request.scale, true, request.time};
    }
    // Bit-identical pass-through of input 0's pixels; but fade is Timed, so it
    // reports the exact time it rendered (request.time), never the input's
    // (Static-null) achieved_time -- the suite's Timed time-honesty rule.
    const RenderResult in_result = **settled;
    return RenderResult{in_result.achieved_scale, in_result.exact, request.time};
  }

  // Partial (or fully-closed) envelope: pull the input at the same region, scale,
  // and time (doc 13:103-113) into a temp surface, then attenuate by compositing
  // it onto a cleared target at opacity E. Premultiplied working space (doc 07,
  // `#compositing-in-working-space`) makes source-over onto transparent at
  // opacity E yield exactly `src * E` -- the alpha fade -- reusing the L2
  // Backend::composite seam (Decision 4).
  const expected<std::unique_ptr<Surface>, SurfaceError> temp_result =
      backend.make_surface(target.width(), target.height(), target.format());
  if (!temp_result.has_value()) {
    backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F); // cannot store the working format: cull (doc 09)
    return RenderResult{request.scale, true, request.time};
  }
  Surface& temp = **temp_result;
  backend.clear(temp, 0.0F, 0.0F, 0.0F, 0.0F);

  const RenderRequest sub{request.region,   request.scale,     request.time,    request.snapshot,
                          temp,             request.exactness, request.deadline};
  auto done = std::make_shared<RenderCompletion>();
  d_pull->pull(d_input, sub, done);
  if (!done->settled()) {
    done->cancel(); // cache miss dispatched to a worker: placeholder (transparent) this frame
    backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
    return RenderResult{request.scale, true, request.time};
  }
  const std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F); // budget-exceeded / failed pull: placeholder
    return RenderResult{request.scale, true, request.time};
  }
  const RenderResult result = **settled;

  backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
  // temp is in the input's device raster (achieved_scale); the target is at
  // request.scale over the same region, so the temp->target map is a pure scale
  // by request.scale/achieved_scale (the region translations cancel). At
  // achieved == request.scale this is the identity.
  const Affine temp_to_dst =
      Affine::scaling(request.scale / result.achieved_scale, request.scale / result.achieved_scale);
  backend.composite(target, temp, temp_to_dst, e);

  // Scale/exactness pass through honestly (doc 13:108-113): fade adds only a
  // gain, so it reports the input's achieved scale and exactness. Time, however,
  // is fade's own: it is Timed and honors the exact request time (no
  // quantization), so it reports request.time -- never the input's null.
  return RenderResult{result.achieved_scale, result.exact, request.time};
}

AudioFacet* FadeContent::audio() { return &d_audio_facet; }

// Audio stability is Timed (the gain depends on time) and latency is zero (pure
// gain, no delay -- doc 12; constraint 8). The audible extent is the input's,
// tightened by the envelope's silent support: the fade is silent (gain 0) before
// `in.start` and at/after `out.end`, so those windows bound the audible region.
// A Timed facet must declare an extent (doc 12:26-29); a fade whose envelope
// closes at neither end over an unbounded input is the sole unbounded (nullopt)
// case -- a degenerate no-op fade the operator is not exercised on.
std::optional<TimeRange> FadeContent::FadeAudioFacet::audio_extent() const {
  FadeContent& self = *d_owner;
  std::int64_t lo = std::numeric_limits<std::int64_t>::min();
  std::int64_t hi = std::numeric_limits<std::int64_t>::max();
  if (self.d_params.in.has_value()) {
    lo = self.d_params.in->start.flicks; // silent before the fade-in opens
  }
  if (self.d_params.out.has_value()) {
    hi = self.d_params.out->end.flicks; // silent once the fade-out closes
  }
  if (self.d_input != nullptr) {
    if (AudioFacet* af = self.d_input->audio(); af != nullptr) {
      if (const std::optional<TimeRange> ie = af->audio_extent(); ie.has_value()) {
        lo = std::max(lo, ie->start.flicks);
        hi = std::min(hi, ie->end.flicks);
      }
    }
  }
  if (lo == std::numeric_limits<std::int64_t>::min() &&
      hi == std::numeric_limits<std::int64_t>::max()) {
    return std::nullopt; // envelope never closes and input is unbounded
  }
  return TimeRange{Time{lo}, Time{hi}};
}

Stability FadeContent::FadeAudioFacet::audio_stability() const { return Stability::Timed; }

Time FadeContent::FadeAudioFacet::latency() const { return Time::zero(); }

std::optional<AudioResult>
FadeContent::FadeAudioFacet::render_audio(const AudioRequest& request,
                                          std::shared_ptr<AudioCompletion> /*done*/) {
  FadeContent& self = *d_owner;
  assert(self.d_pull != nullptr && "FadeContent audio rendered before attach");

  const std::uint32_t ch = channel_count(request.layout);
  const std::size_t n = static_cast<std::size_t>(request.target.frames) * ch;

  // Pull the input into the caller's block, carrying window, rate, layout, and
  // snapshot/exactness VERBATIM (constraint 2). The gain then multiplies in place
  // -- the in-place twin of the nested additive mix (Decision 5).
  auto done = std::make_shared<AudioCompletion>();
  self.d_pull->pull_audio(self.d_input, request, done);
  if (!done->settled()) {
    done->cancel(); // worker-dispatched miss: silence for this block (constraint 3)
    for (std::size_t i = 0; i < n; ++i) {
      request.target.samples[i] = 0.0F;
    }
    return AudioResult{request.sample_rate, true};
  }
  const std::optional<expected<AudioResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    for (std::size_t i = 0; i < n; ++i) {
      request.target.samples[i] = 0.0F; // budget-exceeded / unavailable pull: silence
    }
    return AudioResult{request.sample_rate, true};
  }
  const AudioResult ar = **settled;

  // Per-frame gain: evaluate the envelope at each frame's absolute time so a
  // window straddling the ramp produces a smooth per-frame ramp with no zipper
  // (Decision 5). Ordered, deterministic -- no unordered accumulation.
  const std::int64_t fpf =
      Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  for (std::uint32_t f = 0; f < request.target.frames; ++f) {
    const Time t_frame{request.window.start.flicks + static_cast<std::int64_t>(f) * fpf};
    const float gain = static_cast<float>(self.envelope(t_frame));
    for (std::uint32_t c = 0; c < ch; ++c) {
      request.target.samples[static_cast<std::size_t>(f) * ch + c] *= gain;
    }
  }
  return ar;
}

} // namespace arbc
