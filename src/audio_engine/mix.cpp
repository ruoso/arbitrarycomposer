#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_resampler.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace arbc {
namespace {

// Mix one layer's contribution into `request.target` (doc 12:11-21,128-130). A
// standalone re-expression of `NestedContent::mix_child_layer`
// (`nested_content.cpp:394-567`), one level up: `kind-nested` is L3 and cannot
// call this L4 engine, so the per-layer descent is duplicated between the engine
// (root composition, monitor-driven) and nested (child composition, recursion) --
// exactly as `render_frame` and nested's visual `compose_child_layer` duplicate
// the video walk (doc 17:41 Decision). The heavy machinery (`PullService`, the
// block cache, `resample_audio`) is shared; this is the thin cull loop.
void mix_layer(const LayerRecord& layer, const CompositionRecord& comp, const MixResolver& resolve,
               PullService& pull, const AudioRequest& request, MixPolicy policy,
               std::uint32_t& achieved, bool& exact) {
  // Audio visibility cull (doc 12:86-87,129-130): an inaudible or zero-gain layer
  // contributes nothing.
  if (!layer.audible() || layer.gain <= 0.0) {
    return;
  }
  Content* content = resolve ? resolve(layer.content) : nullptr;
  if (content == nullptr) {
    return; // unresolved layer: silence for this layer (doc 05:50)
  }
  AudioFacet* af = content->audio();
  if (af == nullptr) {
    return; // no audio facet: skipped at zero cost (doc 12:86-87)
  }

  // Span cull (doc 11:62-73): a degenerate span, or one that does not overlap the
  // request window, contributes nothing.
  if (layer.span.empty() || request.window.end.flicks <= layer.span.start.flicks ||
      layer.span.end.flicks <= request.window.start.flicks) {
    return;
  }

  // Varispeed (doc 12:107-118): request the child at the composed rational rate
  // `child_rate = request.sample_rate / rate` (rate = num/den), so a rate-1/2
  // layer requests at twice the rate and pitches the child down an octave. The
  // rate is recomputed from the per-edge rational every descent -- never
  // accumulated (doc 11:187-188). Reverse / zero-rate audio (num <= 0) is out of
  // scope (deferred with time-stretch, doc 12:117-118): cull rather than mis-mix.
  const std::int64_t num = layer.time_map.rate.num();
  const std::int64_t den = layer.time_map.rate.den();
  if (num <= 0) {
    return;
  }
  const std::uint32_t child_rate =
      static_cast<std::uint32_t>(static_cast<std::int64_t>(request.sample_rate) * den / num);
  if (child_rate == 0) {
    return;
  }

  // Child-local window start: the parent window start mapped through the layer
  // time map (doc 11:66-71). A procedural child then samples frame f at
  // child_start + f * flicks_per_second/child_rate == the time-mapped parent frame
  // instant, so a composition-of-tones scene is byte-exact.
  const expected<Time, TimeError> child_start = layer.time_map.evaluate(request.window.start);
  if (!child_start.has_value()) {
    return; // time-map overflow: cull (doc 11:52)
  }

  // Request the child at its composition's working layout (the boundary converts,
  // doc 12:95-105) and the composed rate. A homogeneous tree pays nothing (child
  // layout == request layout -> a direct mix); a layout mismatch is remixed inline.
  const std::uint32_t out_ch = channel_count(request.layout);
  const ChannelLayout child_layout = comp.working_audio_format.layout;
  const std::uint32_t in_ch = channel_count(child_layout);
  const std::uint32_t frames = request.target.frames;
  const std::int64_t fpf_child = Time::flicks_per_second / static_cast<std::int64_t>(child_rate);

  std::vector<float> child_buf(static_cast<std::size_t>(frames) * in_ch, 0.0F);
  AudioBlock child_block{child_buf.data(), frames, child_layout, child_rate};
  const AudioRequest child_req{
      TimeRange{*child_start,
                Time{child_start->flicks + static_cast<std::int64_t>(frames) * fpf_child}},
      child_rate,
      child_layout,
      child_block,
      request.exactness, // carried verbatim (doc 12 Decision 5)
      request.snapshot,  // carried verbatim (doc 05:96-100)
  };

  // Pull through the injected service, NEVER `af->render_audio` directly: the
  // block-cache serve, worker dispatch, and recursion-depth backstop are the
  // service's, and audio "renders ahead" so plugin code must be dispatchable off
  // the RT thread (doc 12:31-34,154-164; the deliberate audio/video asymmetry).
  auto done = std::make_shared<AudioCompletion>();
  pull.pull_audio(content, child_req, done);
  if (!done->settled()) {
    // The service deferred to a worker (a miss): this pass mixes silence for the
    // layer (doc 05:50-52) and cancels the completion; priming those blocks so the
    // pass is all-hits is the `lookahead` leaf's job.
    done->cancel();
    return;
  }
  const std::optional<expected<AudioResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return; // budget-exceeded / unavailable pull (a Droste backstop): silence
  }
  const AudioResult cr = **settled;

  // Below-rate reconstruction (doc 12:24-25,100-104). A child that bottoms out
  // below the composed rate conveyed only `achieved_rate` of genuine information
  // in the caller-sized discovery block. Rather than read that baseline hold, re-
  // request the child's GENUINE native samples over the same child-local window at
  // its native rate (a second, block-cache-served pull) and band-limit-reconstruct
  // them up to `child_rate` with the deterministic `arbc::media` windowed-sinc
  // polyphase kernel -- exactly `frames` samples that feed the UNCHANGED 1:1
  // additive mix below. A rate-honoring child skips this entirely. The kernel
  // improves the sample VALUES only; the achieved_rate/exact honesty math below is
  // untouched, never fabricated.
  const float* mix_src = child_buf.data();
  std::vector<float> resampled_buf;
  if (cr.achieved_rate > 0 && cr.achieved_rate < child_rate) {
    const std::uint32_t native_rate = cr.achieved_rate;
    const std::int64_t fpf_native =
        Time::flicks_per_second / static_cast<std::int64_t>(native_rate);
    // Native frames spanning the same child-local window at the native rate. Output
    // frame n reads native position `n * native_rate / child_rate` (< frames), so
    // this count covers every in-range tap; taps past the block read as zero (the
    // kernel's defined edge convention). One rounding at the leaf is the kernel's
    // (doc 11:187-188) -- the rate is never accumulated across depth.
    const std::uint32_t native_frames = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(frames) * native_rate / child_rate + 1);
    std::vector<float> native_buf(static_cast<std::size_t>(native_frames) * in_ch, 0.0F);
    AudioBlock native_block{native_buf.data(), native_frames, child_layout, native_rate};
    const AudioRequest native_req{
        TimeRange{*child_start, Time{child_start->flicks +
                                     static_cast<std::int64_t>(native_frames) * fpf_native}},
        native_rate,
        child_layout,
        native_block,
        request.exactness, // carried verbatim
        request.snapshot,  // carried verbatim
    };
    auto native_done = std::make_shared<AudioCompletion>();
    pull.pull_audio(content, native_req, native_done);
    if (native_done->settled()) {
      const std::optional<expected<AudioResult, RenderError>> native_settled = native_done->take();
      if (native_settled.has_value() && native_settled->has_value()) {
        resampled_buf.assign(static_cast<std::size_t>(frames) * in_ch, 0.0F);
        AudioBlock resampled_block{resampled_buf.data(), frames, child_layout, child_rate};
        resample_audio(native_block, resampled_block);
        mix_src = resampled_buf.data();
      }
    } else {
      native_done->cancel(); // a deferred native pull: keep the baseline block
    }
  }

  // Additive Flat-mode mix (doc 12:127-130): contribution = gain * child, summed
  // into the target, remixed to the request layout. Placement is 1:1 -- `mix_src`
  // carries exactly `frames` samples at `child_rate` (the honoring child's block,
  // or the reconstructed block above). The `MixPolicy` seam keeps the Spatial
  // policy's pan/attenuation/sub-audible-cull branch additive (`audio.spatial_policy`);
  // only `Flat` is implemented here.
  (void)policy;
  const float gain = static_cast<float>(layer.gain);
  for (std::uint32_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < out_ch; ++c) {
      float s = 0.0F;
      if (in_ch == out_ch) {
        s = mix_src[static_cast<std::size_t>(f) * in_ch + c];
      } else if (in_ch == 1) {
        s = mix_src[f]; // mono child -> every request channel
      } else {
        // stereo child -> mono request: average the channels (baseline downmix).
        s = 0.5F * (mix_src[static_cast<std::size_t>(f) * 2] +
                    mix_src[static_cast<std::size_t>(f) * 2 + 1]);
      }
      request.target.samples[static_cast<std::size_t>(f) * out_ch + c] += gain * s;
    }
  }

  // achieved_rate / exact honesty (doc 12 rate-honesty). A child honoring the
  // composed rate keeps the boundary at the request rate; a below-rate child
  // (whose samples are now band-limit-reconstructed above, not held) still lowers
  // the aggregate and marks it inexact -- the reconstruction improves fidelity but
  // creates no information, so it never raises achieved_rate toward the request
  // rate nor reports exact.
  if (!cr.exact || cr.achieved_rate != child_rate) {
    exact = false;
    const std::uint64_t eff =
        static_cast<std::uint64_t>(cr.achieved_rate) * request.sample_rate / child_rate;
    achieved = std::min(achieved, static_cast<std::uint32_t>(eff));
  }
}

} // namespace

AudioResult mix_composition(const DocRoot& doc, ObjectId composition, const MixResolver& resolve,
                            PullService& pull, const AudioRequest& request, MixPolicy policy) {
  // The mixed block is an ordinary content's samples (doc 12:53,202-208): start
  // from silence, then additively mix each audible layer.
  const std::uint32_t ch = channel_count(request.layout);
  const std::size_t n = static_cast<std::size_t>(request.target.frames) * ch;
  for (std::size_t i = 0; i < n; ++i) {
    request.target.samples[i] = 0.0F;
  }

  const CompositionRecord* comp = doc.find_composition(composition);
  if (comp == nullptr) {
    // Unresolved / not-yet-loaded composition (doc 05:50-52): a silent block,
    // honest at the request rate.
    return AudioResult{request.sample_rate, true};
  }

  // achieved_rate = min over contributing layers (a honoring layer keeps it at the
  // request rate); exact = the conjunction. No contributor -> a faithful silent
  // block at the request rate. Membership is read from the frozen pin (doc
  // 02/05:71-75), bottom-to-top (`for_each_layer_in`).
  std::uint32_t achieved = request.sample_rate;
  bool exact = true;
  doc.for_each_layer_in(composition, [&](ObjectId layer_id) {
    const LayerRecord* layer = doc.find_layer(layer_id);
    if (layer == nullptr) {
      return;
    }
    mix_layer(*layer, *comp, resolve, pull, request, policy, achieved, exact);
  });

  return AudioResult{achieved, exact};
}

} // namespace arbc
