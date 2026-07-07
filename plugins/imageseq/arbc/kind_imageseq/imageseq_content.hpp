#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace arbc::imageseq {

// One native source frame: the path to a decodable still. Frames are numbered
// 0..N-1 and sit on the native-rate grid; the pixels are decoded lazily on the
// first render that resolves the frame (the decoded-frame cache below).
struct FrameSource {
  std::string path;
};

// The decoded working-space surface a resolved frame is returned through. A
// self-contained CPU-memory Surface (doc 09): the plugin owns its own frame
// storage rather than allocating through a Backend, so it pulls in no
// `backend-cpu` edge (imageseq_plugin.md §1). One instance per decoded frame,
// shared by refcount so many tiles / sub-frame times at one instant reuse a
// single decode (Decision 4).
class FrameSurface;

// `org.arbc.imageseq` -- Timed visual content backed by a sequence of decoded
// still frames at a fixed native rate (doc 11 §Scheduling decision). The
// reference proof of the temporal `Content` contract and the out-of-lib plugin
// that keeps its decode dependency off `libarbc`'s link line (doc 17).
//
// Contract shape:
//   * stability() == Timed; time_extent() == [0, N*period).
//   * quantize_time(t) floors t to the nearest native source-frame instant
//     (exact rational, one ties-to-even leaf rounding), clamped to the frame
//     range; render(time=t).achieved_time == quantize_time(t), idempotent
//     (content.hpp:261-263, doc 11:124-126).
//   * render returns the decoded frame through RenderResult.provided (a
//     non-transient refcounted SurfaceRef; Decision 4).
//   * render_thread_safe() == false: the decoder is stateful (a bounded
//     decoded-frame cache + playback_hint pre-roll), so the core serializes its
//     requests through the per-content queue (Decision 5).
//   * playback_hint(...) pre-rolls the decoder sequentially -- advisory and
//     correctness-neutral: rendered pixels are byte-identical with or without a
//     hint (constraint §6).
class ImageSeqContent final : public Content {
public:
  static constexpr const char* kind_id = "org.arbc.imageseq";

  // `frames` in native (ascending) order; `native_rate` = frames per second as
  // an exact rational (e.g. 24/1 or 24000/1001, num > 0); `width`/`height` =
  // the uniform native pixel size. Decodes nothing eagerly.
  ImageSeqContent(std::vector<FrameSource> frames, Rational native_rate, int width, int height);
  ~ImageSeqContent() override;

  // --- Content ---
  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<Time> quantize_time(Time t) const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;
  bool render_thread_safe() const override { return false; }
  void playback_hint(const PlaybackHint& hint) override;

  // --- behavioral counters (doc 16:54-62) ---
  // Decoded-frame-cache miss count: one per genuine decode. N sub-frame times
  // inside one native interval bump it exactly once; a paused (empty) hint
  // bumps it zero (imageseq_plugin.md §counters).
  std::uint64_t decodes_issued() const noexcept;

  int frame_count() const noexcept { return static_cast<int>(d_frames.size()); }
  Time frame_period() const noexcept { return d_period; }

private:
  using FramePtr = std::shared_ptr<FrameSurface>;

  // floor(t) to the native grid, clamped to [0, N-1].
  int frame_index_for(Time t) const;
  // The exact instant of native frame `index` (index * period, one ties-to-even
  // leaf rounding); `index` need not be clamped.
  Time instant_for_index(std::int64_t index) const;
  // Resolve frame `index` from the bounded cache, decoding on a miss (bumps
  // decodes_issued). nullptr on a decode/open failure. Caller holds d_mutex.
  FramePtr resolve_locked(int index);

  std::vector<FrameSource> d_frames;
  Rational d_rate;
  int d_width;
  int d_height;
  Time d_period; // native frame period in flicks (instant_for_index(1))

  mutable std::mutex d_mutex;             // guards the cache + pre-roll state
  std::deque<std::pair<int, FramePtr>> d_cache; // bounded decoded-frame cache (LRU)
  int d_last_index{-1};                   // pre-roll anchor: last resolved frame
  std::uint64_t d_decodes_issued{0};
};

// The factory the plugin entry point registers: resolve `config` (a directory
// of numbered `*.ppm`/`*.pgm` frames) into a fresh content, or an error value
// (missing directory, no frames, undecodable header). Errors are values --
// never thrown across the extern "C" boundary (doc 03:177-180). The native rate
// defaults to 24/1; direct-construction tests pass an explicit rate.
expected<std::unique_ptr<Content>, std::string> make_imageseq_content(ContentConfig config);

} // namespace arbc::imageseq
