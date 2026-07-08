#pragma once

#include <arbc/media/audio_block.hpp>

#include <cstdint>
#include <vector>

namespace arbc {

// A stateful, continuous-stream front-end over the shipped block-anchored
// polyphase kernel `resample_audio` (doc 12:100-104, Constraint 2). It reuses
// the SAME frozen `k_resampler_coeffs` table and the SAME exact-integer phase
// arithmetic as `resample_audio` -- no second table, no second algorithm, no
// tolerance -- but carries a rational input-sample cursor and a filter-support
// history so a long stream fed in successive chunks is byte-identical to a single
// whole-stream `resample_audio` reconstruction of the concatenated input:
// continuous across chunk boundaries (no per-block phase restart, no OOB-zero
// truncation at an interior seam). The audio twin of the one raster resampler
// serving every zoom remainder.
//
// Coordinate model: output frame 0 samples input frame 0's instant (the kernel's
// block anchor, extended to an absolute stream cursor). Output frame `n` samples
// native position `n * src_rate / dst_rate`; taps that fall before the stream
// start (absolute input index < 0) read as zero, exactly as `resample_audio`
// reads OOB taps -- so the leading edge matches byte-for-byte. The trailing edge
// never truncates: a frame is produced only once its whole filter support is
// resident (see `can_produce`), so an interior seam carries no zero tail.
//
// UPSAMPLING only (`src_rate < dst_rate`), mirroring `resample_audio`'s
// precondition: the fixed sinc is cut at the input Nyquist -- correct
// band-limited reconstruction when the output rate is higher. Per-channel over
// `channels` interleaved float32 frames.
//
// RT-safe (doc 12:30-34, Constraint 1, D5): all storage is pre-sized in
// `configure` from `block_frames` and the rate ratio; `push_input` / `produce`
// allocate nothing, lock nothing, and run only ordered no-libm float32 MACs over
// the checked-in table. Single-owner and NOT thread-safe: the device drain owns
// it on the RT callback thread (like `d_scratch`); a transport-change flush is
// routed to it there, never mutated cross-thread.
class StreamingResampler {
public:
  StreamingResampler() = default;

  // Pre-size for continuous `channels`-channel upsampling `src_rate -> dst_rate`,
  // fed at most `block_frames` input frames per `push_input`. Resets all state.
  void configure(std::uint32_t src_rate, std::uint32_t dst_rate, std::uint32_t channels,
                 std::uint32_t block_frames);

  // Flush the phase cursor + filter-support history so the next output restarts a
  // fresh whole-stream reconstruction from the next pushed input frame (the
  // transport-change reprime, doc 12:200-206, D4). Keeps the configured sizing.
  void reset() noexcept;

  // Append `frames` interleaved input frames (`channels * frames` floats). First
  // drops history frames no future output can reach (allocation-free compaction),
  // so a bounded window stays within the configured capacity. Precondition:
  // `frames <= block_frames`, with all producible output drained between pushes.
  void push_input(const float* samples, std::uint32_t frames);

  // Whether the next output frame's whole filter support is resident. False means
  // push more input before producing.
  bool can_produce() const noexcept;

  // Produce one output frame (`channels` interleaved floats) into `out_frame` and
  // advance the output cursor. Precondition: `can_produce()`.
  void produce(float* out_frame) noexcept;

private:
  std::uint32_t d_src_rate{0};
  std::uint32_t d_dst_rate{0};
  std::uint32_t d_channels{0};
  std::uint32_t d_capacity_frames{0}; // d_history capacity, in frames

  // The retained input window is the frames [d_hist_base, d_hist_base + d_hist_len)
  // in absolute stream coordinates; d_hist_base + d_hist_len is the running count
  // of frames ever pushed (the compaction preserves that invariant).
  std::vector<float> d_history;
  std::uint32_t d_hist_len{0};
  std::int64_t d_hist_base{0};
  std::int64_t d_out_index{0}; // absolute output frame cursor
};

} // namespace arbc
