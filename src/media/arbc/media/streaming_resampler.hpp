#pragma once

#include <arbc/base/rt_safety.hpp> // ARBC_RT_NONBLOCKING (RT feed/produce annotations)
#include <arbc/media/audio_block.hpp>

#include <cstdint>
#include <vector>

namespace arbc {

// A stateful, continuous-stream front-end over the shipped block-anchored
// polyphase kernel `resample_audio` (doc 12:100-104, Constraint 2). It reuses
// the SAME exact-integer phase arithmetic as `resample_audio` and the SAME bank
// for the direction in play -- the frozen `k_resampler_coeffs` table when
// upsampling, the generated ratio-scaled widened-lowpass bank when decimating --
// no second algorithm, no tolerance -- but carries a rational input-sample cursor
// and a filter-support history so a long stream fed in successive chunks is
// byte-identical to a single whole-stream `resample_audio` reconstruction of the
// concatenated input: continuous across chunk boundaries (no per-block phase
// restart, no OOB-zero truncation at an interior seam). The audio twin of the one
// raster resampler serving every zoom remainder.
//
// Coordinate model: output frame 0 samples input frame 0's instant (the kernel's
// block anchor, extended to an absolute stream cursor). Output frame `n` samples
// native position `n * src_rate / dst_rate`; taps that fall before the stream
// start (absolute input index < 0) read as zero, exactly as `resample_audio`
// reads OOB taps -- so the leading edge matches byte-for-byte. The trailing edge
// never truncates: a frame is produced only once its whole filter support is
// resident (see `can_produce`), so an interior seam carries no zero tail.
//
// Both rate directions (`src_rate != dst_rate`): upsampling cuts the fixed sinc at
// the input Nyquist (correct when the output rate is higher); decimation cuts a
// ratio-scaled widened lowpass at the lower DEVICE Nyquist so it stays anti-aliased
// (D2). Per-channel over `channels` interleaved float32 frames.
//
// RT-safe (doc 12:30-34, Constraint 1, D5): all storage AND the active bank are
// pre-sized/generated in `configure` from `block_frames` and the rate ratio (the
// decimation bank's libm generation runs there, off the RT thread, never on the
// callback); `push_input` / `produce` allocate nothing, lock nothing, and run only
// ordered no-libm float32 MACs over the resident table. Single-owner and NOT
// thread-safe: the device drain owns it on the RT callback thread (like
// `d_scratch`); a transport-change flush is routed to it there, never mutated
// cross-thread.
class StreamingResampler {
public:
  StreamingResampler() = default;

  // Pre-size for continuous `channels`-channel conversion `src_rate -> dst_rate`
  // (either direction), fed at most `block_frames` input frames per `push_input`.
  // Generates the widened decimation bank when `src_rate > dst_rate`. Resets all
  // state.
  void configure(std::uint32_t src_rate, std::uint32_t dst_rate, std::uint32_t channels,
                 std::uint32_t block_frames);

  // Flush the phase cursor + filter-support history so the next output restarts a
  // fresh whole-stream reconstruction from the next pushed input frame (the
  // transport-change reprime, doc 12:200-206, D4). Keeps the configured sizing.
  // RT-side (audio.rt_safety): allocation-free, on the device callback thread.
  void reset() noexcept ARBC_RT_NONBLOCKING;

  // Append `frames` interleaved input frames (`channels * frames` floats). First
  // drops history frames no future output can reach (allocation-free compaction),
  // so a bounded window stays within the configured capacity. Precondition:
  // `frames <= block_frames`, with all producible output drained between pushes.
  void push_input(const float* samples, std::uint32_t frames) ARBC_RT_NONBLOCKING;

  // Whether the next output frame's whole filter support is resident. False means
  // push more input before producing.
  bool can_produce() const noexcept ARBC_RT_NONBLOCKING;

  // Produce one output frame (`channels` interleaved floats) into `out_frame` and
  // advance the output cursor. Precondition: `can_produce()`.
  void produce(float* out_frame) noexcept ARBC_RT_NONBLOCKING;

private:
  std::uint32_t d_src_rate{0};
  std::uint32_t d_dst_rate{0};
  std::uint32_t d_channels{0};
  std::uint32_t d_capacity_frames{0}; // d_history capacity, in frames

  // The active polyphase bank: the frozen input-Nyquist table (upsampling / matched,
  // `d_dec_coeffs` empty) or the generated widened device-Nyquist bank (decimation,
  // held in `d_dec_coeffs`). `d_taps` is the active tap count -- widened by the
  // decimation ratio, == the frozen tap count otherwise. The RT MAC selects the table
  // from `d_dec_coeffs.empty()` (O(1), no alloc) so a value copy of this object never
  // dangles a bank pointer.
  std::uint32_t d_taps{0};
  std::vector<float> d_dec_coeffs; // generated decimation bank storage; empty otherwise

  // The retained input window is the frames [d_hist_base, d_hist_base + d_hist_len)
  // in absolute stream coordinates; d_hist_base + d_hist_len is the running count
  // of frames ever pushed (the compaction preserves that invariant).
  std::vector<float> d_history;
  std::uint32_t d_hist_len{0};
  std::int64_t d_hist_base{0};
  std::int64_t d_out_index{0}; // absolute output frame cursor
};

} // namespace arbc
