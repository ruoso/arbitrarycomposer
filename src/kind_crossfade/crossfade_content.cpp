#include <arbc/base/transform.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace arbc {

namespace {

// Bounding union of two optional rects (constraint 3). A nullopt operand is an
// UNBOUNDED extent and absorbs (union with all-space is all-space); an empty
// rect contributes nothing (identity). Never an intersection. Kept local so the
// kind stays on `contract` alone (doc 17) rather than reaching model's helper.
std::optional<Rect> union_bounds(const std::optional<Rect>& a, const std::optional<Rect>& b) {
  if (!a.has_value() || !b.has_value()) {
    return std::nullopt;
  }
  if (a->empty()) {
    return b;
  }
  if (b->empty()) {
    return a;
  }
  return Rect{std::min(a->x0, b->x0), std::min(a->y0, b->y0), std::max(a->x1, b->x1),
             std::max(a->y1, b->y1)};
}

// Bounding union of two optional time ranges (constraint 3), same absorbing /
// identity rules as union_bounds one dimension down.
std::optional<TimeRange> union_ranges(const std::optional<TimeRange>& a,
                                      const std::optional<TimeRange>& b) {
  if (!a.has_value() || !b.has_value()) {
    return std::nullopt;
  }
  if (a->empty()) {
    return b;
  }
  if (b->empty()) {
    return a;
  }
  return TimeRange{Time{std::min(a->start.flicks, b->start.flicks)},
                  Time{std::max(a->end.flicks, b->end.flicks)}};
}

} // namespace

CrossfadeContent::CrossfadeContent(ContentRef from, ContentRef to, CrossfadeParams params)
    : d_inputs{from, to}, d_params(params) {}

void CrossfadeContent::attach(PullService& pull, Backend& backend) {
  d_pull = &pull;
  d_backend = &backend;
}

// Extent = union of the two inputs' bounds (constraint 3): a crossfade's output
// covers both inputs' footprints. Not fade's single-input pass-through, never an
// intersection.
std::optional<Rect> CrossfadeContent::bounds() const {
  const std::optional<Rect> b0 = d_inputs[0] != nullptr ? d_inputs[0]->bounds() : std::nullopt;
  const std::optional<Rect> b1 = d_inputs[1] != nullptr ? d_inputs[1]->bounds() : std::nullopt;
  return union_bounds(b0, b1);
}

std::optional<TimeRange> CrossfadeContent::time_extent() const {
  const std::optional<TimeRange> t0 =
      d_inputs[0] != nullptr ? d_inputs[0]->time_extent() : std::nullopt;
  const std::optional<TimeRange> t1 =
      d_inputs[1] != nullptr ? d_inputs[1]->time_extent() : std::nullopt;
  return union_ranges(t0, t1);
}

// Timed even over two Static inputs: the position `w(t)` depends on time
// (constraint 4, doc 13:93).
Stability CrossfadeContent::stability() const { return Stability::Timed; }

std::span<const ContentRef> CrossfadeContent::inputs() const { return d_inputs; }

// The crossfade neither moves nor inflates pixels -- both inputs blend in the
// shared output coordinate space -- so damage on either input maps to the
// identical output rect (constraint 7, the contract default). Over-approximation
// is sound; identity is exact here.
Rect CrossfadeContent::map_input_damage(std::size_t /*input*/, const Rect& rect) const {
  return rect;
}

// Endpoint identity (constraint 5, doc 13:59-65): iff `w == 0` exactly the
// output equals input 0's, iff `w == 1` exactly it equals input 1's, so the
// compositor may serve that input's cached tiles directly. This is sound ONLY
// because render() at each endpoint pulls the corresponding input straight
// through (constraint 6). In the interior the output is a genuine blend of both,
// never equal to either input, so identity is nullopt.
std::optional<std::size_t> CrossfadeContent::identity(const RenderRequest& request) const {
  const double w = position(request.time);
  if (w == 0.0) {
    return std::optional<std::size_t>{0};
  }
  if (w == 1.0) {
    return std::optional<std::size_t>{1};
  }
  return std::nullopt;
}

// The position `w(t) = clamp((t - start) / duration, 0, 1)` both facets evaluate
// (Decision 1/3). Before the window `w == 0` (input 0), at/after its end
// `w == 1` (input 1), a linear ramp in between. A degenerate `duration <= 0` is
// a hard cut at `start`. Both endpoints are exact (0.0 / 1.0), so the identity
// condition is a well-defined interval.
double CrossfadeContent::position(Time t) const {
  const std::int64_t dur = d_params.duration.flicks;
  if (dur <= 0) {
    return t < d_params.start ? 0.0 : 1.0; // degenerate: a cut, no dissolve
  }
  if (t < d_params.start) {
    return 0.0;
  }
  const std::int64_t elapsed = t.flicks - d_params.start.flicks;
  if (elapsed >= dur) {
    return 1.0;
  }
  return static_cast<double>(elapsed) / static_cast<double>(dur);
}

namespace {

// Pull `input` straight into the caller's `target` and settle the RenderResult,
// mirroring fade's fully-open pass-through: the sub-request carries snapshot,
// exactness, and deadline VERBATIM (constraint 2); only the target is ours. On a
// worker-dispatched miss or a failed pull, the target is cleared (transparent)
// and a placeholder result is reported for this frame. Returns the pulled input
// result on success (for scale/exactness honesty), nullopt on the placeholder
// path. This is the endpoint render (`w == 0`/`w == 1`) that keeps render()
// bit-faithful to identity() (constraint 6).
std::optional<RenderResult> pull_through(PullService& pull, Backend& backend, ContentRef input,
                                         const RenderRequest& request) {
  auto done = std::make_shared<RenderCompletion>();
  pull.pull(input, request, done);
  if (!done->settled()) {
    done->cancel();
    backend.clear(request.target, 0.0F, 0.0F, 0.0F, 0.0F);
    return std::nullopt;
  }
  const std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    backend.clear(request.target, 0.0F, 0.0F, 0.0F, 0.0F);
    return std::nullopt;
  }
  return **settled;
}

} // namespace

std::optional<RenderResult> CrossfadeContent::render(const RenderRequest& request,
                                                     std::shared_ptr<RenderCompletion> /*done*/) {
  assert(d_pull != nullptr && d_backend != nullptr && "CrossfadeContent rendered before attach");
  Backend& backend = *d_backend;
  Surface& target = request.target;

  const double w = position(request.time);

  // Endpoint pass-through (Decision 2, constraint 6): at w == 0 the output IS
  // input 0's output, at w == 1 it IS input 1's -- pull the corresponding input
  // straight into the caller's target (no temp, no composite), bit-identical to
  // what the compositor serves under identity(). Crossfade is Timed, so it
  // reports the exact request time, never the input's (Static-null) achieved
  // time.
  if (w == 0.0 || w == 1.0) {
    const std::size_t idx = (w == 0.0) ? 0U : 1U;
    const std::optional<RenderResult> in_result = pull_through(*d_pull, backend, d_inputs[idx],
                                                               request);
    if (!in_result.has_value()) {
      return RenderResult{request.scale, true, request.time};
    }
    return RenderResult{in_result->achieved_scale, in_result->exact, request.time};
  }

  // Interior dissolve (Decision 1, constraint 6): pull input 0 into the target as
  // the base, then a single source-over of input 1 over it at opacity w. In
  // premultiplied working space (doc 07) source-over of an opaque source at
  // opacity w yields the textbook linear crossfade in0*(1-w) + in1*w with full
  // alpha -- reusing the exact Backend::composite seam fade uses, no new
  // primitive. The target is cleared first so regions neither input covers stay
  // transparent.
  backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
  const std::optional<RenderResult> r0 = pull_through(*d_pull, backend, d_inputs[0], request);
  if (!r0.has_value()) {
    return RenderResult{request.scale, true, request.time};
  }

  const expected<std::unique_ptr<Surface>, SurfaceError> temp_result =
      backend.make_surface(target.width(), target.height(), target.format());
  if (!temp_result.has_value()) {
    backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F); // cannot store the working format: cull (doc 09)
    return RenderResult{request.scale, true, request.time};
  }
  Surface& temp1 = **temp_result;
  backend.clear(temp1, 0.0F, 0.0F, 0.0F, 0.0F);

  const RenderRequest sub1{request.region, request.scale,     request.time,    request.snapshot,
                           temp1,          request.exactness, request.deadline};
  auto done1 = std::make_shared<RenderCompletion>();
  d_pull->pull(d_inputs[1], sub1, done1);
  if (!done1->settled()) {
    done1->cancel(); // input 1 miss dispatched to a worker: show input 0 alone this frame
    return RenderResult{r0->achieved_scale, r0->exact, request.time};
  }
  const std::optional<expected<RenderResult, RenderError>> settled1 = done1->take();
  if (!settled1.has_value() || !settled1->has_value()) {
    return RenderResult{r0->achieved_scale, r0->exact, request.time}; // input 1 unavailable
  }
  const RenderResult r1 = **settled1;

  // temp1 is in input 1's device raster (achieved_scale); the target is at
  // request.scale over the same region, so the temp1->target map is a pure scale
  // by request.scale/achieved_scale (region translations cancel), the identity at
  // achieved == request.scale (mirrors fade_content.cpp:152-158).
  const Affine temp1_to_dst =
      Affine::scaling(request.scale / r1.achieved_scale, request.scale / r1.achieved_scale);
  backend.composite(target, temp1, temp1_to_dst, w);

  return RenderResult{request.scale, r0->exact && r1.exact, request.time};
}

AudioFacet* CrossfadeContent::audio() { return &d_audio_facet; }

// Audio extent = union of the two inputs' audible extents (constraint 3/4). An
// input with no audio facet contributes silence (an empty, identity operand); a
// Static-audio input (a tone) exists over all time and makes the union unbounded
// (nullopt). Bounded whenever both inputs' audio is bounded -- the case the
// operator is exercised on; two silent/unbounded inputs are the degenerate case
// (like fade's no-op), which the operator is not driven through the suite on.
std::optional<TimeRange> CrossfadeContent::CrossfadeAudioFacet::audio_extent() const {
  CrossfadeContent& self = *d_owner;
  std::optional<TimeRange> acc; // no audible contribution yet
  bool have = false;
  for (ContentRef in : self.d_inputs) {
    AudioFacet* af = in != nullptr ? in->audio() : nullptr;
    if (af == nullptr) {
      continue; // visual-only input: silent, contributes nothing
    }
    const std::optional<TimeRange> e = af->audio_extent();
    if (!e.has_value()) {
      return std::nullopt; // Static audio: exists over all time -> unbounded union
    }
    if (e->empty()) {
      continue;
    }
    acc = have ? union_ranges(acc, e) : e;
    have = true;
  }
  return have ? acc : std::nullopt;
}

Stability CrossfadeContent::CrossfadeAudioFacet::audio_stability() const {
  return Stability::Timed; // w(t) depends on time even over two Static inputs
}

Time CrossfadeContent::CrossfadeAudioFacet::latency() const { return Time::zero(); }

std::optional<AudioResult>
CrossfadeContent::CrossfadeAudioFacet::render_audio(const AudioRequest& request,
                                                    std::shared_ptr<AudioCompletion> /*done*/) {
  CrossfadeContent& self = *d_owner;
  assert(self.d_pull != nullptr && "CrossfadeContent audio rendered before attach");

  const std::uint32_t ch = channel_count(request.layout);
  const std::size_t n = static_cast<std::size_t>(request.target.frames) * ch;

  // Pull input 0 into the caller's block (window, rate, layout, snapshot,
  // exactness VERBATIM -- constraint 2); on failure the block is silence.
  auto done0 = std::make_shared<AudioCompletion>();
  self.d_pull->pull_audio(self.d_inputs[0], request, done0);
  bool s0_ok = false;
  if (done0->settled()) {
    const std::optional<expected<AudioResult, RenderError>> settled0 = done0->take();
    s0_ok = settled0.has_value() && settled0->has_value();
  } else {
    done0->cancel();
  }
  if (!s0_ok) {
    for (std::size_t i = 0; i < n; ++i) {
      request.target.samples[i] = 0.0F;
    }
  }

  // Pull input 1 into a separate local block (same request but our target), so
  // the two signals can be mixed per frame. On failure it stays silence.
  std::vector<float> buf1(n, 0.0F);
  AudioBlock block1{buf1.data(), request.target.frames, request.layout, request.sample_rate};
  const AudioRequest req1{request.window, request.sample_rate, request.layout,
                          block1,         request.exactness,   request.snapshot};
  auto done1 = std::make_shared<AudioCompletion>();
  self.d_pull->pull_audio(self.d_inputs[1], req1, done1);
  if (done1->settled()) {
    done1->take();
  } else {
    done1->cancel();
  }

  // Per-frame complementary-weight additive mix `s0*(1-w) + s1*w` (Decision 1):
  // evaluate the position at each frame's absolute time so the transition ramps
  // smoothly with no zipper. Ordered, deterministic -- audio's native additive
  // combination (doc 00:52-53), one dimension down from the visual dissolve.
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  for (std::uint32_t f = 0; f < request.target.frames; ++f) {
    const Time t_frame{request.window.start.flicks + static_cast<std::int64_t>(f) * fpf};
    const double w = self.position(t_frame);
    const float w0 = static_cast<float>(1.0 - w);
    const float w1 = static_cast<float>(w);
    for (std::uint32_t c = 0; c < ch; ++c) {
      const std::size_t idx = static_cast<std::size_t>(f) * ch + c;
      request.target.samples[idx] = request.target.samples[idx] * w0 + buf1[idx] * w1;
    }
  }
  return AudioResult{request.sample_rate, true};
}

} // namespace arbc
