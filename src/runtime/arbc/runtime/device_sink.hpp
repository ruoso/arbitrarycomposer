#pragma once

#include <arbc/arbc_api.h>
#include <arbc/media/audio_block.hpp> // ChannelLayout, channel_count

#include <cstdint>
#include <functional>

namespace arbc {

// The interactive audio device stream format (doc 12:100-104): interleaved
// Float32 at a device sample rate and channel layout -- the edge the monitor
// converts the working format toward. `arbc::media` owns the channel vocabulary
// (doc 17:50); this is the audio analog of a `SurfaceFormat` at the display edge.
struct DeviceFormat {
  std::uint32_t sample_rate{0};
  ChannelLayout layout{ChannelLayout::Stereo};

  friend constexpr bool operator==(const DeviceFormat&, const DeviceFormat&) = default;
};

// The monitor-supplied RT fill callback: fill exactly `frames` interleaved Float32
// frames at the device format into `out` (`channel_count(format.layout) * frames`
// floats). Invoked on the device's RT thread. The `DeviceMonitor`'s implementation
// of it is RT-safe (no plugin code, no allocation): it consumes prepared mixed
// blocks through `LookaheadPump::drain` and bumps one atomic frame counter
// (doc 12:31-34,155-164; Constraint 1).
using DeviceFillCallback = std::function<void(float* out, std::uint32_t frames)>;

// The pure-virtual device sink seam for `arbc::runtime` (L5, OS-audio-free): a
// concrete backend implements it to own/open a device stream at a device format
// and, on the device's RT thread, invoke the monitor-supplied `DeviceFillCallback`
// to obtain interleaved Float32 frames. It names NO OS audio API -- it is the
// audio analog of a codec's in-lib decode interface (doc 17 "the codec line", this
// task's delta): the real OS backend lives behind it as a separate out-of-lib
// plugin artifact (`arbc-plugin-<device>` under `plugins/`), never on `libarbc`'s
// link line. Owning a thread/clock/device is runtime policy (doc 17:84-86), so the
// interface and its `DeviceMonitor` driver live in `runtime`, not the engines.
class ARBC_API DeviceSink {
public:
  DeviceSink(const DeviceSink&) = delete;
  DeviceSink& operator=(const DeviceSink&) = delete;
  // Declared out-of-line (defined in device_sink.cpp) so it is the class's KEY
  // FUNCTION: its vtable and typeinfo are then emitted in exactly one translation
  // unit -- libarbc -- rather than as a weak COMDAT copy in every plugin that
  // derives from DeviceSink. That single-definition-site is what lets a device
  // backend plugin resolve DeviceSink's vtable/RTTI FROM the shared libarbc across
  // the dlopen/LoadLibrary boundary (packaging.shared_library_build_msvc; the same
  // key-function discipline arbc::Content already uses, contract/content.cpp). An
  // inline `= default` here would keep the vtable keyless and force the private
  // per-plugin copy the single-instance proof exists to forbid.
  virtual ~DeviceSink();

  // The device format the sink runs its stream at (queried before `start`).
  virtual DeviceFormat format() const = 0;

  // Open the stream and begin invoking `fill` on the device RT thread.
  virtual void start(DeviceFillCallback fill) = 0;

  // Stop the stream; after it returns, `fill` is guaranteed not to be invoked
  // (the RT thread is quiesced), so the monitor can tear down safely.
  virtual void stop() = 0;

protected:
  DeviceSink() = default;
};

} // namespace arbc
