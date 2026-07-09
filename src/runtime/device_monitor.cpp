#include <arbc/base/rt_safety.hpp> // RtScope, ARBC_RT_NONBLOCKING
#include <arbc/runtime/device_monitor.hpp>

#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_set>

namespace arbc {

namespace {

// The one-monitor-per-transport guard (doc 12:177-178, Constraint 6). It lives here,
// NOT inside `Transport`: the transport is deliberately single-owner, no-sync value
// state (`transport.hpp:26-30`, D2), so the ownership registry is a monitor-layer
// concern. Non-RT: touched only at monitor construction/destruction.
std::mutex& registry_mutex() {
  static std::mutex m;
  return m;
}
std::unordered_set<const Transport*>& registered_transports() {
  static std::unordered_set<const Transport*> s;
  return s;
}

int rate_sign(Rational rate) {
  // Sign is carried in the numerator (rational_time.hpp:31); a 0 rate is
  // playing-but-frozen (doc 11:97-101) and chases forward, matching the pump's
  // "empty direction_source -> forward" default.
  if (rate.num() < 0) {
    return -1;
  }
  return 1;
}

} // namespace

DeviceMonitor::DeviceMonitor(Transport& transport, LookaheadPump& pump, DeviceSink& sink,
                             DeviceMonitorConfig config)
    : d_transport(transport), d_pump(pump), d_sink(sink), d_config(config),
      d_device(sink.format()) {
  d_working_channels = channel_count(config.working_layout);
  d_device_channels = channel_count(d_device.layout);
  if (config.working_rate == 0 || config.block_frames == 0) {
    throw std::invalid_argument("DeviceMonitor: working_rate and block_frames must be non-zero");
  }
  // Either device-rate direction engages the streaming resampler at the edge: above
  // the working rate upsamples through the frozen input-Nyquist table; below it
  // decimates through a ratio-scaled widened lowpass cut at the device Nyquist
  // (audio.device_edge_decimation, D2). Only a matched rate keeps the 1:1 drain.
  d_resampling = d_device.sample_rate != config.working_rate;

  // Two rate axes, kept distinct (Constraint 3): the clock advance is device-rate
  // framed; the drain block span is working-rate framed. They coincide only at a
  // matched rate.
  d_flicks_per_frame = Time::flicks_per_second / static_cast<std::int64_t>(d_device.sample_rate);
  d_working_flicks_per_frame =
      Time::flicks_per_second / static_cast<std::int64_t>(config.working_rate);
  d_scratch.assign(static_cast<std::size_t>(config.block_frames) * d_working_channels, 0.0F);
  if (d_resampling) {
    // Pre-size the RT-owned resampler state and its device-frame batch buffer (D5):
    // no allocation on the callback thread.
    d_resampler.configure(config.working_rate, d_device.sample_rate, d_working_channels,
                          config.block_frames);
    d_src_out.assign(static_cast<std::size_t>(config.block_frames) * d_working_channels, 0.0F);
  }
  d_drain_index = start_block_index();
  d_published.store(transport.position(), std::memory_order_release);
  d_direction.store(rate_sign(transport.rate()), std::memory_order_release);

  // Reject a second monitor on this transport (last throwing step: nothing after it
  // needs unwinding on the duplicate path, and this monitor never registered).
  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    if (!registered_transports().insert(&transport).second) {
      throw std::logic_error("DeviceMonitor: transport already has a device monitor");
    }
  }

  // Capture the ring's static Spatial seed once (audio.spatial_camera_follow, D2): a
  // live camera re-seed carries its viewport extent + sub-audible threshold, swapping
  // only the listener. `nullopt` (Flat) => camera-follow is inert. A plain value read,
  // race-free with the pump tick's read-only `prime` (no writer until the first
  // re-seed, which this owner thread alone issues).
  d_spatial_base = d_pump.spatial();

  d_master_thread = std::thread(&DeviceMonitor::run_master, this);
  // Open the device stream LAST: everything the RT callback reads is initialized.
  d_sink.start([this](float* out, std::uint32_t frames) { fill_rt(out, frames); });
}

DeviceMonitor::~DeviceMonitor() {
  // Quiesce the RT thread first: after `stop()` returns, `fill_rt` is never called
  // again, so tearing down the owner thread cannot race a live callback.
  d_sink.stop();
  {
    std::lock_guard<std::mutex> lock(d_master_mutex);
    d_master_stop = true;
  }
  d_master_cv.notify_one();
  d_master_progress_cv.notify_all();
  if (d_master_thread.joinable()) {
    d_master_thread.join();
  }
  {
    std::lock_guard<std::mutex> lock(registry_mutex());
    registered_transports().erase(&d_transport);
  }
}

std::int64_t DeviceMonitor::start_block_index() const {
  // The first output-block index the device draws from: the block covering the
  // transport's current playhead, so the drain cursor and the mastered clock start
  // aligned (both derive from the same position). The drain block index is in
  // WORKING-rate frames (Constraint 3), so span uses the working-rate duration.
  const std::int64_t span =
      static_cast<std::int64_t>(d_config.block_frames) * d_working_flicks_per_frame;
  if (span == 0) {
    return 0;
  }
  const std::int64_t p = d_transport.position().flicks;
  // Floored, sign-correct (mirrors LookaheadRing::block_index_at).
  return p >= 0 ? p / span : -((-p + span - 1) / span);
}

void DeviceMonitor::convert_frames(const float* src, float* dst,
                                   std::uint32_t frames) const ARBC_RT_NONBLOCKING {
  if (d_config.working_layout == d_device.layout) {
    std::memcpy(dst, src, static_cast<std::size_t>(frames) * d_working_channels * sizeof(float));
    return;
  }
  const std::uint32_t sc = d_working_channels;
  const std::uint32_t dc = d_device_channels;
  for (std::uint32_t f = 0; f < frames; ++f) {
    if (d_device.layout == ChannelLayout::Mono) {
      // Downmix: average the source channels into the single device channel.
      float acc = 0.0F;
      for (std::uint32_t c = 0; c < sc; ++c) {
        acc += src[static_cast<std::size_t>(f) * sc + c];
      }
      dst[f] = acc / static_cast<float>(sc);
    } else {
      // Up from mono: duplicate the single source channel across device channels.
      const float v = src[static_cast<std::size_t>(f) * sc];
      for (std::uint32_t c = 0; c < dc; ++c) {
        dst[static_cast<std::size_t>(f) * dc + c] = v;
      }
    }
  }
}

void DeviceMonitor::drain_block() ARBC_RT_NONBLOCKING {
  AudioBlock block{d_scratch.data(), d_config.block_frames, d_config.working_layout,
                   d_config.working_rate};
  AudioResult meta{};
  // Prepared block copied out, or silence + an underrun on a starved block --
  // never an inline mix (the RT-safety invariant the whole engine buys).
  if (!d_pump.drain(d_drain_index, block, meta)) {
    d_underruns.fetch_add(1, std::memory_order_relaxed);
  }
  ++d_drain_index;
  d_carry_frames = d_config.block_frames;
  d_carry_pos = 0;
}

void DeviceMonitor::fill_rt(float* out, std::uint32_t frames) ARBC_RT_NONBLOCKING {
  // Arm the debug RT guard for the whole callback body (audio.rt_safety, Layer B,
  // Decision D1): under the debug-hardened build any heap allocation on this thread
  // now aborts build-failingly, and `RtScope::allocations()` counts them for the
  // enforcement test. `ARBC_RT_NONBLOCKING` puts the same body under
  // RealtimeSanitizer on the Clang rtsan lane (Layer A). The guard is a thread-local
  // increment -- no allocation, no lock.
  RtScope rt_guard;
  // RT-safe (Constraint 1): only `pump.drain` (a LOCK-FREE seqlock read, never a
  // mix), a pure layout conversion (and the pre-sized streaming resampler under
  // SRC), and one atomic increment. No allocation -- `d_scratch` / `d_src_out` /
  // the resampler are pre-sized; `out` is the caller's device buffer.
  //
  // Drain-cursor realign (audio.seek_drain_realign, D1/Constraint 3): on a rebase the
  // owner published the block index covering the reprimed playhead; re-seat the cursor
  // to it and drop the stale pre-change working carry so BOTH drain paths restart
  // byte-exact from the first post-change frame. Precedes the `d_resampling` branch so
  // the matched-rate and SRC paths realign identically; a single acquire-`exchange`
  // consumes the request exactly once (Constraint 6), no allocation/lock/refcount.
  // enforces: 12-audio#device-drain-realigns-on-transport-change
  if (d_realign_request.exchange(false, std::memory_order_acquire)) {
    d_drain_index = d_realign_index.load(std::memory_order_acquire);
    d_carry_frames = 0;
    d_carry_pos = 0;
    d_drain_realigns.fetch_add(1, std::memory_order_relaxed);
  }
  if (!d_resampling) {
    // Matched-rate 1:1 drain: byte-for-byte the working mix, zero SRC (Constraint 5).
    std::uint32_t written = 0;
    while (written < frames) {
      if (d_carry_frames == 0) {
        drain_block();
      }
      const std::uint32_t n = frames - written < d_carry_frames ? frames - written : d_carry_frames;
      convert_frames(d_scratch.data() + static_cast<std::size_t>(d_carry_pos) * d_working_channels,
                     out + static_cast<std::size_t>(written) * d_device_channels, n);
      written += n;
      d_carry_pos += n;
      d_carry_frames -= n;
    }
    d_delivered.fetch_add(frames, std::memory_order_release);
    return;
  }

  // Device-edge SRC (device_rate != working_rate: upsample above, decimate below).
  // A transport change flushes the
  // resampler's filter memory so post-flush output restarts byte-exact (D4). The
  // drain cursor was already re-seated at the top of `fill_rt` (the realign consume,
  // audio.seek_drain_realign), which also dropped the pre-change working carry; this
  // co-triggered flush independently resets the resampler's DSP state (Constraint 3).
  if (d_resampler_flush.exchange(false, std::memory_order_acquire)) {
    d_resampler.reset();
    d_carry_frames = 0;
    d_carry_pos = 0;
  }
  std::uint32_t written = 0;
  while (written < frames) {
    // Feed working-rate blocks until the next device frame's filter support is
    // resident. Upsampling consumes fewer working frames than it emits; decimation
    // consumes MORE (the widened support strides past several input frames per output
    // frame), so this feed loop may pull several working blocks before one device
    // frame is producible -- the inverse frame-count direction (Constraint 4).
    while (!d_resampler.can_produce()) {
      if (d_carry_frames == 0) {
        drain_block();
      }
      d_resampler.push_input(d_scratch.data() +
                                 static_cast<std::size_t>(d_carry_pos) * d_working_channels,
                             d_carry_frames);
      d_carry_pos += d_carry_frames;
      d_carry_frames = 0;
    }
    // Reconstruct a batch of device-rate frames (working layout) then remix to the
    // device layout, bounded by one block-worth of `d_src_out` scratch.
    std::uint32_t produced = 0;
    const std::uint32_t want = frames - written;
    while (produced < want && produced < d_config.block_frames && d_resampler.can_produce()) {
      d_resampler.produce(d_src_out.data() +
                          static_cast<std::size_t>(produced) * d_working_channels);
      ++produced;
    }
    convert_frames(d_src_out.data(), out + static_cast<std::size_t>(written) * d_device_channels,
                   produced);
    written += produced;
  }
  // The device frame count IS the clock: publish the delivered total for the owner
  // thread to master from (release-paired with the owner's acquire load).
  d_delivered.fetch_add(frames, std::memory_order_release);
}

void DeviceMonitor::master_step() {
  std::optional<Time> seek;
  std::optional<Rational> rate;
  {
    std::lock_guard<std::mutex> lock(d_master_mutex);
    seek.swap(d_pending_seek);
    rate.swap(d_pending_rate);
  }

  bool rebased = false;
  if (rate.has_value()) {
    d_transport.set_rate(*rate);
    d_direction.store(rate_sign(*rate), std::memory_order_release);
    rebased = true;
  }
  if (seek.has_value()) {
    d_transport.seek(*seek);
    // Rebase the sample origin so frames delivered before the seek do not re-advance.
    d_last_delivered = d_delivered.load(std::memory_order_acquire);
    rebased = true;
  }

  const std::uint64_t delivered = d_delivered.load(std::memory_order_acquire);
  const std::uint64_t delta = delivered - d_last_delivered;
  d_last_delivered = delivered;
  if (delta > 0) {
    // Exact sample-derived advance: `delta` frames is exactly `delta * flicks_per_frame`
    // flicks of real elapsed time at the device rate (no remainder -- flicks divide
    // every common rate). `Transport::advance` scales by the rate and wraps loops with
    // its own exact ties-to-even rounding, all-or-nothing on overflow.
    (void)d_transport.advance(Time{static_cast<std::int64_t>(delta) * d_flicks_per_frame});
  }

  // Publish the mastered playhead (release): the pump and viewports chase this.
  d_published.store(d_transport.position(), std::memory_order_release);

  // Live camera follow (audio.spatial_camera_follow, doc 12 "Camera follow",
  // D1/D2/D4): sample the interactive viewport camera on THIS non-RT owner thread and,
  // on a change, stage a listener re-seed. Only when the ring seed is Spatial
  // (`d_spatial_base` present) and a `camera_source` is wired; a Flat scene or an
  // absent source never follows, so every existing device-path drain is byte-exact.
  bool camera_reseed = false;
  if (d_config.camera_source && d_spatial_base.has_value()) {
    const Affine cam = d_config.camera_source();
    // Exact `Affine`-coefficient compare (D4): a still camera is a no-op — no re-seed,
    // no reprime — so a static scene never re-renders; the byte-exact digest keys on
    // exact `Affine` bits, so an epsilon compare would be wrong.
    if (!d_last_camera.has_value() || *d_last_camera != cam) {
      d_last_camera = cam;
      // Swap only the listener + its uniform scale-attenuation; the viewport extent and
      // sub-audible threshold stay from the static seed (D2). `spatial_edge_atten(cam)`
      // is exactly `clamp(max_scale(cam), 0, 1)` — the camera's root attenuation the
      // static monitor seed already computes (lookahead.hpp:78), so the live drain is
      // byte-identical to a static-seed run under this camera.
      Spatialization ctx = *d_spatial_base;
      ctx.listener = cam;
      ctx.accum_atten = spatial_edge_atten(cam);
      // Stage the seed BEFORE `notify_transport_change` (D5): the pump applies it to
      // the ring before it reprimes, so the fill re-warms under the new listener.
      d_pump.set_spatial(ctx);
      d_listener_reseeds.fetch_add(1, std::memory_order_release);
      camera_reseed = true;
    }
  }

  // A seek/rate rebase re-seats the drain cursor AND reprimes; a pure camera change
  // reprimes WITHOUT re-seating the cursor (the playhead is unmoved, D3), so a camera
  // reprime is distinguishable from a seek reprime (`drain_realigns()` does not
  // advance); a plain advance only nudges the pump to prime further ahead.
  if (rebased) {
    // Re-seat the RT drain cursor to the block covering the freshly-published
    // playhead so the device drain tracks the reprimed ring window from the first
    // post-change frame (audio.seek_drain_realign, D1/D3). `start_block_index()`
    // reads the transport, which the owner alone owns (Constraint 2/D3), so it is
    // evaluated HERE and only the resulting integer crosses to the RT thread. Publish
    // the index (release) before the request flag (release) so the RT callback's
    // acquire-`exchange` of the flag sees the matching index.
    d_realign_index.store(start_block_index(), std::memory_order_release);
    d_realign_request.store(true, std::memory_order_release);
    if (d_resampling) {
      // Ask the RT callback to flush the resampler's filter memory before it next
      // fills, so post-seek/-rate output carries no pre-change tail (D4). A control
      // flag only: the DSP buffers stay RT-single-owner. Co-triggered with the
      // realign above but a distinct concern (block cursor vs. filter memory).
      d_resampler_flush.store(true, std::memory_order_release);
    }
  }
  if (rebased || camera_reseed) {
    d_pump.notify_transport_change();
  } else {
    d_pump.poke();
  }
}

void DeviceMonitor::run_master() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(d_master_mutex);
      d_master_cv.wait_for(lock, d_config.master_period,
                           [this] { return d_master_stop || d_master_poke; });
      if (d_master_stop) {
        break;
      }
      d_master_poke = false;
    }
    master_step();
    {
      // Publish the step under the mutex so a `flush_master()` cannot miss it between
      // its predicate check and its wait (mirrors HousekeepingThread / the pump).
      std::lock_guard<std::mutex> lock(d_master_mutex);
      d_master_steps.fetch_add(1, std::memory_order_release);
    }
    d_master_progress_cv.notify_all();
  }
}

void DeviceMonitor::seek(Time t) {
  {
    std::lock_guard<std::mutex> lock(d_master_mutex);
    d_pending_seek = t;
    d_master_poke = true;
  }
  d_master_cv.notify_one();
}

void DeviceMonitor::set_rate(Rational rate) {
  {
    std::lock_guard<std::mutex> lock(d_master_mutex);
    d_pending_rate = rate;
    d_master_poke = true;
  }
  d_master_cv.notify_one();
}

std::uint64_t DeviceMonitor::flush_master() {
  std::unique_lock<std::mutex> lock(d_master_mutex);
  const std::uint64_t target = d_master_steps.load(std::memory_order_relaxed) + 1;
  d_master_poke = true;
  d_master_cv.notify_one();
  d_master_progress_cv.wait(lock, [this, target] {
    return d_master_stop || d_master_steps.load(std::memory_order_acquire) >= target;
  });
  return d_master_steps.load(std::memory_order_relaxed);
}

} // namespace arbc
