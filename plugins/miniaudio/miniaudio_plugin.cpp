#include <arbc/contract/plugin.hpp> // ARBC_PLUGIN_EXPORT
#include <arbc/device_miniaudio/miniaudio_sink.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/runtime/device_sink.hpp>

// The shipped loadable artifact (a MODULE): the device analog of imageseq's
// `arbc_plugin_register`. v1 has NO device-sink registry (the `Registry` is
// content-kind-only; general device discovery is runtime.plugin_loading, M8,
// device_monitor.md D5), so the reference device backend exposes a minimal
// extern "C" factory the future loader will resolve. No exceptions cross the
// boundary (doc 03:177-180): a failed open is the NULL value.
extern "C" ARBC_PLUGIN_EXPORT arbc::DeviceSink* arbc_device_sink_create(unsigned int sample_rate,
                                                                        int channels) {
  const arbc::ChannelLayout layout =
      channels == 1 ? arbc::ChannelLayout::Mono : arbc::ChannelLayout::Stereo;
  auto sink = arbc::device_miniaudio::make_miniaudio_sink(arbc::DeviceFormat{sample_rate, layout});
  return sink.has_value() ? sink.value().release() : nullptr;
}

extern "C" ARBC_PLUGIN_EXPORT void arbc_device_sink_destroy(arbc::DeviceSink* sink) {
  delete sink;
}
