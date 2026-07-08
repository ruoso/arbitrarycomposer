// The reference device backend's build + hardware-gated smoke check
// (device_monitor.md §acceptance "Reference-backend artifact"). It constructs the
// out-of-lib `MiniaudioSink` and, WHEN A PLAYBACK DEVICE IS PRESENT, opens/closes a
// stream and confirms the backend drove the monitor-supplied fill callback. In
// headless CI there is no device, so the open/close path is skipped -- the
// deterministic behavioral coverage is the fake sink
// (src/runtime/t/device_monitor.t.cpp + tests/device_monitor_concurrency.t.cpp),
// not this backend. Links the plugin's impl archive directly (like imageseq's
// in-process tests); the backend dependency stays private to the plugin.

#include <arbc/device_miniaudio/miniaudio_sink.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/runtime/device_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>

using namespace arbc;
using arbc::device_miniaudio::make_miniaudio_sink;
using arbc::device_miniaudio::MiniaudioSink;

TEST_CASE("the miniaudio reference sink constructs and reports its device format") {
  const DeviceFormat fmt{48'000, ChannelLayout::Stereo};
  auto made = make_miniaudio_sink(fmt);
  REQUIRE(made.has_value());
  REQUIRE(made.value()->format() == fmt);

  // A degenerate format is an error value, never a throw across the boundary.
  auto bad = make_miniaudio_sink(DeviceFormat{0, ChannelLayout::Stereo});
  REQUIRE_FALSE(bad.has_value());
}

TEST_CASE("the miniaudio reference sink opens/closes a stream when a device is present") {
  if (!MiniaudioSink::device_available()) {
    // Headless CI: no playback device -> the backend open path is skipped. This is
    // the hardware gate; the fake sink carries the deterministic coverage.
    SUCCEED("no playback device present (headless) -- device smoke check skipped");
    return;
  }
  MiniaudioSink sink{DeviceFormat{48'000, ChannelLayout::Stereo}};
  std::atomic<std::uint64_t> frames{0};
  sink.start([&frames](float*, std::uint32_t n) { frames.fetch_add(n, std::memory_order_relaxed); });
  sink.stop();
  REQUIRE(frames.load(std::memory_order_relaxed) > 0); // the backend drove the fill callback
}
