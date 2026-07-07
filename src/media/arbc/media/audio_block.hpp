#pragma once

#include <cstdint>

namespace arbc {

// The audio channel layout (doc 12, doc 17:50): the sample-block analog of a
// `PixelFormat`, naming how many interleaved channels a block carries and in
// what order. Minimal by design -- doc 12's working default is stereo, and the
// two-member set types the v1 facet honestly; 5.1/ambisonic layouts are an
// engine concern (`audio.audio_types`) and extend this enum losslessly when a
// caller needs them. `arbc::media` (L1) is the home for channel layouts and
// typed sample-span views (doc 17:50), mirroring pixel formats living here.
enum class ChannelLayout {
  Mono,
  Stereo,
};

// Interleaved channel count for a layout: 1 for Mono, 2 for Stereo. `constexpr`
// so `AudioBlock` addressing and `AudioRequest` sizing stay compile-time.
constexpr std::uint32_t channel_count(ChannelLayout layout) {
  switch (layout) {
  case ChannelLayout::Mono:
    return 1;
  case ChannelLayout::Stereo:
    return 2;
  }
  return 0;
}

// A non-owning, mutable interleaved float32 sample-block view: the audio analog
// of the media typed pixel-span views and of `Surface&` as a caller-owned
// render target (doc 12:54, "zero-initialized"). It carries a bare pointer plus
// its shape (`frames`, `layout`, `rate`) and owns nothing -- the caller (the
// mix engine, an offline export) owns the storage. Samples are interleaved: the
// value for frame `f`, channel `c` lives at `samples[f * channel_count(layout) +
// c]`, so a stereo block of `frames` frames spans `2 * frames` floats. Pooled /
// owned block storage and the working-format configuration are engine concerns
// (`audio.audio_types`, `arbc::cache`); this is the minimal vocabulary the
// audio facet signature names. Trivially copyable, so it stays a cheap
// by-value view.
struct AudioBlock {
  float* samples{nullptr};                     // interleaved float32, caller-owned
  std::uint32_t frames{0};                     // number of sample frames in the block
  ChannelLayout layout{ChannelLayout::Stereo}; // interleaving of `samples`
  std::uint32_t rate{0};                       // sample rate in Hz
};

} // namespace arbc
