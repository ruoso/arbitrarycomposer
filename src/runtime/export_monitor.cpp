#include <arbc/base/expected.hpp>           // expected, unexpected
#include <arbc/base/transform.hpp>          // Affine
#include <arbc/compositor/pull_service.hpp> // direct_dispatch, direct_audio_dispatch, PullConfig
#include <arbc/contract/content.hpp>        // Content, AudioRequest, Exactness, StateHandle
#include <arbc/runtime/export_monitor.hpp>
#include <arbc/runtime/pull_identity.hpp> // make_pull_identity_of (child-distinct id_of)
#include <arbc/surface/backend.hpp>       // Backend, BackendCaps
#include <arbc/surface/surface.hpp>       // Surface
#include <arbc/surface/surface_error.hpp> // SurfaceError

#include <algorithm>
#include <cstddef>
#include <memory>

namespace arbc {

namespace {

// A backend that stores nothing: the offline audio path never composites, so the
// `PullServiceImpl` this monitor owns never dereferences its `Backend&` (the visual
// `pull` path is unused; `pull_audio` touches no backend). It exists only to satisfy
// the service's constructor -- `runtime` links no `backend-cpu` (doc 17:60 DEPENDS),
// and a real backend would be dead weight. `make_surface` is capability-honest (doc
// 10): it returns the error value, never an abort, on the off chance a visual pull
// is ever issued.
class NullBackend final : public Backend {
public:
  BackendCaps capabilities() const override { return {}; }
  expected<std::unique_ptr<Surface>, SurfaceError> make_surface(int, int, SurfaceFormat) override {
    return unexpected(SurfaceError::UnsupportedFormat);
  }
  void clear(Surface&, float, float, float, float) override {}
  void composite(Surface&, const Surface&, const Affine&, double) override {}
  void downsample(Surface&, const Surface&) override {}
  void convert(Surface&, const Surface&) override {}
};

// The visual tile cache and the 1D block cache the pull substrate owns. Both stay
// empty in a pure offline export (the audio path issues no visual pull, and the
// prefetch-ring block fill is `audio.lookahead`'s, not this driver's), so a modest
// budget suffices -- it is never charged.
constexpr std::size_t k_export_tile_cache_budget = 1u * 1024 * 1024;
constexpr std::size_t k_export_block_cache_budget = 1u * 1024 * 1024;

} // namespace

std::vector<TimeRange> block_windows_over(const TimeRange& range, std::uint32_t rate,
                                          std::uint32_t block_frames) {
  std::vector<TimeRange> windows;
  // A non-positive block size mixes nothing, a zero rate clocks no sample, and an
  // empty range spans no instant: an empty series, never an abort (faults-as-values,
  // doc 10).
  if (range.empty() || rate == 0 || block_frames == 0) {
    return windows;
  }
  // Flicks per sample frame at the working rate -- the same integer step the mix
  // engine walks (`mix.t.cpp:239`), so window boundaries land exactly on the mixer's
  // per-sample grid and the concatenated sample set is `block_frames`-invariant. A
  // rate finer than the flick resolution has no representable step: an empty series.
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  if (fpf <= 0) {
    return windows;
  }
  const std::int64_t span = range.end.flicks - range.start.flicks;
  // Whole sample frames that fit in `range` at `rate`: a sample frame occupies
  // `[start + i*fpf, start + (i+1)*fpf)`, so frame i fits iff `(i+1)*fpf <= span`.
  // A range shorter than one sample yields zero -> an empty series.
  const std::int64_t total_frames = span / fpf;
  if (total_frames <= 0) {
    return windows;
  }
  const std::int64_t b = static_cast<std::int64_t>(block_frames);
  for (std::int64_t f0 = 0; f0 < total_frames; f0 += b) {
    const std::int64_t f1 = std::min(f0 + b, total_frames); // partial trailing block at the tail
    windows.push_back(
        TimeRange{Time{range.start.flicks + f0 * fpf}, Time{range.start.flicks + f1 * fpf}});
  }
  return windows;
}

ExportMonitor::ExportMonitor(const Document& document, ObjectId composition,
                             std::optional<AudioFormat> format, MixPolicy policy,
                             std::optional<Spatialization> spatial)
    : d_document(document), d_composition(composition),
      // Pin ONCE for the whole export (Decision, doc 02:77-80): this snapshot
      // outlives every block and every later commit, so the export is
      // revision-consistent across blocks, not merely within one.
      d_pinned(document.pin()),
      // Produce at the working rate/layout (doc 12:94-104): the caller's `format`, or
      // the pinned composition's configured working `AudioFormat` by default.
      d_format(format.has_value() ? *format : d_pinned->working_audio_format()), d_policy(policy),
      d_backend(std::make_unique<NullBackend>()), d_cache(k_export_tile_cache_budget),
      d_blocks(k_export_block_cache_budget) {
  // Activate the Spatial seed only under `Spatial` (doc 12:167-206, refinement point
  // 5): the camera's uniform scale-attenuation seeds the accumulated attenuation the
  // sub-audible cull compares against, and post-scales the root mix once. Under `Flat`
  // (or an unseeded `Spatial`) nothing is threaded and the export stays byte-identical.
  if (d_policy == MixPolicy::Spatial && spatial.has_value()) {
    d_spatial = *spatial;
    d_spatial->accum_atten = spatial_edge_atten(d_spatial->listener);
  }

  // The mix engine's resolver over the pinned document's bindings (`mix.hpp:33`).
  d_resolve = [this](ObjectId id) -> Content* { return d_document.resolve(id); };

  // A reverse `Content* -> ObjectId` map over the pinned revision so the block cache
  // keys each block under its content's identity (doc 13:141-154). Built once here
  // from the frozen revision: it seeds from every layer record (so nested-composition
  // contributors are covered too) AND assigns every operator input child a distinct
  // synthesized id (runtime.operator_input_cache_identity), so two same-stability
  // audio inputs of one operator key under different `BlockKey`s instead of aliasing
  // on `ObjectId{}` -- the audio twin of the visual collision fix.
  PullConfig config;
  config.counters = &d_counters;
  config.id_of = make_pull_identity_of(*d_pinned, d_resolve);
  // Every node contributes the one pinned revision (doc 05:82-91): the whole export
  // keys at `DocRoot::revision() == R`.
  const std::uint64_t revision = d_pinned->revision();
  config.contribution = [revision](const Content*) { return revision; };
  // Render each block-cache miss inline on this (the export) thread -- offline has no
  // worker fan-out (Decision "drive mix_composition directly"): the audio twin of
  // `direct_dispatch`.
  config.audio_dispatch = direct_audio_dispatch();
  config.blocks = &d_blocks;
  d_pull = std::make_unique<PullServiceImpl>(d_cache, *d_backend, direct_dispatch(), config);

  // Bind every service-needing content (`org.arbc.fade`, `org.arbc.nested`, ...) in
  // the pinned document to this export's live audio pull + null backend for the whole
  // export (operators.fade_runtime_binding Constraint 2,
  // kinds.nested_runtime_binding): a fade layer's `render_audio` pulls its input
  // through `*d_pull` and would otherwise assert (`fade_content.cpp:209`), and a
  // nested layer would mix silence through a null `PullService`. Nested additionally
  // takes the resolver and `d_pinned` -- THIS export's one snapshot, so every block
  // reads the same membership the rest of the export does. The scope (member
  // `d_binding`) tears every binding down on destruction, before `d_pull`/`d_backend`
  // die (Constraint 3/4), and holds the pin until it does. The audio path never
  // dereferences the backend, so the `NullBackend` is a faithful borrow.
  register_builtin_operator_binders();
  d_binding = bind_operators(d_document, *d_pull, *d_backend, d_pinned);
}

AudioResult ExportMonitor::render_block_at(const TimeRange& window, AudioBlock& target) {
  // Zero-initialize the caller-owned target: the mix engine sums additively into
  // silence (`mix.hpp:44`).
  const std::size_t n = static_cast<std::size_t>(target.frames) * channel_count(target.layout);
  if (target.samples != nullptr) {
    std::fill_n(target.samples, n, 0.0F);
  }
  // Exact, faithful, deadline-free (doc 02:73-75, contract `content.hpp:239-240`):
  // the offline-export mode renders every contributor to completion. The request
  // rides the working rate/layout; the pinned revision is threaded as `*d_pinned` to
  // `mix_composition` directly (the same machinery video uses -- the request-level
  // `StateHandle` stays none, exactly as every existing mix caller).
  const AudioRequest request{window,           d_format.sample_rate, d_format.layout, target,
                             Exactness::Exact, StateHandle{},        d_spatial};
  const AudioResult result =
      mix_composition(*d_pinned, d_composition, d_resolve, *d_pull, request, d_policy);
  if (d_spatial.has_value() && target.samples != nullptr) {
    // Post-scale the top mix by the camera's uniform scale-attenuation (doc 12:186-190,
    // refinement point 5): each layer applied only its own edge attenuation, so the
    // camera's scale multiplies the whole root mix once, here.
    const float cam_atten = d_spatial->accum_atten;
    for (std::size_t i = 0; i < n; ++i) {
      target.samples[i] *= cam_atten;
    }
  }
  return result;
}

void ExportMonitor::render_range(const TimeRange& range, std::uint32_t block_frames,
                                 const BlockSink& sink) {
  const std::vector<TimeRange> windows =
      block_windows_over(range, d_format.sample_rate, block_frames);
  if (windows.empty()) {
    return; // empty range / non-positive block / degenerate rate: drive the sink zero times
  }
  const std::uint32_t channels = channel_count(d_format.layout);
  // Safe: a non-empty window series implies `fpf > 0` (`block_windows_over` returned
  // early otherwise), so this division never traps.
  const std::int64_t fpf =
      Time::flicks_per_second / static_cast<std::int64_t>(d_format.sample_rate);
  std::vector<float> buffer;
  for (const TimeRange& window : windows) {
    const std::uint32_t frames =
        static_cast<std::uint32_t>((window.end.flicks - window.start.flicks) / fpf);
    // Own the block's storage for the duration of the sink call; the host keeps,
    // resamples, encodes, or muxes it (doc 12:190-191).
    buffer.assign(static_cast<std::size_t>(frames) * channels, 0.0F);
    AudioBlock block{buffer.data(), frames, d_format.layout, d_format.sample_rate};
    const AudioResult result = render_block_at(window, block);
    sink(window, block, result);
  }
}

void ExportMonitor::render_range_to(const TimeRange& range, std::uint32_t output_rate,
                                    std::uint32_t block_frames, const BlockSink& sink) {
  const std::uint32_t working_rate = d_format.sample_rate;
  // Matched rate: the untouched 1:1 pass (D4, Constraint 2). The shipped working-rate
  // `render_range` runs verbatim -- no `StreamingResampler` configured, zero SRC work,
  // working-rate blocks handed straight to the sink -- so every shipped export golden,
  // claim, and counter is preserved byte-for-byte.
  if (output_rate == working_rate) {
    render_range(range, block_frames, sink);
    return;
  }

  // The container-rate output windows tile `range` at `output_rate`; their frame total
  // is the exact whole-stream output-frame count the tail drain must reach (D3,
  // Constraint 4). A zero/degenerate `output_rate` or a sub-sample range yields an
  // empty series -> drive the sink zero times (faults-as-values, `block_windows_over`).
  const std::vector<TimeRange> out_windows = block_windows_over(range, output_rate, block_frames);
  if (out_windows.empty()) {
    return;
  }
  // The working-rate input windows the mix is pulled through (unchanged producer loop).
  const std::vector<TimeRange> in_windows = block_windows_over(range, working_rate, block_frames);
  const std::uint32_t channels = channel_count(d_format.layout);
  // Safe: a non-empty output series implies both rates clock a representable step.
  const std::int64_t in_fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t out_fpf = Time::flicks_per_second / static_cast<std::int64_t>(output_rate);

  // Configure ONCE at the start of the range (Constraint 5): `configure` selects
  // up-sample vs widened-lowpass decimation from the rate ordering, generates the
  // decimation bank off the per-block path, sizes history, and resets all state (so no
  // separate `reset` between ranges is needed). No media-side change (D1).
  d_resampler.configure(working_rate, output_rate, channels, block_frames);

  // The running fold of every mixed input block's result: `achieved_rate` folds as a
  // min (capped at `output_rate` -- the edge cannot manufacture bandwidth the mix did
  // not carry, and it cannot exceed the container rate it delivers at), `exact` as a
  // conjunction. A below-working-rate contributor thus surfaces honestly on the
  // resampled blocks, never silently upgraded.
  AudioResult folded{output_rate, true};
  std::vector<float> in_buf;
  std::size_t next_in = 0;
  // Feed the next working block (or a zero pad once the finite input is exhausted, so
  // the trailing output frames whose filter support runs past the last input frame
  // read zero taps exactly as the whole-stream oracle does -- the finite tail, D3).
  const auto feed_next = [&]() {
    if (next_in < in_windows.size()) {
      const TimeRange& w = in_windows[next_in++];
      const std::uint32_t frames =
          static_cast<std::uint32_t>((w.end.flicks - w.start.flicks) / in_fpf);
      in_buf.assign(static_cast<std::size_t>(frames) * channels, 0.0F);
      AudioBlock block{in_buf.data(), frames, d_format.layout, working_rate};
      const AudioResult r = render_block_at(w, block);
      folded.achieved_rate = std::min(folded.achieved_rate, r.achieved_rate);
      folded.exact = folded.exact && r.exact;
      d_resampler.push_input(in_buf.data(), frames);
    } else {
      in_buf.assign(static_cast<std::size_t>(block_frames) * channels, 0.0F);
      d_resampler.push_input(in_buf.data(), block_frames);
    }
  };

  std::vector<float> out_buf;
  for (const TimeRange& ow : out_windows) {
    const std::uint32_t out_frames =
        static_cast<std::uint32_t>((ow.end.flicks - ow.start.flicks) / out_fpf);
    out_buf.assign(static_cast<std::size_t>(out_frames) * channels, 0.0F);
    for (std::uint32_t f = 0; f < out_frames; ++f) {
      // Feed working blocks (then zero pad) until the next output frame's whole filter
      // support is resident -- the configure-once / feed / drain discipline that keeps
      // streaming byte-identical to whole-stream (Constraint 1/5).
      while (!d_resampler.can_produce()) {
        feed_next();
      }
      d_resampler.produce(out_buf.data() + static_cast<std::size_t>(f) * channels);
    }
    AudioBlock block{out_buf.data(), out_frames, d_format.layout, output_rate};
    sink(ow, block, folded);
  }
}

} // namespace arbc
