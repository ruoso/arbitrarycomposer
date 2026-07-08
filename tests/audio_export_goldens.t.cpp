#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>
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
  monitor.render_range(range, block_frames,
                       [&](TimeRange, const AudioBlock& block, AudioResult) {
                         const std::size_t n =
                             static_cast<std::size_t>(block.frames) * channel_count(block.layout);
                         out.insert(out.end(), block.samples, block.samples + n);
                       });
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
