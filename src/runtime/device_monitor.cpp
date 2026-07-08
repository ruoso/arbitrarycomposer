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
  if (d_device.sample_rate != config.working_rate) {
    // Device-edge sample-rate conversion (reusing the shipped windowed-sinc
    // resampler) is deferred; v1 masters and drains at a matched rate so the drained
    // bytes stay byte-identical to the working-rate mix oracle (Constraint 9).
    throw std::invalid_argument(
        "DeviceMonitor: device sample rate must equal the working rate (edge SRC is out of scope)");
  }

  d_flicks_per_frame = Time::flicks_per_second / static_cast<std::int64_t>(d_device.sample_rate);
  d_scratch.assign(static_cast<std::size_t>(config.block_frames) * d_working_channels, 0.0F);
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
  // aligned (both derive from the same position).
  const std::int64_t span =
      static_cast<std::int64_t>(d_config.block_frames) * d_flicks_per_frame;
  if (span == 0) {
    return 0;
  }
  const std::int64_t p = d_transport.position().flicks;
  // Floored, sign-correct (mirrors LookaheadRing::block_index_at).
  return p >= 0 ? p / span : -((-p + span - 1) / span);
}

void DeviceMonitor::convert_frames(const float* src, float* dst, std::uint32_t frames) const {
  if (d_config.working_layout == d_device.layout) {
    std::memcpy(dst, src,
                static_cast<std::size_t>(frames) * d_working_channels * sizeof(float));
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

void DeviceMonitor::fill_rt(float* out, std::uint32_t frames) {
  // RT-safe (Constraint 1): only `pump.drain` (never a mix), a pure layout
  // conversion, and one atomic increment. No allocation -- `d_scratch` is
  // pre-sized; `out` is the caller's device buffer.
  std::uint32_t written = 0;
  while (written < frames) {
    if (d_carry_frames == 0) {
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
    const std::uint32_t n = frames - written < d_carry_frames ? frames - written : d_carry_frames;
    convert_frames(d_scratch.data() + static_cast<std::size_t>(d_carry_pos) * d_working_channels,
                   out + static_cast<std::size_t>(written) * d_device_channels, n);
    written += n;
    d_carry_pos += n;
    d_carry_frames -= n;
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

  // A transport change flushes + reprimes; a plain advance only nudges the pump to
  // prime further ahead of the freshly-published playhead.
  if (rebased) {
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
