#pragma once

#include <arbc/media/audio_block.hpp>

namespace arbc {

// Deterministic windowed-sinc polyphase resampler (doc 12:24-25,100-104; doc
// 16:48-53), the audio analog of the bilinear/box raster resamplers in
// `backend_cpu`. Reconstructs `out.frames` interleaved float32 samples at
// `out.rate` from the native-rate samples in `in` (`in.rate`), for the
// UPSAMPLING case `in.rate < out.rate` at the nesting boundary: a below-rate
// child's genuine native block is band-limited-reconstructed to the composed
// request rate before the additive mix. This is decisively NOT a nearest/hold --
// it is a fixed 16-tap Blackman-Harris windowed sinc over a 32-phase polyphase
// bank, whose coefficients are a checked-in table (`audio_resampler.cpp`) so
// runtime does only ordered, no-libm float32 multiply-accumulates. The output is
// therefore byte-exact with NO tolerance and portable across toolchains (no
// runtime `std::sin`), mirroring tone's parabolic-sine discipline
// (`kinds.tone`).
//
// Output positions map through the EXACT rate ratio and are rounded ONCE to the
// polyphase phase index (doc 11:216-234, "one rounding at the leaf, never
// accumulate"): output frame `n` samples native position `n * in.rate / out.rate`
// (native-frame coordinates, `in` frame 0 == `out` frame 0's instant), computed
// in integer arithmetic. Filter taps that fall outside `in` read as zero -- a
// defined edge convention. `in` and `out` must share a channel layout; each
// channel is filtered independently (layout remix is the caller's separate step).
//
// A pure function of `(in samples, in.rate, out.rate)` with no shared state, safe
// on any worker (Constraint 8). Requires `0 < in.rate < out.rate` and matching
// layouts; on any other shape it leaves `out` unchanged (the caller keeps the 1:1
// path for the rate-honoring case, which never needs reconstruction).
void resample_audio(const AudioBlock& in, AudioBlock& out);

} // namespace arbc
