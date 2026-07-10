#pragma once

#include <arbc/base/rational_time.hpp>        // Rational
#include <arbc/base/rt_safety.hpp>            // ARBC_RT_NONBLOCKING (RT callback annotations)
#include <arbc/base/time.hpp>                 // Time
#include <arbc/base/transform.hpp>            // Affine (live viewport camera sample)
#include <arbc/media/audio_block.hpp>         // ChannelLayout
#include <arbc/media/streaming_resampler.hpp> // StreamingResampler
#include <arbc/runtime/device_sink.hpp>       // DeviceSink, DeviceFormat
#include <arbc/runtime/lookahead_pump.hpp>    // LookaheadPump
#include <arbc/runtime/transport.hpp>         // Transport

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace arbc {

// The interactive audio driver and the transport's audio-clock master for
// `arbc::runtime` (L5, doc 12:155-178): the thin runtime adapter the lookahead
// note reserved (`lookahead_pump.cpp:35-37`, "a lock-free double-buffer for a true
// RT device thread is device_monitor's concern"). It binds a `Transport`, a
// `LookaheadPump`, and a `DeviceSink`, and owns clock mastering:
//
//   * RT callback (pure consume + count). For each output block the device needs,
//     it calls `LookaheadPump::drain` to copy an already-mixed block (or silence +
//     an underrun on a starved block, never an inline mix), converts the working
//     format to the device format at this edge (doc 12:100-104), and bumps one
//     atomic delivered-frame counter -- its only mutation. No `render_audio` /
//     `mix_composition` / `pull_audio`, no heap allocation on this thread
//     (Constraint 1). Full lock-freedom of `drain` is the reserved RT double-buffer
//     that `audio.rt_safety` annotates build-failingly; the mandatory lock-free
//     surface THIS task lands is the mastered-playhead publish below (D3).
//   * Mastering (single-owner). A single non-RT owner thread reads the delivered-
//     frame delta and advances the `Transport` by `delivered_frames / device_rate`
//     as an exact `Time` (doc 12:171-172): the device frame count IS the clock, no
//     wall-clock read. The RT thread never touches the `Transport`, so its
//     single-owner discipline (`transport.hpp:26-30`) is preserved -- host
//     `seek`/`set_rate` are routed to the same owner thread (D2).
//   * Video chases audio. The mastered playhead is published as a lock-free atomic
//     `Time` snapshot (release); the pump's `playhead_source` and video viewports
//     on the same transport sample it (acquire) to schedule against the audio clock
//     -- "video chases audio, never the reverse" (doc 12:173-175, doc 01:103-106,
//     D3). A `seek`/`set_rate` rebases the master and calls
//     `pump.notify_transport_change` (flush + reprime).
//   * One monitor per transport; free-run fallback. Attaching a second monitor to a
//     transport is rejected (a `logic_error`); a transport with NO monitor free-runs
//     on the injected system clock exactly as before (doc 12:176-178).
//
// Concurrency: the delivered-frame counter is the ONLY channel from the RT thread to
// the owner thread; the published `Time` snapshot is the only channel from the owner
// thread to the pump/viewport readers -- both atomic, no lock, "only an immutable
// sampled `Time` crosses threads" (`transport.hpp:26-30`). This is the new
// cross-thread surface the transport reserved (`transport.md:338-339`); it carries
// this task's TSan obligation.
struct DeviceMonitorConfig {
  std::uint32_t working_rate{0};
  ChannelLayout working_layout{ChannelLayout::Stereo};
  std::uint32_t block_frames{0};
  // Idle master-thread park interval; never appears in a test assertion (mirrors
  // `LookaheadPumpConfig::tick_period`). The mastering step is driven by the
  // delivered-frame counter, not by this timeout.
  std::chrono::steady_clock::duration master_period{std::chrono::milliseconds(2)};
  // Live viewport-camera source for Spatial camera-follow (audio.spatial_camera_follow,
  // doc 12 "Camera follow", Decision D1). When set AND the ring's static seed is
  // Spatial, the owner thread samples this each mastering step and, on a change,
  // re-seeds the mix listener + flushes/reprimes the ring (never re-seating the drain
  // cursor — the playhead is unmoved). Returning an L0 `Affine` — not a `Viewport&` —
  // mirrors the pump's `playhead_source`/`direction_source` injected-source idiom and
  // keeps `DeviceMonitor` free of any dependency on the L4 compositor `Viewport`; the
  // host closure (itself L5) closes over the live viewport. Empty => the static seed
  // stands (no follow), so every existing device-path drain is byte-unchanged.
  std::function<Affine()> camera_source{};
};

class DeviceMonitor {
public:
  // Binds `transport`, `pump`, and `sink` (references, not owned). Starts the owner
  // (mastering) thread and opens the device stream. A device rate ABOVE the working
  // rate is upsampled at the edge through the frozen input-Nyquist polyphase table
  // (audio.device_edge_resample); a device rate BELOW it is decimated through a
  // ratio-scaled widened lowpass cut at the device Nyquist
  // (audio.device_edge_decimation); a device rate EQUAL to it keeps the 1:1 drain.
  // Throws `std::logic_error` if `transport` already has a device monitor, and
  // `std::invalid_argument` on a degenerate config (zero working rate / block).
  DeviceMonitor(Transport& transport, LookaheadPump& pump, DeviceSink& sink,
                DeviceMonitorConfig config);
  ~DeviceMonitor();

  DeviceMonitor(const DeviceMonitor&) = delete;
  DeviceMonitor& operator=(const DeviceMonitor&) = delete;

  // The lock-free mastered-playhead snapshot (acquire): wire the pump's
  // `playhead_source` and any video viewport on this transport to it so they chase
  // the audio clock (D3).
  Time playhead_snapshot() const noexcept { return d_published.load(std::memory_order_acquire); }

  // The playback direction (sign of the transport rate; 0-rate -> forward), sampled
  // lock-free for the pump's `direction_source`.
  int direction_snapshot() const noexcept { return d_direction.load(std::memory_order_acquire); }

  // Host controls, routed to the single owner thread (which alone mutates the
  // `Transport`): each rebases the master's sample origin and reprimes the pump
  // (flush + reprime). Non-blocking; use `flush_master()` to await application.
  void seek(Time t);
  void set_rate(Rational rate);

  // Poke and BLOCK until the owner thread completes one further mastering step
  // (applies any pending seek/rate, advances the transport from the delivered-frame
  // delta, republishes). Waits on a condition (the step counter), never a wall
  // clock. Returns the new step count. The deterministic test barrier.
  std::uint64_t flush_master();

  // Behavioral counters (doc 16:54-62), wall-clock-free.
  std::uint64_t delivered_frames() const noexcept {
    return d_delivered.load(std::memory_order_acquire);
  }
  std::uint64_t underruns() const noexcept { return d_underruns.load(std::memory_order_acquire); }
  std::uint64_t master_steps() const noexcept {
    return d_master_steps.load(std::memory_order_acquire);
  }
  // Count of drain-cursor realignments consumed by the RT callback: one per applied
  // seek/rate rebase, incremented when `fill_rt` re-seats `d_drain_index` to the
  // reprimed block window (audio.seek_drain_realign). Wall-clock-free.
  std::uint64_t drain_realigns() const noexcept {
    return d_drain_realigns.load(std::memory_order_acquire);
  }
  // Count of live-camera listener re-seeds (audio.spatial_camera_follow): the initial
  // seed plus one per subsequent camera change; 0 for a Flat seed or an absent
  // `camera_source`. A still camera never advances it (Constraint 4). Wall-clock-free.
  std::uint64_t listener_reseeds() const noexcept {
    return d_listener_reseeds.load(std::memory_order_acquire);
  }

private:
  // The device RT callback body and its RT-thread callees. All are
  // `ARBC_RT_NONBLOCKING` (audio.rt_safety): under RealtimeSanitizer their whole
  // call graph must issue no lock / allocation / blocking syscall; `fill_rt` arms
  // the `RtScope` guard for the debug-hardened Layer-B backstop.
  void fill_rt(float* out, std::uint32_t frames) noexcept ARBC_RT_NONBLOCKING; // device RT callback
  void drain_block() noexcept ARBC_RT_NONBLOCKING; // pull one working block into d_scratch (RT)
  void run_master();                               // the owner-thread loop
  void master_step();
  // Remix `frames` interleaved working-LAYOUT frames (already at the device RATE --
  // the resampler runs first) to the device layout. Pure, allocation-free (memcpy
  // for an identity layout; average/duplicate for a mono<->stereo remap).
  void convert_frames(const float* src, float* dst,
                      std::uint32_t frames) const noexcept ARBC_RT_NONBLOCKING;
  // The first output-block index the device draws from: the block covering the
  // transport's current playhead, so the drain cursor starts aligned with the clock.
  std::int64_t start_block_index() const;

  Transport& d_transport;
  LookaheadPump& d_pump;
  DeviceSink& d_sink;
  DeviceMonitorConfig d_config;
  DeviceFormat d_device{};
  std::int64_t d_flicks_per_frame{0};         // device-rate frame duration (clock advance)
  std::int64_t d_working_flicks_per_frame{0}; // working-rate frame duration (drain block span)
  std::uint32_t d_working_channels{0};
  std::uint32_t d_device_channels{0};
  bool d_resampling{false}; // device_rate != working_rate: engage the streaming resampler

  // --- RT-callback-owned state (touched only on the device thread) ------------
  std::vector<float> d_scratch;  // one working-format block, pre-allocated
  std::int64_t d_drain_index{0}; // next output-block index to drain
  std::uint32_t d_carry_frames{0};
  std::uint32_t d_carry_pos{0};
  // Device-edge sample-rate conversion state (D1/D5), pre-sized at construction and
  // owned solely by the RT callback (like `d_scratch`); idle when `!d_resampling`.
  StreamingResampler d_resampler;
  std::vector<float> d_src_out; // one device-frame batch, working layout, pre-allocated

  // --- cross-thread published surface (D3) ------------------------------------
  std::atomic<std::uint64_t> d_delivered{0};      // RT writes (release), owner reads
  std::atomic<Time> d_published{Time::zero()};    // owner writes (release), readers acquire
  std::atomic<int> d_direction{1};                // owner writes, readers acquire
  std::atomic<std::uint64_t> d_underruns{0};      // RT writes
  std::atomic<std::uint64_t> d_drain_realigns{0}; // RT writes
  // Transport-change flush request for the RT-owned resampler (D4/Constraint 8): the
  // owner sets it on a rebase (release), the RT callback consumes it (acquire) and
  // flushes the resampler's phase + history. A control channel only -- the resampler
  // DSP buffers stay RT-single-owner, adding no shared mutable audio state.
  std::atomic<bool> d_resampler_flush{false};
  // Transport-change drain-realign request (audio.seek_drain_realign, D1): on a
  // rebase the owner computes the block index covering the reprimed playhead and
  // publishes it in `d_realign_index` (release), then sets `d_realign_request`
  // (release). The RT callback `exchange`-consumes the flag (acquire) at the top of
  // `fill_rt`, re-seats `d_drain_index` to the published index, and drops the
  // pre-change working carry. A control channel only -- `d_drain_index` stays
  // RT-single-owner; the owner never writes it (D1, Constraint 1).
  std::atomic<std::int64_t> d_realign_index{0};
  std::atomic<bool> d_realign_request{false};

  // --- owner-thread-owned state -----------------------------------------------
  std::uint64_t d_last_delivered{0};
  // Live-camera follow state (audio.spatial_camera_follow, D1/D2), owner-thread only.
  // `d_spatial_base` is the ring's static Spatial seed captured ONCE at construction
  // (nullopt in Flat mode => the follow is inert): a camera re-seed carries its
  // `viewport_w/h` and `sub_audible`, swapping only `listener` and the derived
  // `accum_atten`. `d_last_camera` is the last-applied camera; an exact
  // `Affine`-coefficient compare against it gates the re-seed so a still camera costs
  // nothing (D4).
  std::optional<Spatialization> d_spatial_base;
  std::optional<Affine> d_last_camera;
  std::atomic<std::uint64_t> d_listener_reseeds{0};

  // owner-thread lifecycle (mirrors the pump / HousekeepingThread template)
  mutable std::mutex d_master_mutex;
  std::condition_variable d_master_cv;          // parks the owner (poke / stop / timeout)
  std::condition_variable d_master_progress_cv; // wakes a blocked flush_master()
  bool d_master_stop{false};
  bool d_master_poke{false};
  std::optional<Time> d_pending_seek;
  std::optional<Rational> d_pending_rate;
  std::atomic<std::uint64_t> d_master_steps{0};

  std::thread d_master_thread; // started before the sink stream in the ctor body
};

} // namespace arbc
