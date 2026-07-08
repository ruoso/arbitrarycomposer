#include <arbc/base/expected.hpp>  // expected, unexpected
#include <arbc/base/transform.hpp> // Affine
#include <arbc/compositor/pull_service.hpp> // direct_dispatch, direct_audio_dispatch, PullConfig
#include <arbc/contract/content.hpp>        // Content, AudioRequest, Exactness, StateHandle
#include <arbc/model/records.hpp>           // LayerRecord (reverse-map walk)
#include <arbc/runtime/export_monitor.hpp>
#include <arbc/surface/backend.hpp>         // Backend, BackendCaps
#include <arbc/surface/surface.hpp>         // Surface
#include <arbc/surface/surface_error.hpp>   // SurfaceError

#include <algorithm>
#include <cstddef>
#include <memory>
#include <unordered_map>

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
    windows.push_back(TimeRange{Time{range.start.flicks + f0 * fpf},
                                Time{range.start.flicks + f1 * fpf}});
  }
  return windows;
}

ExportMonitor::ExportMonitor(const Document& document, ObjectId composition,
                             std::optional<AudioFormat> format, MixPolicy policy)
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
  // The mix engine's resolver over the pinned document's bindings (`mix.hpp:33`).
  d_resolve = [this](ObjectId id) -> Content* { return d_document.resolve(id); };

  // A reverse `Content* -> ObjectId` map over the pinned revision so the block cache
  // keys each block under its content's identity (doc 13:126). Walked once here from
  // the frozen revision (`for_each_layer` visits every layer record, so nested-
  // composition contributors are covered too). In a pure offline export nothing
  // fills the block cache, so these keys never serve a hit -- wiring the substrate
  // faithfully keeps the pull path identical to the one the mix core runs under when
  // a lookahead pump does fill the cache.
  auto ids = std::make_shared<std::unordered_map<const Content*, ObjectId>>();
  d_pinned->for_each_layer([&](const LayerRecord& layer) {
    if (Content* c = d_document.resolve(layer.content)) {
      ids->emplace(c, layer.content);
    }
  });

  PullConfig config;
  config.counters = &d_counters;
  config.id_of = [ids](const Content* c) {
    const auto it = ids->find(c);
    return it != ids->end() ? it->second : ObjectId{};
  };
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
                             Exactness::Exact, StateHandle{}};
  return mix_composition(*d_pinned, d_composition, d_resolve, *d_pull, request, d_policy);
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

} // namespace arbc
