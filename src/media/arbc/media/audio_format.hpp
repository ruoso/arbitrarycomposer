#pragma once

#include <arbc/media/audio_block.hpp> // ChannelLayout

#include <cstdint>

namespace arbc {

// The per-composition working audio format (doc 12): the audio analog of doc
// 07's working color space, carried as one value so audio signatures don't
// accrete a loose `(rate, layout)` pair. It names the working **sample rate**
// and **channel layout** the mix engine pulls, sizes, and converts toward at
// composition edges and nesting boundaries (doc 12:95-105). Samples are always
// float32 (doc 12:98) -- there is no format/variant axis, so unlike
// `SurfaceFormat` there is no audio analog of `PixelFormat` here. Equality is
// member-wise so a nesting boundary can compare two working formats at once.
struct AudioFormat {
  std::uint32_t sample_rate = 48000;              // working sample rate in Hz
  ChannelLayout layout = ChannelLayout::Stereo;   // working channel interleaving

  friend constexpr bool operator==(const AudioFormat&, const AudioFormat&) = default;
};

// The doc 12 default working audio format: 48 kHz stereo, float32. Unlike the
// pixel side's staged 16f default, this is fully functional the moment it
// exists -- float32 is the sole audio format, so there is no describable-before-
// storable gap. Mirrors the `k_working_rgba32f` naming convention.
inline constexpr AudioFormat k_working_audio{48000, ChannelLayout::Stereo};

} // namespace arbc
