#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/audio_resampler.hpp> // resample_audio (whole-stream export-edge oracle)
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/export_monitor.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// Cross-component byte-exact integration golden for `arbc::ExportMonitor` driven
// through the REAL machinery -- the export driver's own `PullServiceImpl` and the
// `org.arbc.tone` reference kind (doc 12:187-191). Cross-component (pulls
// kind_tone), so it lives in top-level `tests/` (not level-checked): the export
// monitor (L5) + the concrete pull (L4) + the tone kind (L3) are only assembled
// here. The pure-unit export behavior with local doubles lives in
// `src/runtime/t/export_monitor.t.cpp`.

namespace {

using namespace arbc;

constexpr std::uint32_t k_rate = 48'000;
constexpr std::int64_t k_fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);

// Render one tone's audio directly into an owned buffer over `[0, frames)` at the
// working rate -- the `tone_conformance` oracle style (the mix is exactly the sum of
// the individual tone renders scaled by gain).
std::vector<float> render_tone(ToneContent& tone, std::uint32_t frames) {
  std::vector<float> buf(static_cast<std::size_t>(frames) * 2, 0.0F);
  AudioBlock block{buf.data(), frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * k_fpf}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::Exact,
                         StateHandle{}};
  auto done = std::make_shared<AudioCompletion>();
  (void)tone.audio()->render_audio(req, done);
  return buf;
}

std::vector<float> export_all(ExportMonitor& monitor, const TimeRange& range,
                              std::uint32_t block_frames) {
  std::vector<float> out;
  monitor.render_range(range, block_frames, [&](TimeRange, const AudioBlock& block, AudioResult) {
    const std::size_t n = static_cast<std::size_t>(block.frames) * channel_count(block.layout);
    out.insert(out.end(), block.samples, block.samples + n);
  });
  return out;
}

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

// The whole-stream export-edge oracle: the concatenated working-rate `render_range`
// mix (block-boundary-invariant, so any working block size gives the same signal)
// resampled ONCE to `output_rate` through the shipped `resample_audio`, producing
// exactly the container-rate frame count covering the range. The byte-exact reference
// the concatenated container-rate export must equal (Constraint 1/4, D3).
std::vector<float> resample_oracle(ExportMonitor& monitor, const TimeRange& range,
                                   std::uint32_t output_rate) {
  const std::uint32_t working_rate = monitor.format().sample_rate;
  const std::int64_t out_fpf = Time::flicks_per_second / static_cast<std::int64_t>(output_rate);
  const std::vector<float> working = export_all(monitor, range, 64); // whole-range working mix
  const auto in_frames = static_cast<std::uint32_t>(working.size() / 2);
  const auto out_frames =
      static_cast<std::uint32_t>((range.end.flicks - range.start.flicks) / out_fpf);
  AudioBlock in_block{const_cast<float*>(working.data()), in_frames, ChannelLayout::Stereo,
                      working_rate};
  std::vector<float> out(static_cast<std::size_t>(out_frames) * 2, 0.0F);
  AudioBlock out_block{out.data(), out_frames, ChannelLayout::Stereo, output_rate};
  resample_audio(in_block, out_block);
  return out;
}

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

ObjectId add_tone(Document& doc, ObjectId comp, std::shared_ptr<ToneContent> tone) {
  const ObjectId cid = doc.add_content(std::move(tone));
  const ObjectId layer = doc.add_layer(cid, Affine::identity());
  doc.attach_layer(comp, layer);
  return layer;
}

} // namespace

// enforces: 12-audio#export-monitor-mixes-exactly-over-range
// enforces: 12-audio#mix-engine-mixes-layers-additively
TEST_CASE("an export of a multi-tone composition is byte-exact to summing the tone renders") {
  auto tone_a = std::make_shared<ToneContent>(440, 0.5F);
  auto tone_b = std::make_shared<ToneContent>(660, 0.25F);

  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_tone(document, comp, tone_a);
  const ObjectId lb = add_tone(document, comp, tone_b);
  document.set_layer_gain(lb, 0.5);

  const std::int64_t frames = 96;
  const TimeRange range{Time::zero(), Time{frames * k_fpf}};
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  const std::vector<float> got = export_all(monitor, range, 32);

  // Hand-summed reference: tone_a at gain 1, tone_b at gain 0.5, over the whole range
  // (a Static tone's per-window renders concatenate to the whole-range render).
  const std::vector<float> ra = render_tone(*tone_a, static_cast<std::uint32_t>(frames));
  const std::vector<float> rb = render_tone(*tone_b, static_cast<std::uint32_t>(frames));
  std::vector<float> want(static_cast<std::size_t>(frames) * 2, 0.0F);
  for (std::size_t i = 0; i < want.size(); ++i) {
    want[i] = ra[i] + 0.5F * rb[i];
  }
  REQUIRE(bytes_equal(got, want));

  // Block-boundary invariance: 96 frames tiled in 32s (3 whole blocks) vs 40s (40 +
  // 40 + a 16-frame partial trailing block) concatenate byte-identically.
  const std::vector<float> got40 = export_all(monitor, range, 40);
  REQUIRE(bytes_equal(got40, got));
}

// enforces: 12-audio#export-edge-resamples-working-to-container
TEST_CASE("an export to a differing container rate is byte-exact to a whole-stream resample") {
  auto tone_a = std::make_shared<ToneContent>(440, 0.5F);
  auto tone_b = std::make_shared<ToneContent>(660, 0.25F);

  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_tone(document, comp, tone_a);
  const ObjectId lb = add_tone(document, comp, tone_b);
  document.set_layer_gain(lb, 0.5);
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  SECTION("decimation container: 48 kHz working -> 44.1 kHz (coprime 160:147) -- flagship") {
    const TimeRange range{Time::zero(), Time{480 * k_fpf}}; // -> 441 output frames
    const std::vector<float> got = export_all_to(monitor, range, 44'100, 32);
    const std::vector<float> want = resample_oracle(monitor, range, 44'100);
    REQUIRE(!got.empty());
    REQUIRE(bytes_equal(got, want)); // no tolerance, phase accumulator + widened history carry
  }

  SECTION("up-sample container: 48 kHz working -> 96 kHz (1:2)") {
    const TimeRange range{Time::zero(), Time{96 * k_fpf}}; // -> 192 output frames
    const std::vector<float> got = export_all_to(monitor, range, 96'000, 32);
    const std::vector<float> want = resample_oracle(monitor, range, 96'000);
    REQUIRE(!got.empty());
    REQUIRE(bytes_equal(got, want));
  }

  SECTION("continuity: many export blocks concatenate to one whole-stream conversion") {
    const TimeRange range{Time::zero(), Time{480 * k_fpf}};
    const std::vector<float> many = export_all_to(monitor, range, 44'100, 8); // ~55 blocks
    const std::vector<float> want = resample_oracle(monitor, range, 44'100);
    REQUIRE(!many.empty());
    REQUIRE(bytes_equal(many, want)); // no boundary click, correct finite tail (Constraint 1/4)
    // Independent of the export block size (the SRC cursor + history carry across seams).
    const std::vector<float> few = export_all_to(monitor, range, 44'100, 128);
    REQUIRE(bytes_equal(many, few));
  }
}

// enforces: 12-audio#export-edge-resamples-working-to-container
TEST_CASE("a matched container rate reproduces the shipped 1:1 export byte-for-byte") {
  auto tone_a = std::make_shared<ToneContent>(440, 0.5F);
  auto tone_b = std::make_shared<ToneContent>(660, 0.25F);

  Document document;
  const ObjectId comp = document.add_composition(0.0, 0.0);
  add_tone(document, comp, tone_a);
  const ObjectId lb = add_tone(document, comp, tone_b);
  document.set_layer_gain(lb, 0.5);
  ExportMonitor monitor(document, comp, AudioFormat{k_rate, ChannelLayout::Stereo});

  const TimeRange range{Time::zero(), Time{96 * k_fpf}};
  const std::vector<float> one_to_one = export_all(monitor, range, 32);         // shipped 1:1 path
  const std::vector<float> matched = export_all_to(monitor, range, k_rate, 32); // matched edge
  REQUIRE(bytes_equal(matched, one_to_one));
}
