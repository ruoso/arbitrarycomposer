#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

// AudioFormat is a trivially-copyable value: it rides a composition record slab
// and is compared by value at nesting boundaries (doc 12:104).
static_assert(std::is_trivially_copyable_v<arbc::AudioFormat>);

TEST_CASE("k_working_audio is 48 kHz stereo, float32 by construction") {
  REQUIRE(arbc::k_working_audio == arbc::AudioFormat{48000, arbc::ChannelLayout::Stereo});
  REQUIRE(arbc::k_working_audio.sample_rate == 48000);
  REQUIRE(arbc::k_working_audio.layout == arbc::ChannelLayout::Stereo);
  // The working default carries two interleaved channels.
  REQUIRE(arbc::channel_count(arbc::k_working_audio.layout) == 2);
  // The defaulted value equals the designed default.
  static_assert(arbc::k_working_audio == arbc::AudioFormat{});
}

TEST_CASE("AudioFormat has value equality and copy semantics") {
  constexpr arbc::AudioFormat a = arbc::k_working_audio;
  arbc::AudioFormat b = a; // copy
  REQUIRE(a == b);
  REQUIRE(b.sample_rate == 48000);
  REQUIRE(b.layout == arbc::ChannelLayout::Stereo);

  // Either axis differing breaks equality -- each field is load-bearing.
  arbc::AudioFormat c = a;
  c.sample_rate = 44100;
  REQUIRE_FALSE(a == c);

  arbc::AudioFormat d = a;
  d.layout = arbc::ChannelLayout::Mono;
  REQUIRE_FALSE(a == d);
  REQUIRE(arbc::channel_count(d.layout) == 1);
}

} // namespace
