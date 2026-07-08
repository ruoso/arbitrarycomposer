#pragma once

#include <arbc/audio_engine/mix.hpp>        // MixPolicy, MixResolver, mix_composition
#include <arbc/base/ids.hpp>                // ObjectId
#include <arbc/base/time.hpp>               // Time, TimeRange
#include <arbc/compositor/counters.hpp>     // CompositorCounters, TileCache
#include <arbc/compositor/pull_service.hpp> // PullServiceImpl, BlockCache, PullConfig
#include <arbc/contract/content.hpp>        // AudioRequest, AudioResult
#include <arbc/media/audio_block.hpp>       // AudioBlock, ChannelLayout
#include <arbc/media/audio_format.hpp>      // AudioFormat, k_working_audio
#include <arbc/media/streaming_resampler.hpp> // StreamingResampler (export-edge working -> container SRC)
#include <arbc/runtime/document.hpp>          // Document, DocStatePtr, DocRoot
#include <arbc/surface/backend.hpp> // Backend (the null backend the audio path never composites through)

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

// The offline audio export driver for `arbc::runtime` (L5, doc 17:60,78-86): doc
// 12:187-191 ("Export monitor (offline)") + doc 02:73-85 ("The frame, offline")
// made concrete for the 1D signal. It is the audio twin of `SequenceRenderer`
// (`offline_sequence.hpp`) and the offline sibling of `DeviceMonitor`: where the
// device monitor owns a device clock, a lookahead ring, and RT-safety discipline,
// the export monitor inverts every one of those -- NO deadline, NO ring, NO clock,
// exact only (doc 12:283-284's "export monitor (no realtime pressure)"). It drives
// the pure L4 mix core `mix_composition` (`mix.hpp:59-61`) directly in `Exact` mode.
//
// It is a CLASS (not a per-call free function) for the same reason
// `SequenceRenderer` is: the doc-02 consistency guarantee -- a mid-export edit must
// not leak into a late block -- is a property of the WHOLE range, so the pinned
// `DocStatePtr` must outlive every block and be shared by all of them, and the
// `PullServiceImpl` + `BlockCache` the mix pulls through wants the same long-lived
// home (Decision "ExportMonitor is a stateful L5 class"). The instance pins ONE
// `DocStatePtr` at construction and mixes EVERY block against that one
// `DocRoot::revision()`, so an export stays internally consistent even while the
// host keeps editing on the writer thread (doc 02:77-80, doc 12:189-190).
//
// Output boundary (doc 12:190-191, doc 17:150-173): the driver hands back
// sample-exact `AudioBlock`s through a host `BlockSink`; it writes no files, muxes
// nothing, and links no codec -- muxing is the host's business. The mix is produced
// at the composition working rate; `render_range_to` converts it to a different
// container output rate at the export edge through the SAME shared working -> edge
// `StreamingResampler` the device monitor uses (`audio.export_edge_resample`), not a
// second path (doc 12:190-194); a matched output rate keeps the 1:1 `render_range`
// pass unchanged (no resampler engaged).

namespace arbc {

// The contiguous half-open working-rate block-window series tiling `range` at
// `rate` in blocks of `block_frames` samples each (a partial trailing block where
// the range is not a whole multiple). The audio analog of `frame_times_over`
// (`offline_sequence.hpp:58`): window k covers sample frames `[k*block_frames,
// min((k+1)*block_frames, T))` where `T` is the number of whole sample frames that
// fit in `range` at `rate`, and frame i lands at `range.start + i * (flicks_per_
// second / rate)` in exact integer flick arithmetic -- so the series is
// deterministic and byte-reproducible, and the concatenated per-window sample set
// is independent of `block_frames` (block-boundary invariance, doc 16). An empty
// range, a non-positive `block_frames`, a `rate` of zero, or a range shorter than
// one sample yields an empty series (faults-as-values, never an abort, doc 10). A
// caller may compute the series any other way and feed `render_block_at` raw.
std::vector<TimeRange> block_windows_over(const TimeRange& range, std::uint32_t rate,
                                          std::uint32_t block_frames);

// The stateful offline audio export driver (Decision "stateful L5 class").
// Non-copyable and non-movable: it pins a document revision and owns the pull
// substrate (a `PullServiceImpl`, its `BlockCache`, a `TileCache`, and the
// persistent counters) the mix core pulls through.
class ExportMonitor {
public:
  // What the caller's per-block sink receives: the block's working-rate time
  // window, the owned sample-exact mixed block (caller keeps/resamples/encodes/
  // muxes it -- the engine writes no files and links no codec, doc 12:190-191), and
  // the aggregate `AudioResult`. The audio twin of `SequenceRenderer::FrameSink`.
  using BlockSink = std::function<void(TimeRange, const AudioBlock&, AudioResult)>;

  // Construction PINS THE EXPORT (Decision, doc 02:77-80): one `DocStatePtr` for the
  // whole export, so every block reads one frozen `DocRoot::revision()`. `composition`
  // is the root composition mixed; `format` is the working rate + layout the mix is
  // produced at, defaulting (`nullopt`) to the pinned composition's working
  // `AudioFormat` (`DocRoot::working_audio_format()`, doc 12:94-104); `policy` is the
  // per-layer mix policy (`Flat` by default, or `Spatial`, doc 12:167-206). `spatial`
  // is the static Spatial seed the monitor threads into every block's request when
  // `policy == Spatial` -- listener (camera -> composition-local -> viewport),
  // viewport extent, and sub-audible threshold (its `accum_atten` is recomputed by
  // the monitor to the camera's `clamp(max_scale(listener), 0, 1)`). Ignored under
  // `Flat` (byte-identical to the pre-Spatial export path); required under `Spatial`
  // (an unseeded `Spatial` export falls back to Flat, mixing no spatialization).
  explicit ExportMonitor(const Document& document, ObjectId composition,
                         std::optional<AudioFormat> format = std::nullopt,
                         MixPolicy policy = MixPolicy::Flat,
                         std::optional<Spatialization> spatial = std::nullopt);

  ExportMonitor(const ExportMonitor&) = delete;
  ExportMonitor& operator=(const ExportMonitor&) = delete;
  ExportMonitor(ExportMonitor&&) = delete;
  ExportMonitor& operator=(ExportMonitor&&) = delete;

  // Mix one sample-exact block over `window` into the caller-owned `target` against
  // the pinned revision (Decision "drive mix_composition directly in Exact mode").
  // Zero-initializes `target` (the mix engine sums into silence, `mix.hpp:44`),
  // builds an `Exactness::Exact` `AudioRequest` at the working rate/layout carrying
  // NO deadline (offline renders every contributor to completion), and calls
  // `mix_composition(*pinned, composition, resolver, pull, request, policy)`. For a
  // composition whose contributors honor the working rate the returned `AudioResult`
  // reports `exact == true` and `achieved_rate == working_rate`; a below-rate
  // contributor folds `achieved_rate`/`exact` as `min`/conjunction honestly (never
  // silently upgraded). An unresolved composition id mixes a faithful silent block.
  // Returns the aggregate `AudioResult`. `target.frames` sizes the mix; the caller
  // owns `target.samples`.
  AudioResult render_block_at(const TimeRange& window, AudioBlock& target);

  // Tile `range` into contiguous working-rate windows of `block_frames` samples
  // (`block_windows_over`), mix each against the pinned revision via
  // `render_block_at`, and hand each `(window, block, result)` to `sink` (doc
  // 12:187-191). A thin convenience loop that owns each block's storage for the
  // duration of the `sink` call; the HOST keeps/resamples/encodes/muxes. An empty
  // range or a non-positive `block_frames` drives the sink zero times.
  void render_range(const TimeRange& range, std::uint32_t block_frames, const BlockSink& sink);

  // Render `range` to a caller-requested container `output_rate` at the export edge
  // (doc 12:190-194, `audio.export_edge_resample`). The mix stays honest at the
  // working rate: `render_block_at` produces each working-rate block unchanged (D2),
  // then the owned `StreamingResampler` converts working -> `output_rate` -- both
  // directions come from the shipped seam (up-sampling through the frozen
  // input-Nyquist table for `output_rate > working_rate`; the ratio-scaled widened
  // lowpass cut at the output Nyquist for `output_rate < working_rate`), selected
  // internally by `configure` from the rate ordering, no new DSP (D1). The container
  // output is tiled into `block_frames`-sized windows at `output_rate` and handed to
  // `sink` as `(window, block, result)` tuples, each block stamped `output_rate`. The
  // conversion is configured ONCE at the start of the range and fed/drained
  // continuously across block boundaries, so the concatenated output is byte-exact
  // against a single whole-stream `resample_audio` of the whole-range working mix,
  // with a finite tail drained to exactly the container-rate frame count covering the
  // range (D3, Constraint 1/4). When `output_rate` equals the working rate the
  // existing `render_range` runs verbatim with NO resampler engaged (D4, Constraint
  // 2). A degenerate `output_rate` (zero, or a range shorter than one output sample)
  // drives the sink zero times (faults-as-values, `block_windows_over` contract).
  void render_range_to(const TimeRange& range, std::uint32_t output_rate,
                       std::uint32_t block_frames, const BlockSink& sink);

  // The pinned version -- the single frozen `DocRoot` every block mixes against.
  const DocRoot& pinned_state() const noexcept { return *d_pinned; }
  // The one revision the whole export is consistent at (the pin held).
  std::uint64_t revision() const noexcept { return d_pinned->revision(); }
  // The working `AudioFormat` (rate + layout) the mix is produced at.
  const AudioFormat& format() const noexcept { return d_format; }
  // The persistent behavioral counters accumulated across the export (doc 16:54-62):
  // `audio_dispatches()` reads exactly one dispatch per audible in-span audio layer
  // and zero for facet-less/silent layers -- the doc-12 no-cost proof.
  const CompositorCounters& counters() const noexcept { return d_counters; }

private:
  const Document& d_document;
  ObjectId d_composition;
  // The single frozen revision pinned for the WHOLE export (Decision, doc 02:77-80):
  // every block reads `*d_pinned`, so a writer committing new revisions during the
  // export changes no exported block.
  DocStatePtr d_pinned;
  AudioFormat d_format;
  MixPolicy d_policy;
  // The active Spatial seed (doc 12:167-206): set only when `d_policy == Spatial` and
  // a seed was supplied, with `accum_atten` pre-computed to the camera's uniform
  // scale-attenuation `clamp(max_scale(listener), 0, 1)`. `render_block_at` threads it
  // onto the request and post-scales the mixed block by that same value. Nullopt =>
  // the Flat path, byte-identical.
  std::optional<Spatialization> d_spatial;
  // The mix engine's resolver over the pinned document's bindings (`mix.hpp:33`).
  MixResolver d_resolve;
  // The pull substrate the mix core pulls through (Decision "long-lived home"). The
  // null backend is never dereferenced: the offline audio path only issues
  // `pull_audio` (which never touches the backend); `d_cache` (visual tiles) stays
  // empty for the same reason. `d_blocks` is the 1D block cache the audio pull
  // probes cache-first; in a pure offline export nothing fills it (the prefetch-ring
  // fill is `audio.lookahead`'s), so every `pull_audio` is an exact fresh dispatch.
  std::unique_ptr<Backend> d_backend;
  TileCache d_cache;
  BlockCache d_blocks;
  CompositorCounters d_counters;
  std::unique_ptr<PullServiceImpl> d_pull;
  // The shared working -> container-output-rate edge resampler (doc 12:106-121,190-194,
  // D1). Owned but engaged ONLY by `render_range_to` on a rate mismatch: `configure`
  // (which resets all state) runs once per converted range off the per-block path
  // (Constraint 5); the matched-rate 1:1 pass never touches it (Constraint 2). Mirrors
  // `DeviceMonitor::d_resampler` minus the RT flush atomic (offline, no callback).
  StreamingResampler d_resampler;
};

} // namespace arbc
