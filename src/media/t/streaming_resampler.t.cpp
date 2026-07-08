#include <arbc/media/audio_block.hpp>
#include <arbc/media/audio_resampler.hpp>
#include <arbc/media/streaming_resampler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// Component-level tests for the `arbc::media` streaming front-end over the shipped
// block-anchored polyphase kernel (audio.device_edge_resample, Constraint 2/4).
// The whole point is a tolerance-free equivalence: a long stream fed in arbitrary
// successive chunks is BYTE-IDENTICAL to a single whole-stream `resample_audio`
// reconstruction of the concatenated input -- no per-chunk phase restart, no
// interior boundary click -- reusing the SAME frozen coefficient table and exact
// phase math (no second algorithm). Plus `reset()` restarts a fresh
// reconstruction (the device-edge transport-change flush, D4).
//
// enforces: 12-audio#device-edge-resamples-working-to-device

namespace {

using namespace arbc;

// A fixed non-trivial input signal (deterministic, libm-free): a broadband
// pseudo-random sequence in [-1, 1) per channel, so every tap contributes and a
// fractional phase is provably distinct from a hold. The equivalence under test is
// purely between two paths over this identical input, so its exact shape is
// irrelevant -- only that it is non-constant and non-periodic.
std::vector<float> make_signal(std::uint32_t frames, std::uint32_t channels) {
  std::vector<float> s(static_cast<std::size_t>(frames) * channels, 0.0F);
  for (std::uint32_t c = 0; c < channels; ++c) {
    std::uint32_t state = 0x1234567u + 0x9E3779B9u * (c + 1); // distinct per channel
    for (std::uint32_t f = 0; f < frames; ++f) {
      state = state * 1664525u + 1013904223u; // LCG
      const float v = static_cast<float>(state >> 8) / static_cast<float>(1u << 24) * 2.0F - 1.0F;
      s[static_cast<std::size_t>(f) * channels + c] = v;
    }
  }
  return s;
}

std::vector<float> whole_stream(const std::vector<float>& in, std::uint32_t channels,
                                std::uint32_t src_rate, std::uint32_t dst_rate,
                                std::uint32_t out_frames) {
  const std::uint32_t in_frames = static_cast<std::uint32_t>(in.size() / channels);
  std::vector<float> out(static_cast<std::size_t>(out_frames) * channels, 0.0F);
  const ChannelLayout layout = channels == 1 ? ChannelLayout::Mono : ChannelLayout::Stereo;
  AudioBlock in_block{const_cast<float*>(in.data()), in_frames, layout, src_rate};
  AudioBlock out_block{out.data(), out_frames, layout, dst_rate};
  resample_audio(in_block, out_block);
  return out;
}

// Feed `in` through the streaming resampler in successive chunks of the given
// sizes (each <= block_frames), draining every producible output frame between
// pushes, and return the concatenated output.
std::vector<float> stream_in_chunks(const std::vector<float>& in, std::uint32_t channels,
                                    std::uint32_t src_rate, std::uint32_t dst_rate,
                                    std::uint32_t block_frames,
                                    const std::vector<std::uint32_t>& chunk_sizes) {
  StreamingResampler r;
  r.configure(src_rate, dst_rate, channels, block_frames);
  std::vector<float> out;
  std::vector<float> frame(channels, 0.0F);
  const std::uint32_t in_frames = static_cast<std::uint32_t>(in.size() / channels);
  std::uint32_t pos = 0;
  std::size_t ci = 0;
  while (pos < in_frames) {
    const std::uint32_t want = chunk_sizes[ci % chunk_sizes.size()];
    const std::uint32_t n = pos + want <= in_frames ? want : in_frames - pos;
    r.push_input(in.data() + static_cast<std::size_t>(pos) * channels, n);
    pos += n;
    ++ci;
    while (r.can_produce()) {
      r.produce(frame.data());
      out.insert(out.end(), frame.begin(), frame.end());
    }
  }
  return out;
}

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

// enforces: 12-audio#device-edge-resamples-working-to-device
TEST_CASE("streaming resampler is byte-identical to a whole-stream reconstruction (integer 1:2)") {
  const std::uint32_t channels = 2;
  const std::vector<float> in = make_signal(200, channels);
  // Chunk sizes deliberately not aligned to any block boundary.
  const std::vector<std::uint32_t> chunks = {7, 13, 32, 5, 19};
  const std::vector<float> got =
      stream_in_chunks(in, channels, 48'000, 96'000, 32, chunks);
  const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / channels);
  const std::vector<float> want = whole_stream(in, channels, 48'000, 96'000, out_frames);
  REQUIRE(out_frames > 0);
  REQUIRE(bytes_equal(got, want)); // no tolerance
}

// enforces: 12-audio#device-edge-resamples-working-to-device
TEST_CASE("streaming resampler is byte-identical to a whole-stream reconstruction (coprime 2:3)") {
  const std::uint32_t channels = 2;
  const std::vector<float> in = make_signal(240, channels);
  const std::vector<std::uint32_t> chunks = {11, 24, 3, 17, 24};
  const std::vector<float> got =
      stream_in_chunks(in, channels, 32'000, 48'000, 24, chunks);
  const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / channels);
  const std::vector<float> want = whole_stream(in, channels, 32'000, 48'000, out_frames);
  REQUIRE(out_frames > 0);
  REQUIRE(bytes_equal(got, want));
}

// enforces: 12-audio#device-edge-resamples-working-to-device
TEST_CASE("streaming resampler output is independent of the chunk boundaries") {
  const std::uint32_t channels = 1;
  const std::vector<float> in = make_signal(180, channels);
  const std::vector<float> a =
      stream_in_chunks(in, channels, 32'000, 48'000, 32, {1, 2, 3, 5, 8, 13});
  const std::vector<float> b =
      stream_in_chunks(in, channels, 32'000, 48'000, 32, {32, 32, 32, 32, 32, 32});
  REQUIRE(a.size() == b.size());
  REQUIRE(bytes_equal(a, b)); // continuity: no boundary click, any chunking
}

// enforces: 12-audio#device-edge-resamples-working-to-device
TEST_CASE("streaming resampler reset restarts a fresh whole-stream reconstruction") {
  const std::uint32_t channels = 2;
  const std::uint32_t block_frames = 32;
  const std::vector<float> first = make_signal(150, channels);
  const std::vector<float> second = make_signal(160, channels); // distinct new stream

  StreamingResampler r;
  r.configure(48'000, 96'000, channels, block_frames);

  // Run a non-trivial first stream so the phase cursor + history are mid-stream.
  std::vector<float> frame(channels, 0.0F);
  const std::uint32_t first_frames = static_cast<std::uint32_t>(first.size() / channels);
  for (std::uint32_t pos = 0; pos < first_frames; pos += block_frames) {
    const std::uint32_t n = pos + block_frames <= first_frames ? block_frames : first_frames - pos;
    r.push_input(first.data() + static_cast<std::size_t>(pos) * channels, n);
    while (r.can_produce()) {
      r.produce(frame.data());
    }
  }

  // Flush: the next output must be a fresh reconstruction of `second` from frame 0,
  // carrying NO filter tail of `first`.
  r.reset();
  std::vector<float> got;
  const std::uint32_t second_frames = static_cast<std::uint32_t>(second.size() / channels);
  for (std::uint32_t pos = 0; pos < second_frames; pos += block_frames) {
    const std::uint32_t n = pos + block_frames <= second_frames ? block_frames : second_frames - pos;
    r.push_input(second.data() + static_cast<std::size_t>(pos) * channels, n);
    while (r.can_produce()) {
      r.produce(frame.data());
      got.insert(got.end(), frame.begin(), frame.end());
    }
  }
  const std::uint32_t out_frames = static_cast<std::uint32_t>(got.size() / channels);
  const std::vector<float> want = whole_stream(second, channels, 48'000, 96'000, out_frames);
  REQUIRE(out_frames > 0);
  REQUIRE(bytes_equal(got, want));
}
