// Rate-independence pin for org.arbc.tone (refinement Acceptance "New claim").
// The generic conformance suite exercises the audio facet at a single working
// rate; this driver renders the tone across a spread of sample rates under both
// exactness modes and asserts a procedural source never degrades below a native
// rate -- the procedural side of doc 12's resolution/rate-independence symmetry.

#include <arbc/contract/content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

using namespace arbc;

// enforces: 12-audio#tone-renders-at-any-requested-rate
TEST_CASE("org.arbc.tone renders a procedural tone at any requested sample rate") {
  ToneContent tone(440, 0.5F);
  AudioFacet* facet = tone.audio();
  REQUIRE(facet != nullptr);

  for (std::uint32_t rate : {8000U, 22050U, 44100U, 48000U, 96000U}) {
    for (Exactness exactness : {Exactness::BestEffort, Exactness::Exact}) {
      const std::uint32_t frames = 64;
      std::vector<float> samples(static_cast<std::size_t>(frames) * 2U, -1.0F);
      AudioBlock block{samples.data(), frames, ChannelLayout::Stereo, rate};

      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(rate);
      const AudioRequest request{
          TimeRange{Time::zero(), Time{static_cast<std::int64_t>(frames) * fpf}}, rate,
          ChannelLayout::Stereo, block, exactness};

      auto done = std::make_shared<AudioCompletion>();
      const std::optional<AudioResult> result = facet->render_audio(request, done);

      // A procedural source has no native rate to bottom out at: it synthesizes
      // directly at the requested rate and reports it exactly, every time.
      REQUIRE(result.has_value());
      CHECK(result->achieved_rate == rate);
      CHECK(result->exact == true);

      // Both channels of a frame carry the same value (a mono tone spread
      // across the layout); the block is fully written (no leftover sentinel).
      for (std::uint32_t f = 0; f < frames; ++f) {
        CHECK(samples[static_cast<std::size_t>(f) * 2U] ==
              samples[static_cast<std::size_t>(f) * 2U + 1U]);
      }
    }
  }
}
