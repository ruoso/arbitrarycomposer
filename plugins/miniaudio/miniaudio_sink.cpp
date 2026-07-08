#include <arbc/device_miniaudio/miniaudio_sink.hpp>
#include <arbc/media/audio_block.hpp> // channel_count

#include <maudio.h> // private backend dep (PRIVATE include dir; never in libarbc)

#include <stdexcept>
#include <utility>

namespace arbc::device_miniaudio {

// The device-thread trampoline: the C backend hands us `out`/`frames`; we invoke
// the monitor-supplied RT fill callback (which drains prepared mixed blocks and
// bumps the delivered-frame counter -- RT-safe, no plugin code here).
struct MiniaudioSink::Impl {
  DeviceFormat format;
  DeviceFillCallback fill;
  maudio_device* device{nullptr};

  static void on_data(void* user, float* out, unsigned int frames) {
    auto* self = static_cast<Impl*>(user);
    if (self->fill) {
      self->fill(out, frames);
    }
  }
};

MiniaudioSink::MiniaudioSink(DeviceFormat format) : d_impl(std::make_unique<Impl>()) {
  if (format.sample_rate == 0) {
    throw std::invalid_argument("MiniaudioSink: device sample_rate must be non-zero");
  }
  d_impl->format = format;
}

MiniaudioSink::~MiniaudioSink() { stop(); }

DeviceFormat MiniaudioSink::format() const { return d_impl->format; }

bool MiniaudioSink::device_available() { return maudio_device_count() > 0; }

void MiniaudioSink::start(DeviceFillCallback fill) {
  d_impl->fill = std::move(fill);
  maudio_config config{};
  config.sample_rate = d_impl->format.sample_rate;
  config.channels = channel_count(d_impl->format.layout);
  config.callback = &Impl::on_data;
  config.user = d_impl.get();
  d_impl->device = maudio_device_open(&config);
  if (d_impl->device == nullptr) {
    // No device present (headless) or a degenerate config: the reference backend
    // cannot open a stream. The deterministic behavioral coverage is the fake
    // sink; this path is reached only by the hardware-gated smoke test.
    throw std::runtime_error("MiniaudioSink: no playback device available");
  }
  maudio_device_start(d_impl->device);
}

void MiniaudioSink::stop() {
  if (d_impl && d_impl->device != nullptr) {
    maudio_device_stop(d_impl->device);
    maudio_device_close(d_impl->device);
    d_impl->device = nullptr;
  }
}

expected<std::unique_ptr<DeviceSink>, std::string> make_miniaudio_sink(DeviceFormat format) {
  if (format.sample_rate == 0) {
    return unexpected<std::string>("MiniaudioSink: device sample_rate must be non-zero");
  }
  return std::unique_ptr<DeviceSink>(std::make_unique<MiniaudioSink>(format));
}

} // namespace arbc::device_miniaudio
