#pragma once

#include <arbc/base/expected.hpp>       // expected
#include <arbc/runtime/device_sink.hpp> // DeviceSink, DeviceFormat, DeviceFillCallback

#include <memory>
#include <string>

namespace arbc::device_miniaudio {

// `org.arbc.device.miniaudio` -- the out-of-lib reference `DeviceSink` backed by
// the private miniaudio-class single-header backend (`third_party/maudio.h`). It
// is the device analog of `arbc-plugin-imageseq`: the concrete OS-audio backend
// that keeps its dependency off `libarbc`'s link line (doc 17 "the codec line",
// generalized to device backends; device_monitor.md D1/D4). The `DeviceMonitor`
// (in `arbc::runtime`) drives it; this plugin owns nothing of the clock/mastering
// policy -- it only opens a device stream and invokes the monitor-supplied RT fill
// callback (device_sink.hpp:29-37).
//
// v1 wires this sink by direct construction (the host/test constructs it and hands
// it to a `DeviceMonitor`); general device-sink discovery/registration rides the
// deferred plugin loader `runtime.plugin_loading` (M8, device_monitor.md D5).
class MiniaudioSink final : public DeviceSink {
public:
  static constexpr const char* backend_id = "org.arbc.device.miniaudio";

  // Runs a device stream at `format` (interleaved Float32). Constructs without
  // touching hardware; `start` opens the stream. Requires a non-degenerate format.
  explicit MiniaudioSink(DeviceFormat format);
  ~MiniaudioSink() override;

  DeviceFormat format() const override;
  void start(DeviceFillCallback fill) override;
  void stop() override;

  // True iff the host exposes at least one playback device (false in headless CI;
  // the hardware-gated smoke test opens a stream only when this holds).
  static bool device_available();

private:
  struct Impl;
  std::unique_ptr<Impl> d_impl;
};

// The factory the host/test constructs (the device analog of
// `make_imageseq_content`): a fresh sink at `format`, or an error value on a
// degenerate format. Errors are values -- never thrown across the boundary
// (doc 03:177-180).
expected<std::unique_ptr<DeviceSink>, std::string> make_miniaudio_sink(DeviceFormat format);

} // namespace arbc::device_miniaudio
