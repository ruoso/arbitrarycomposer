#include <arbc/audio_engine/mix.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/audio_resampler.hpp> // resample_audio (whole-stream export-edge oracle)
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/export_monitor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

// Component unit + byte-exact goldens + a focused concurrency (TSan) test for
// `arbc::ExportMonitor` (`runtime.export_monitor`, doc 12:187-191). The scene
// doubles are LOCAL Content / AudioFacet leaves (mirroring `mix.t.cpp`'s SineLeaf /
// BelowRateLeaf / VisualLeaf) so the includes stay in runtime's dependency closure
// -- the byte-exact goldens through the REAL `PullServiceImpl` + `org.arbc.tone`
// live in top-level `tests/audio_export_goldens.t.cpp`. The export driver itself
// owns the real `PullServiceImpl`; these leaves only supply per-content audio.

namespace {

using namespace arbc;

// A deterministic procedural audio leaf: sample = amp * parabolic-sine over an
// EXACT integer flick phase (never std::sin), a rate-honoring Static child. Copied
// from `mix.t.cpp` -- the byte-exact integer-flick oracle the repo mandates.
float parab_sine(std::int64_t t_flicks, std::uint32_t freq_hz, float amp) {
  const std::int64_t fps = Time::flicks_per_second;
  std::int64_t t = t_flicks % fps;
  if (t < 0) {
    t += fps;
  }
  const std::int64_t r = (static_cast<std::int64_t>(freq_hz) * t) % fps;
  double p = 2.0 * (static_cast<double>(r) / static_cast<double>(fps));
  if (p > 1.0) {
    p -= 2.0;
  }
  const double abs_p = p < 0.0 ? -p : p;
  return static_cast<float>(static_cast<double>(amp) * (4.0 * p * (1.0 - abs_p)));
}

class SineLeaf final : public Content {
public:
  SineLeaf(std::uint32_t freq_hz, float amp) : d_facet(freq_hz, amp) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    Facet(std::uint32_t freq_hz, float amp) : d_freq(freq_hz), d_amp(amp) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v = parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf,
                                   d_freq, d_amp);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    std::uint32_t d_freq;
    float d_amp;
  };
  Facet d_facet;
};

// A leaf that bottoms out at a native rate: asked above it, reports
// achieved_rate == native / exact == false (driving the mixer's below-rate
// reconstruction and the honest fold). Copied from `mix.t.cpp`.
class BelowRateLeaf final : public Content {
public:
  explicit BelowRateLeaf(std::uint32_t native_rate) : d_facet(native_rate) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    explicit Facet(std::uint32_t native_rate) : d_native(native_rate) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v =
            parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf, 400, 0.5F);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      const std::uint32_t achieved = std::min(request.sample_rate, d_native);
      return AudioResult{achieved, request.sample_rate <= d_native};
    }

  private:
    std::uint32_t d_native;
  };
  Facet d_facet;
};

// A purely visual leaf: no audio facet, so it must cost the export nothing.
class VisualLeaf final : public Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 1.0, 1.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true};
  }
};

// The stateless inline audio pull the per-window ORACLE drives `mix_composition`
// through: routes `pull_audio` to the input's facet and settles inline, exactly as
// the export driver's real `PullServiceImpl` + `direct_audio_dispatch` does, so the
// oracle's samples are byte-identical to the driver's.
class InlineAudioPull final : public PullService {
public:
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
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

constexpr std::uint32_t k_rate = 48'000;
constexpr std::int64_t k_fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);

// A range spanning `frames` sample frames at the working rate, from zero.
TimeRange range_of(std::int64_t frames) { return TimeRange{Time::zero(), Time{frames * k_fpf}}; }

// Concatenate every block `render_range` hands the sink, in order.
std::vector<float> export_all(ExportMonitor& monitor, const TimeRange& range,
                              std::uint32_t block_frames) {
  std::vector<float> out;
  monitor.render_range(range, block_frames, [&](TimeRange, const AudioBlock& block, AudioResult) {
    const std::size_t n = static_cast<std::size_t>(block.frames) * channel_count(block.layout);
    out.insert(out.end(), block.samples, block.samples + n);
  });
  return out;
}

// The independent oracle: `mix_composition` called once per window directly through
// a fresh inline pull, concatenated -- the byte-exact reference for `render_range`.
std::vector<float> oracle_all(const DocRoot& doc, ObjectId comp, const MixResolver& resolve,
                              const TimeRange& range, std::uint32_t rate,
                              std::uint32_t block_frames, ChannelLayout layout) {
  InlineAudioPull pull;
  std::vector<float> out;
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(rate);
  for (const TimeRange& window : block_windows_over(range, rate, block_frames)) {
    const std::uint32_t frames =
        static_cast<std::uint32_t>((window.end.flicks - window.start.flicks) / fpf);
    std::vector<float> buf(static_cast<std::size_t>(frames) * channel_count(layout), 0.0F);
    AudioBlock block{buf.data(), frames, layout, rate};
    const AudioRequest req{window, rate, layout, block, Exactness::Exact, StateHandle{}};
    mix_composition(doc, comp, resolve, pull, req);
    out.insert(out.end(), buf.begin(), buf.end());
  }
  return out;
}

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// Concatenate every container-rate block `render_range_to` hands the sink, in order.
std::vector<float> export_all_to(ExportMonitor& monitor, const TimeRange& range,
                                 std::uint32_t output_rate, std::uint32_t block_frames) {
  std::vector<float> out;
  monitor.render_range_to(
      range, output_rate, block_frames, [&](TimeRange, const AudioBlock& block, AudioResult) {
        const std::size_t n = static_cast<std::size_t>(block.frames) * channel_count(block.layout);
        out.insert(out.end(), block.samples, block.samples + n);
      });
  return out;
}

// The whole-stream export-edge oracle (Constraint 1/4, D3): the whole-range working
// mix -- one `render_block_at` over `range`, which for a Static composition is
// byte-identical to the concatenated per-block mix `render_range_to` feeds -- resampled
// ONCE to `output_rate` through the shipped `resample_audio`, producing exactly the
// container-rate frame count covering the range (the same `span / out_fpf` total the
// stage drains its finite tail to). The reference the streaming export edge must equal.
std::vector<float> resample_oracle(ExportMonitor& monitor, const TimeRange& range,
                                   std::uint32_t output_rate, ChannelLayout layout) {
  const std::uint32_t working_rate = monitor.format().sample_rate;
  const std::int64_t in_fpf = Time::flicks_per_second / static_cast<std::int64_t>(working_rate);
  const std::int64_t out_fpf = Time::flicks_per_second / static_cast<std::int64_t>(output_rate);
  const std::uint32_t ch = channel_count(layout);
  const auto in_frames =
      static_cast<std::uint32_t>((range.end.flicks - range.start.flicks) / in_fpf);
  const auto out_frames =
      static_cast<std::uint32_t>((range.end.flicks - range.start.flicks) / out_fpf);
  std::vector<float> in(static_cast<std::size_t>(in_frames) * ch, 0.0F);
  AudioBlock in_block{in.data(), in_frames, layout, working_rate};
  (void)monitor.render_block_at(range, in_block);
  std::vector<float> out(static_cast<std::size_t>(out_frames) * ch, 0.0F);
  AudioBlock out_block{out.data(), out_frames, layout, output_rate};
  resample_audio(in_block, out_block);
  return out;
}

// Attach a fresh content-bound layer to `comp` and return its layer id.
ObjectId add_layer(Document& doc, ObjectId comp, std::shared_ptr<Content> content) {
  const ObjectId cid = doc.add_content(std::move(content));
  const ObjectId layer = doc.add_layer(cid, Affine::identity());
  doc.attach_layer(comp, layer);
  return layer;
}

} // namespace

// enforces: 12-audio#export-monitor-mixes-exactly-over-range
// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("the export block loop is byte-identical to mix_composition per window") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.6F));
  const ObjectId lb = add_layer(document, comp, std::make_shared<SineLeaf>(700, 0.4F));
  document.set_layer_gain(lb, 0.5);

  const TimeRange range = range_of(96); // 96 sample frames at the working rate
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  // Block-loop output equals the per-window oracle, byte-exact (no tolerance).
  const std::vector<float> got = export_all(monitor, range, 32);
  const std::vector<float> want = oracle_all(
      monitor.pinned_state(), comp, [&](ObjectId id) { return document.resolve(id); }, range,
      k_rate, 32, ChannelLayout::Stereo);
  REQUIRE(bytes_equal(got, want));

  // Block-boundary invariance: 96 is a whole multiple of 32 (3 blocks) but not of 40
  // (40 + 40 + a 16-frame partial trailing block); the concatenation is identical.
  const std::vector<float> got40 = export_all(monitor, range, 40);
  REQUIRE(bytes_equal(got40, got));
}

// enforces: 12-audio#export-monitor-mixes-exactly-over-range
TEST_CASE("every exported block of a rate-honoring composition is exact at the working rate") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(440, 0.5F));
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  int blocks = 0;
  monitor.render_range(range_of(96), 32, [&](TimeRange, const AudioBlock&, AudioResult r) {
    ++blocks;
    CHECK(r.exact);
    CHECK(r.achieved_rate == k_rate);
  });
  CHECK(blocks == 3);
}

// enforces: 12-audio#export-monitor-mixes-exactly-over-range
TEST_CASE("a below-working-rate contributor folds achieved_rate/exact honestly") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<BelowRateLeaf>(24'000)); // half the working rate
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  monitor.render_range(range_of(64), 32, [&](TimeRange, const AudioBlock&, AudioResult r) {
    CHECK_FALSE(r.exact);             // never silently upgraded
    CHECK(r.achieved_rate == 24'000); // min-folded honestly
  });
}

// enforces: 12-audio#export-monitor-mixes-exactly-over-range
TEST_CASE("re-running the same export yields byte-identical output") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.6F));
  add_layer(document, comp, std::make_shared<SineLeaf>(700, 0.4F));
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  const std::vector<float> first = export_all(monitor, range_of(96), 32);
  const std::vector<float> second = export_all(monitor, range_of(96), 32);
  REQUIRE(bytes_equal(first, second));
}

// enforces: 12-audio#mix-engine-facetless-costs-nothing
// enforces: 12-audio#pull-audio-is-cache-first-single-settle
TEST_CASE("facet-less and silent layers cost the export zero dispatches") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.5F)); // audible
  add_layer(document, comp, std::make_shared<VisualLeaf>());        // no audio facet
  const ObjectId muted = add_layer(document, comp, std::make_shared<SineLeaf>(440, 0.5F));
  const ObjectId zero = add_layer(document, comp, std::make_shared<SineLeaf>(440, 0.5F));
  add_layer(document, comp, std::make_shared<SineLeaf>(600, 0.5F)); // audible
  document.set_layer_audible(muted, false);
  document.set_layer_gain(zero, 0.0);

  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  // One block through the export driver: exactly two audible in-span audio layers
  // dispatch; the facet-less / muted / zero-gain layers issue zero (asserted on the
  // behavioral counter, never wall-clock).
  std::vector<float> buf(static_cast<std::size_t>(32) * 2, 1.0F);
  AudioBlock block{buf.data(), 32, ChannelLayout::Stereo, k_rate};
  (void)monitor.render_block_at(range_of(32), block);
  CHECK(monitor.counters().audio_dispatches() == 2);
}

// enforces: 12-audio#export-monitor-mixes-exactly-over-range
TEST_CASE("export faults surface as values, never an abort") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.5F));

  SECTION("an empty range or a non-positive block size drives the sink zero times") {
    ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});
    int blocks = 0;
    monitor.render_range(TimeRange{Time::zero(), Time::zero()}, 32,
                         [&](TimeRange, const AudioBlock&, AudioResult) { ++blocks; });
    monitor.render_range(range_of(96), 0,
                         [&](TimeRange, const AudioBlock&, AudioResult) { ++blocks; });
    CHECK(blocks == 0);
    CHECK(block_windows_over(TimeRange{Time::zero(), Time::zero()}, k_rate, 32).empty());
    CHECK(block_windows_over(range_of(96), k_rate, 0).empty());
    CHECK(block_windows_over(range_of(96), 0, 32).empty());
  }

  SECTION("an unresolved composition id mixes a faithful silent block") {
    ExportMonitor monitor(document, ObjectId{9'999}, AudioFormat{k_rate, ChannelLayout::Stereo});
    std::vector<float> buf(static_cast<std::size_t>(32) * 2, 1.0F);
    AudioBlock block{buf.data(), 32, ChannelLayout::Stereo, k_rate};
    const AudioResult r = monitor.render_block_at(range_of(32), block);
    for (const float v : buf) {
      CHECK(v == 0.0F); // silence-filled, no abort
    }
    CHECK(r.exact);
    CHECK(r.achieved_rate == k_rate); // an honest silent block at the request rate
    CHECK(monitor.counters().audio_dispatches() == 0);
  }
}

// enforces: 12-audio#export-edge-resamples-working-to-container
TEST_CASE("the export edge resamples the working mix to a container rate byte-exactly") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.6F));
  add_layer(document, comp, std::make_shared<SineLeaf>(700, 0.4F));
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  SECTION("decimation: 48 kHz working -> 44.1 kHz container (coprime 160:147)") {
    const TimeRange range = range_of(480); // -> 441 output frames, exercises the phase accumulator
    const std::vector<float> got = export_all_to(monitor, range, 44'100, 32);
    const std::vector<float> want = resample_oracle(monitor, range, 44'100, ChannelLayout::Stereo);
    REQUIRE(!got.empty());
    REQUIRE(bytes_equal(got, want)); // no tolerance, correct finite tail
  }

  SECTION("up-sample: 48 kHz working -> 96 kHz container (1:2)") {
    const TimeRange range = range_of(96); // -> 192 output frames
    const std::vector<float> got = export_all_to(monitor, range, 96'000, 32);
    const std::vector<float> want = resample_oracle(monitor, range, 96'000, ChannelLayout::Stereo);
    REQUIRE(!got.empty());
    REQUIRE(bytes_equal(got, want));
  }

  SECTION("block-boundary invariance: the container output is independent of block_frames") {
    const TimeRange range = range_of(480);
    const std::vector<float> a = export_all_to(monitor, range, 44'100, 16); // many small blocks
    const std::vector<float> b = export_all_to(monitor, range, 44'100, 100);
    const std::vector<float> c = export_all_to(monitor, range, 44'100, 33); // partial trailing
    REQUIRE(!a.empty());
    REQUIRE(bytes_equal(a, b)); // continuity across export block boundaries, no phase restart
    REQUIRE(bytes_equal(a, c));
  }
}

// enforces: 12-audio#export-edge-resamples-working-to-container
TEST_CASE("a degenerate container output rate drives the export sink zero times") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(440, 0.5F));
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  int blocks = 0;
  const auto count = [&](TimeRange, const AudioBlock&, AudioResult) { ++blocks; };
  monitor.render_range_to(range_of(96), 0, 32, count);     // zero rate
  monitor.render_range_to(range_of(96), 44'100, 0, count); // non-positive block
  monitor.render_range_to(TimeRange{Time::zero(), Time::zero()}, 44'100, 32, count); // empty range
  CHECK(blocks == 0);
}

// enforces: 12-audio#export-edge-resamples-working-to-container
// The matched-rate no-regression / zero-engagement promise (the audio analog of "a
// fade at envelope=1 issues zero operator renders"): output_rate == working_rate runs
// the shipped 1:1 render_range verbatim -- byte-identical output AND the identical
// per-block mix dispatch count, with no resampler engaged.
TEST_CASE("a matched container rate keeps the 1:1 export path byte-identical and cost-free") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.6F));
  add_layer(document, comp, std::make_shared<SineLeaf>(700, 0.4F));

  const TimeRange range = range_of(96);
  ExportMonitor plain(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});
  ExportMonitor matched(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  const std::vector<float> want = export_all(plain, range, 32);             // shipped 1:1 golden
  const std::vector<float> got = export_all_to(matched, range, k_rate, 32); // matched-rate edge
  REQUIRE(bytes_equal(got, want));
  // Zero extra work: the matched path issues exactly the 1:1 path's mix dispatches
  // (two audible layers over three blocks), never a resampler-driven re-pull.
  CHECK(matched.counters().audio_dispatches() == plain.counters().audio_dispatches());
  CHECK(matched.counters().audio_dispatches() == 2u * 3u);
}

TEST_CASE("block_windows_over tiles a half-open range at the working rate exactly") {
  SECTION("a whole multiple gives equal contiguous windows") {
    const std::vector<TimeRange> w = block_windows_over(range_of(96), k_rate, 32);
    REQUIRE(w.size() == 3);
    CHECK(w[0] == TimeRange{Time::zero(), Time{32 * k_fpf}});
    CHECK(w[1] == TimeRange{Time{32 * k_fpf}, Time{64 * k_fpf}});
    CHECK(w[2] == TimeRange{Time{64 * k_fpf}, Time{96 * k_fpf}});
  }
  SECTION("a non-multiple ends in a partial trailing block") {
    const std::vector<TimeRange> w = block_windows_over(range_of(96), k_rate, 40);
    REQUIRE(w.size() == 3);
    CHECK(w[2] == TimeRange{Time{80 * k_fpf}, Time{96 * k_fpf}}); // 16-frame partial tail
  }
  SECTION("a range shorter than one sample yields an empty series") {
    CHECK(block_windows_over(TimeRange{Time::zero(), Time{k_fpf - 1}}, k_rate, 32).empty());
  }
}

TEST_CASE("the export monitor produces at the composition's configured working format by default") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_layer(document, comp, std::make_shared<SineLeaf>(300, 0.5F));
  document.set_working_audio_format(comp, AudioFormat{k_rate, ChannelLayout::Mono});

  // No explicit format: the monitor defaults to the pinned composition's working
  // format (mono here), so the blocks it hands back are mono.
  ExportMonitor monitor(document, comp);
  CHECK(monitor.format().layout == ChannelLayout::Mono);
  CHECK(monitor.format().sample_rate == k_rate);
  monitor.render_range(range_of(32), 32, [&](TimeRange, const AudioBlock& block, AudioResult) {
    CHECK(block.layout == ChannelLayout::Mono);
  });
}

// enforces: 12-audio#export-monitor-pins-single-revision
TEST_CASE("an export pins one revision while a writer keeps editing") {
  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  const ObjectId layer = add_layer(document, comp, std::make_shared<SineLeaf>(440, 0.5F));
  document.set_layer_gain(layer, 1.0);

  const TimeRange range = range_of(96);
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});
  const std::uint64_t pinned = monitor.revision();
  // The pinned-revision golden (gain 1.0). A leaked mid-export gain edit would scale
  // every sample, so byte-equality to this golden is a genuine no-leak witness.
  const std::vector<float> golden = export_all(monitor, range, 32);

  // The model is writer-thread-confined, so the COMMITS stay on this (owning) thread
  // and the read-only EXPORT runs on a second thread -- the "export while editing"
  // split of doc 02:77-80. The exporter records outcomes into atomics (Catch2 macros
  // are not thread-safe) checked after the join. No timing assertion (the idiom from
  // offline_sequence.t.cpp).
  constexpr int k_runs = 64;
  std::atomic<bool> go{false};
  std::atomic<bool> mismatch{false};
  std::atomic<bool> wrong_revision{false};
  std::atomic<int> runs_done{0};
  std::thread exporter([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < k_runs; ++i) {
      const std::vector<float> out = export_all(monitor, range, 32);
      if (!bytes_equal(out, golden)) {
        mismatch.store(true, std::memory_order_release); // a mid-export edit leaked in
      }
      if (monitor.revision() != pinned) {
        wrong_revision.store(true, std::memory_order_release);
      }
      runs_done.fetch_add(1, std::memory_order_release);
    }
  });

  go.store(true, std::memory_order_release);
  int commits = 0;
  // Keep committing gain edits that WOULD change the mix if they leaked, until the
  // exporter has drained every run.
  while (runs_done.load(std::memory_order_acquire) < k_runs) {
    document.set_layer_gain(layer, (commits % 2 == 0) ? 0.25 : 1.0);
    if ((++commits % 16) == 0) {
      std::this_thread::yield(); // widen the race window
    }
  }
  exporter.join();

  CHECK_FALSE(mismatch.load(std::memory_order_acquire));       // no exported block saw an edit
  CHECK_FALSE(wrong_revision.load(std::memory_order_acquire)); // the pin held throughout
  CHECK(monitor.revision() == pinned);
  CHECK(document.pin()->revision() > pinned); // the writer really did advance the model
}
