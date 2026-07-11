#include <arbc/media/audio_resampler.hpp>
#include <arbc/media/resampler_prototype.hpp>
#include <arbc/media/streaming_resampler.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace arbc {

namespace {

// ===========================================================================
// FROZEN POLYPHASE COEFFICIENT TABLE -- regenerate deliberately (doc 16:50-53).
// ===========================================================================
//
// A fixed 16-tap (half N=8) Blackman-Harris windowed sinc over a 32-phase
// polyphase bank, per-phase normalized so each phase's taps sum to 1 (DC gain
// unity) and phase 0 is an exact Kronecker delta (integer-argument sinc forced
// to exact zero) -- so an aligned tap (an integer-ratio phase-0 sample)
// reproduces its native input byte-for-byte and a constant signal reconstructs
// to itself in the block interior. The coefficients carry no runtime libm: they
// are tabulated once, offline, and only ordered no-FMA float32 MACs run at
// render time (doc 16 CPU-determinism recipe; the audio twin of tone's
// parabolic-sine portability, `kinds.tone`).
//
// Window/tap/phase choices are fixed (Constraint 7): no runtime quality knob.
//
// REGENERATE PROCEDURE (doc 16 tier-3): an intended filter change (different
// window, tap count, or phase count) deliberately re-freezes this table; it
// never regenerates silently. Build the media unit test and run only the hidden
// dump case, which prints this paste-ready block:
//
//     cmake --build --preset dev --target arbc_media_t
//     ./build/dev/src/media/arbc_media_t "[.regen]"
//
// then replace the block below with its output AND re-freeze the dependent
// output goldens (src/media/t/audio_resampler.t.cpp and
// tests/nested_audio_resampling_goldens.t.cpp).
//
// The dump calls `resampler_prototype::generate(k_resampler_phases, k_resampler_taps,
// 1.0, true)` -- the SAME generator that builds the widened decimation bank, differing
// only in cutoff and the Kronecker rule. A guard test asserts the generator still
// reproduces this table byte-for-byte, so the two can never drift apart silently.

// PHASES=32 TAPS=16 (half N=8), Blackman-Harris windowed sinc, per-phase normalized
constexpr std::size_t k_resampler_phases = 32;
constexpr std::size_t k_resampler_taps = 16;
constexpr std::array<float, 512> k_resampler_coeffs = {
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x1.0000000000000p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f,
    0x0.0p+0f, // phase 0
    -0x1.a4fd660000000p-17f,
    0x1.c0c0540000000p-14f,
    -0x1.03c39e0000000p-11f,
    0x1.ae049a0000000p-10f,
    -0x1.201c720000000p-8f,
    0x1.5a25900000000p-7f,
    -0x1.c298920000000p-6f,
    0x1.ff22240000000p-1f,
    0x1.e517d60000000p-6f,
    -0x1.6d73e40000000p-7f,
    0x1.30f3f00000000p-8f,
    -0x1.cbb70c0000000p-10f,
    0x1.1a41880000000p-11f,
    -0x1.f489720000000p-14f,
    0x1.f072920000000p-17f,
    -0x1.0532bc0000000p-22f, // phase 1
    -0x1.80e2aa0000000p-16f,
    0x1.a683620000000p-13f,
    -0x1.efc3400000000p-11f,
    0x1.9dc47e0000000p-9f,
    -0x1.16a5d80000000p-7f,
    0x1.4f48080000000p-6f,
    -0x1.b0ae140000000p-5f,
    0x1.fc89f60000000p-1f,
    0x1.f5875c0000000p-5f,
    -0x1.75bafc0000000p-6f,
    0x1.382dde0000000p-7f,
    -0x1.d8e4b00000000p-9f,
    0x1.24ab160000000p-10f,
    -0x1.06d55e0000000p-12f,
    0x1.0ba84a0000000p-15f,
    -0x1.1fdae20000000p-21f, // phase 2
    -0x1.06999c0000000p-15f,
    0x1.2939e40000000p-12f,
    -0x1.618d240000000p-10f,
    0x1.29922e0000000p-8f,
    -0x1.92e3c80000000p-7f,
    0x1.e594140000000p-6f,
    -0x1.36d01c0000000p-4f,
    0x1.f83bc60000000p-1f,
    0x1.83feda0000000p-4f,
    -0x1.1dbd640000000p-5f,
    0x1.ddc4420000000p-7f,
    -0x1.6b96020000000p-8f,
    0x1.c596200000000p-10f,
    -0x1.9c7e220000000p-12f,
    0x1.aeebd80000000p-15f,
    -0x1.f1277c0000000p-21f, // phase 3
    -0x1.3ce5640000000p-15f,
    0x1.724a760000000p-12f,
    -0x1.bea0a40000000p-10f,
    0x1.7b21ca0000000p-8f,
    -0x1.0206900000000p-6f,
    0x1.378b460000000p-5f,
    -0x1.8be1ac0000000p-4f,
    0x1.f23ebc0000000p-1f,
    0x1.0a35bc0000000p-3f,
    -0x1.8325540000000p-5f,
    0x1.43e0f40000000p-6f,
    -0x1.ef3fee0000000p-8f,
    0x1.3752900000000p-9f,
    -0x1.1ea2aa0000000p-11f,
    0x1.32ee7a0000000p-14f,
    -0x1.877bae0000000p-20f, // phase 4
    -0x1.64a1a60000000p-15f,
    0x1.aecd240000000p-12f,
    -0x1.07823a0000000p-9f,
    0x1.c343120000000p-8f,
    -0x1.34c78a0000000p-6f,
    0x1.758d3a0000000p-5f,
    -0x1.d76edc0000000p-4f,
    0x1.ea9cc60000000p-1f,
    0x1.55b6440000000p-3f,
    -0x1.ea2bf80000000p-5f,
    0x1.9a450a0000000p-6f,
    -0x1.3b1bc40000000p-7f,
    0x1.8f33d20000000p-9f,
    -0x1.740a3e0000000p-11f,
    0x1.9809da0000000p-14f,
    -0x1.240b1e0000000p-19f, // phase 5
    -0x1.7f31d40000000p-15f,
    0x1.df3eb60000000p-12f,
    -0x1.2964fe0000000p-9f,
    0x1.00e4080000000p-7f,
    -0x1.617f340000000p-6f,
    0x1.ac8afa0000000p-5f,
    -0x1.0cb6aa0000000p-3f,
    0x1.e162880000000p-1f,
    0x1.a42a780000000p-3f,
    -0x1.28ebae0000000p-4f,
    0x1.f130b40000000p-6f,
    -0x1.7f93200000000p-7f,
    0x1.e99d380000000p-9f,
    -0x1.cdca160000000p-11f,
    0x1.0330400000000p-13f,
    -0x1.a2598e0000000p-19f, // phase 6
    -0x1.8e07340000000p-15f,
    0x1.021cfe0000000p-11f,
    -0x1.45143c0000000p-9f,
    0x1.1b50b20000000p-7f,
    -0x1.880b4a0000000p-6f,
    0x1.dc54d20000000p-5f,
    -0x1.28f26c0000000p-3f,
    0x1.d69f380000000p-1f,
    0x1.f534420000000p-3f,
    -0x1.5c8f460000000p-4f,
    0x1.23dc460000000p-5f,
    -0x1.c44ce00000000p-7f,
    0x1.22d2460000000p-8f,
    -0x1.158ab20000000p-10f,
    0x1.3ea95a0000000p-13f,
    -0x1.218ac00000000p-18f, // phase 7
    -0x1.9298f40000000p-15f,
    0x1.0f396c0000000p-11f,
    -0x1.5abcb60000000p-9f,
    0x1.30ee4e0000000p-7f,
    -0x1.a85c360000000p-6f,
    0x1.0267de0000000p-4f,
    -0x1.40775e0000000p-3f,
    0x1.ca648a0000000p-1f,
    0x1.2437480000000p-2f,
    -0x1.8f75ac0000000p-4f,
    0x1.4e72b00000000p-5f,
    -0x1.0441ae0000000p-6f,
    0x1.5124200000000p-8f,
    -0x1.457f1a0000000p-10f,
    0x1.7e024c0000000p-13f,
    -0x1.84e2b60000000p-18f, // phase 8
    -0x1.8e5cea0000000p-15f,
    0x1.1758aa0000000p-11f,
    -0x1.6a9a840000000p-9f,
    0x1.41d11c0000000p-7f,
    -0x1.c274420000000p-6f,
    0x1.12fa540000000p-4f,
    -0x1.535a960000000p-3f,
    0x1.bcc6820000000p-1f,
    0x1.4eb7040000000p-2f,
    -0x1.c10e360000000p-4f,
    0x1.77db1e0000000p-5f,
    -0x1.25b3000000000p-6f,
    0x1.7f387e0000000p-8f,
    -0x1.763ada0000000p-10f,
    0x1.c0ad540000000p-13f,
    -0x1.fc70440000000p-18f, // phase 9
    -0x1.82c13c0000000p-15f,
    0x1.1ae6780000000p-11f,
    -0x1.74f71e0000000p-9f,
    0x1.4e19c40000000p-7f,
    -0x1.d666a40000000p-6f,
    0x1.1fe7d00000000p-4f,
    -0x1.61b92e0000000p-3f,
    0x1.addb4a0000000p-1f,
    0x1.79e0da0000000p-2f,
    -0x1.f0c3a80000000p-4f,
    0x1.9f916a0000000p-5f,
    -0x1.460d8c0000000p-6f,
    0x1.ac79d20000000p-8f,
    -0x1.a7269a0000000p-10f,
    0x1.02fdc60000000p-12f,
    -0x1.4469ba0000000p-17f, // phase 10
    -0x1.7126be0000000p-15f,
    0x1.1a53fe0000000p-11f,
    -0x1.7a27700000000p-9f,
    0x1.55f4300000000p-7f,
    -0x1.e4565c0000000p-6f,
    0x1.293f640000000p-4f,
    -0x1.6bb7ca0000000p-3f,
    0x1.9dbb100000000p-1f,
    0x1.a579e40000000p-2f,
    -0x1.0efeba0000000p-3f,
    0x1.c50ef80000000p-5f,
    -0x1.64e0f20000000p-6f,
    0x1.d849e00000000p-8f,
    -0x1.d79bc00000000p-10f,
    0x1.268df80000000p-12f,
    -0x1.94feea0000000p-17f, // phase 11
    -0x1.5adc3c0000000p-15f,
    0x1.1615b80000000p-11f,
    -0x1.7a89ea0000000p-9f,
    0x1.5996500000000p-7f,
    -0x1.ec74fa0000000p-6f,
    0x1.2f188e0000000p-4f,
    -0x1.71820a0000000p-3f,
    0x1.8c7fc60000000p-1f,
    0x1.d145440000000p-2f,
    -0x1.2410740000000p-3f,
    0x1.e7cc120000000p-5f,
    -0x1.81ba640000000p-6f,
    0x1.0101920000000p-7f,
    -0x1.0372be0000000p-9f,
    0x1.4a8d7c0000000p-12f,
    -0x1.ef907c0000000p-17f, // phase 12
    -0x1.411a740000000p-15f,
    0x1.0ea1680000000p-11f,
    -0x1.76848e0000000p-9f,
    0x1.593ede0000000p-7f,
    -0x1.ef01560000000p-6f,
    0x1.3192a20000000p-4f,
    -0x1.734a080000000p-3f,
    0x1.7a44f60000000p-1f,
    0x1.fd04880000000p-2f,
    -0x1.3749420000000p-3f,
    0x1.03a0a80000000p-4f,
    -0x1.9c25e40000000p-6f,
    0x1.147d340000000p-7f,
    -0x1.1a21020000000p-9f,
    0x1.6e71580000000p-12f,
    -0x1.29a7a20000000p-16f, // phase 13
    -0x1.2500f80000000p-15f,
    0x1.046c4e0000000p-11f,
    -0x1.6e83140000000p-9f,
    0x1.55340a0000000p-7f,
    -0x1.ec461c0000000p-6f,
    0x1.30d4140000000p-4f,
    -0x1.7147ac0000000p-3f,
    0x1.6727840000000p-1f,
    0x1.143c120000000p-1f,
    -0x1.485ba40000000p-3f,
    0x1.1174900000000p-4f,
    -0x1.b3af840000000p-6f,
    0x1.2640460000000p-7f,
    -0x1.2f72200000000p-9f,
    0x1.919db20000000p-12f,
    -0x1.5f7ea60000000p-16f, // phase 14
    -0x1.0793ba0000000p-15f,
    0x1.efd2ec0000000p-12f,
    -0x1.62f50e0000000p-9f,
    0x1.4dc2360000000p-7f,
    -0x1.e498660000000p-6f,
    0x1.2d09c20000000p-4f,
    -0x1.6bb8080000000p-3f,
    0x1.5345780000000p-1f,
    0x1.29aff40000000p-1f,
    -0x1.56fb160000000p-3f,
    0x1.1d209e0000000p-4f,
    -0x1.c7e4ce0000000p-6f,
    0x1.35f2400000000p-7f,
    -0x1.42fad20000000p-9f,
    0x1.b366800000000p-12f,
    -0x1.9874240000000p-16f, // phase 15
    -0x1.d372b00000000p-16f,
    0x1.d310900000000p-12f,
    -0x1.544c380000000p-9f,
    0x1.433aa60000000p-7f,
    -0x1.d8562c0000000p-6f,
    0x1.2666260000000p-4f,
    -0x1.62dcb60000000p-3f,
    0x1.3ebdc00000000p-1f,
    0x1.3ebdc00000000p-1f,
    -0x1.62dcb60000000p-3f,
    0x1.2666260000000p-4f,
    -0x1.d8562c0000000p-6f,
    0x1.433aa60000000p-7f,
    -0x1.544c380000000p-9f,
    0x1.d310900000000p-12f,
    -0x1.d372b00000000p-16f, // phase 16
    -0x1.9874240000000p-16f,
    0x1.b366800000000p-12f,
    -0x1.42fad20000000p-9f,
    0x1.35f2400000000p-7f,
    -0x1.c7e4ce0000000p-6f,
    0x1.1d209e0000000p-4f,
    -0x1.56fb160000000p-3f,
    0x1.29aff40000000p-1f,
    0x1.5345780000000p-1f,
    -0x1.6bb8080000000p-3f,
    0x1.2d09c20000000p-4f,
    -0x1.e498660000000p-6f,
    0x1.4dc2360000000p-7f,
    -0x1.62f50e0000000p-9f,
    0x1.efd2ec0000000p-12f,
    -0x1.0793ba0000000p-15f, // phase 17
    -0x1.5f7ea60000000p-16f,
    0x1.919db20000000p-12f,
    -0x1.2f72200000000p-9f,
    0x1.2640460000000p-7f,
    -0x1.b3af840000000p-6f,
    0x1.1174900000000p-4f,
    -0x1.485ba40000000p-3f,
    0x1.143c120000000p-1f,
    0x1.6727840000000p-1f,
    -0x1.7147ac0000000p-3f,
    0x1.30d4140000000p-4f,
    -0x1.ec461c0000000p-6f,
    0x1.55340a0000000p-7f,
    -0x1.6e83140000000p-9f,
    0x1.046c4e0000000p-11f,
    -0x1.2500f80000000p-15f, // phase 18
    -0x1.29a7a20000000p-16f,
    0x1.6e71580000000p-12f,
    -0x1.1a21020000000p-9f,
    0x1.147d340000000p-7f,
    -0x1.9c25e40000000p-6f,
    0x1.03a0a80000000p-4f,
    -0x1.3749420000000p-3f,
    0x1.fd04880000000p-2f,
    0x1.7a44f60000000p-1f,
    -0x1.734a080000000p-3f,
    0x1.3192a20000000p-4f,
    -0x1.ef01560000000p-6f,
    0x1.593ede0000000p-7f,
    -0x1.76848e0000000p-9f,
    0x1.0ea1680000000p-11f,
    -0x1.411a740000000p-15f, // phase 19
    -0x1.ef907c0000000p-17f,
    0x1.4a8d7c0000000p-12f,
    -0x1.0372be0000000p-9f,
    0x1.0101920000000p-7f,
    -0x1.81ba640000000p-6f,
    0x1.e7cc120000000p-5f,
    -0x1.2410740000000p-3f,
    0x1.d145440000000p-2f,
    0x1.8c7fc60000000p-1f,
    -0x1.71820a0000000p-3f,
    0x1.2f188e0000000p-4f,
    -0x1.ec74fa0000000p-6f,
    0x1.5996500000000p-7f,
    -0x1.7a89ea0000000p-9f,
    0x1.1615b80000000p-11f,
    -0x1.5adc3c0000000p-15f, // phase 20
    -0x1.94feea0000000p-17f,
    0x1.268df80000000p-12f,
    -0x1.d79bc00000000p-10f,
    0x1.d849e00000000p-8f,
    -0x1.64e0f20000000p-6f,
    0x1.c50ef80000000p-5f,
    -0x1.0efeba0000000p-3f,
    0x1.a579e40000000p-2f,
    0x1.9dbb100000000p-1f,
    -0x1.6bb7ca0000000p-3f,
    0x1.293f640000000p-4f,
    -0x1.e4565c0000000p-6f,
    0x1.55f4300000000p-7f,
    -0x1.7a27700000000p-9f,
    0x1.1a53fe0000000p-11f,
    -0x1.7126be0000000p-15f, // phase 21
    -0x1.4469ba0000000p-17f,
    0x1.02fdc60000000p-12f,
    -0x1.a7269a0000000p-10f,
    0x1.ac79d20000000p-8f,
    -0x1.460d8c0000000p-6f,
    0x1.9f916a0000000p-5f,
    -0x1.f0c3a80000000p-4f,
    0x1.79e0da0000000p-2f,
    0x1.addb4a0000000p-1f,
    -0x1.61b92e0000000p-3f,
    0x1.1fe7d00000000p-4f,
    -0x1.d666a40000000p-6f,
    0x1.4e19c40000000p-7f,
    -0x1.74f71e0000000p-9f,
    0x1.1ae6780000000p-11f,
    -0x1.82c13c0000000p-15f, // phase 22
    -0x1.fc70440000000p-18f,
    0x1.c0ad540000000p-13f,
    -0x1.763ada0000000p-10f,
    0x1.7f387e0000000p-8f,
    -0x1.25b3000000000p-6f,
    0x1.77db1e0000000p-5f,
    -0x1.c10e360000000p-4f,
    0x1.4eb7040000000p-2f,
    0x1.bcc6820000000p-1f,
    -0x1.535a960000000p-3f,
    0x1.12fa540000000p-4f,
    -0x1.c274420000000p-6f,
    0x1.41d11c0000000p-7f,
    -0x1.6a9a840000000p-9f,
    0x1.1758aa0000000p-11f,
    -0x1.8e5cea0000000p-15f, // phase 23
    -0x1.84e2b60000000p-18f,
    0x1.7e024c0000000p-13f,
    -0x1.457f1a0000000p-10f,
    0x1.5124200000000p-8f,
    -0x1.0441ae0000000p-6f,
    0x1.4e72b00000000p-5f,
    -0x1.8f75ac0000000p-4f,
    0x1.2437480000000p-2f,
    0x1.ca648a0000000p-1f,
    -0x1.40775e0000000p-3f,
    0x1.0267de0000000p-4f,
    -0x1.a85c360000000p-6f,
    0x1.30ee4e0000000p-7f,
    -0x1.5abcb60000000p-9f,
    0x1.0f396c0000000p-11f,
    -0x1.9298f40000000p-15f, // phase 24
    -0x1.218ac00000000p-18f,
    0x1.3ea95a0000000p-13f,
    -0x1.158ab20000000p-10f,
    0x1.22d2460000000p-8f,
    -0x1.c44ce00000000p-7f,
    0x1.23dc460000000p-5f,
    -0x1.5c8f460000000p-4f,
    0x1.f534420000000p-3f,
    0x1.d69f380000000p-1f,
    -0x1.28f26c0000000p-3f,
    0x1.dc54d20000000p-5f,
    -0x1.880b4a0000000p-6f,
    0x1.1b50b20000000p-7f,
    -0x1.45143c0000000p-9f,
    0x1.021cfe0000000p-11f,
    -0x1.8e07340000000p-15f, // phase 25
    -0x1.a2598e0000000p-19f,
    0x1.0330400000000p-13f,
    -0x1.cdca160000000p-11f,
    0x1.e99d380000000p-9f,
    -0x1.7f93200000000p-7f,
    0x1.f130b40000000p-6f,
    -0x1.28ebae0000000p-4f,
    0x1.a42a780000000p-3f,
    0x1.e162880000000p-1f,
    -0x1.0cb6aa0000000p-3f,
    0x1.ac8afa0000000p-5f,
    -0x1.617f340000000p-6f,
    0x1.00e4080000000p-7f,
    -0x1.2964fe0000000p-9f,
    0x1.df3eb60000000p-12f,
    -0x1.7f31d40000000p-15f, // phase 26
    -0x1.240b1e0000000p-19f,
    0x1.9809da0000000p-14f,
    -0x1.740a3e0000000p-11f,
    0x1.8f33d20000000p-9f,
    -0x1.3b1bc40000000p-7f,
    0x1.9a450a0000000p-6f,
    -0x1.ea2bf80000000p-5f,
    0x1.55b6440000000p-3f,
    0x1.ea9cc60000000p-1f,
    -0x1.d76edc0000000p-4f,
    0x1.758d3a0000000p-5f,
    -0x1.34c78a0000000p-6f,
    0x1.c343120000000p-8f,
    -0x1.07823a0000000p-9f,
    0x1.aecd240000000p-12f,
    -0x1.64a1a60000000p-15f, // phase 27
    -0x1.877bae0000000p-20f,
    0x1.32ee7a0000000p-14f,
    -0x1.1ea2aa0000000p-11f,
    0x1.3752900000000p-9f,
    -0x1.ef3fee0000000p-8f,
    0x1.43e0f40000000p-6f,
    -0x1.8325540000000p-5f,
    0x1.0a35bc0000000p-3f,
    0x1.f23ebc0000000p-1f,
    -0x1.8be1ac0000000p-4f,
    0x1.378b460000000p-5f,
    -0x1.0206900000000p-6f,
    0x1.7b21ca0000000p-8f,
    -0x1.bea0a40000000p-10f,
    0x1.724a760000000p-12f,
    -0x1.3ce5640000000p-15f, // phase 28
    -0x1.f1277c0000000p-21f,
    0x1.aeebd80000000p-15f,
    -0x1.9c7e220000000p-12f,
    0x1.c596200000000p-10f,
    -0x1.6b96020000000p-8f,
    0x1.ddc4420000000p-7f,
    -0x1.1dbd640000000p-5f,
    0x1.83feda0000000p-4f,
    0x1.f83bc60000000p-1f,
    -0x1.36d01c0000000p-4f,
    0x1.e594140000000p-6f,
    -0x1.92e3c80000000p-7f,
    0x1.29922e0000000p-8f,
    -0x1.618d240000000p-10f,
    0x1.2939e40000000p-12f,
    -0x1.06999c0000000p-15f, // phase 29
    -0x1.1fdae20000000p-21f,
    0x1.0ba84a0000000p-15f,
    -0x1.06d55e0000000p-12f,
    0x1.24ab160000000p-10f,
    -0x1.d8e4b00000000p-9f,
    0x1.382dde0000000p-7f,
    -0x1.75bafc0000000p-6f,
    0x1.f5875c0000000p-5f,
    0x1.fc89f60000000p-1f,
    -0x1.b0ae140000000p-5f,
    0x1.4f48080000000p-6f,
    -0x1.16a5d80000000p-7f,
    0x1.9dc47e0000000p-9f,
    -0x1.efc3400000000p-11f,
    0x1.a683620000000p-13f,
    -0x1.80e2aa0000000p-16f, // phase 30
    -0x1.0532bc0000000p-22f,
    0x1.f072920000000p-17f,
    -0x1.f489720000000p-14f,
    0x1.1a41880000000p-11f,
    -0x1.cbb70c0000000p-10f,
    0x1.30f3f00000000p-8f,
    -0x1.6d73e40000000p-7f,
    0x1.e517d60000000p-6f,
    0x1.ff22240000000p-1f,
    -0x1.c298920000000p-6f,
    0x1.5a25900000000p-7f,
    -0x1.201c720000000p-8f,
    0x1.ae049a0000000p-10f,
    -0x1.03c39e0000000p-11f,
    0x1.c0c0540000000p-14f,
    -0x1.a4fd660000000p-17f, // phase 31
};
// ===========================================================================
// END FROZEN POLYPHASE COEFFICIENT TABLE
// ===========================================================================

// The exact-integer phase math shared by the block kernel and the streaming
// front-end (Constraint 2, doc 11:216-234): for output frame `n` at the ratio
// src:dst, the native center frame + polyphase phase index, computed as an EXACT
// rational and rounded ONCE -- never a float accumulation across frames.
struct FramePos {
  std::int64_t center;
  std::uint64_t phase;
};

inline FramePos frame_pos(std::int64_t n, std::uint64_t src_rate, std::uint64_t dst_rate) {
  const std::uint64_t phases = static_cast<std::uint64_t>(k_resampler_phases);
  const std::uint64_t pos_num = static_cast<std::uint64_t>(n) * src_rate;
  std::int64_t center = static_cast<std::int64_t>(pos_num / dst_rate);
  const std::uint64_t rem = pos_num % dst_rate;
  std::uint64_t phase = (rem * phases + dst_rate / 2) / dst_rate; // one rounding
  if (phase >= phases) {
    phase -= phases; // the top half-step lands on the next sample
    center += 1;
  }
  return {center, phase};
}

// One output frame's ordered per-channel MAC over a `taps`-wide polyphase window
// (doc 16 determinism): `sample(idx, c)` returns the native sample at absolute
// frame `idx` for channel `c` (0 for a tap outside the input, the edge
// convention). `coeffs` is the active bank (the frozen input-Nyquist table for
// upsampling; the generated widened device-Nyquist bank for decimation), laid out
// phase-major with `taps` coefficients per phase. The single tap loop shared by
// both the whole-stream and the streaming callers guarantees they run
// bit-identical MACs over the same bank.
template <typename Sample>
inline void mac_frame(const FramePos& fp, std::uint32_t ch, const float* coeffs, std::int64_t taps,
                      Sample&& sample, float* out_frame) {
  const std::int64_t half = taps / 2 - 1; // N-1: the delta tap offset (phase 0)
  const float* coef = coeffs + static_cast<std::size_t>(fp.phase) * static_cast<std::size_t>(taps);
  for (std::uint32_t c = 0; c < ch; ++c) {
    float acc = 0.0F; // ordered reduction, tap 0..2N-1
    for (std::int64_t k = 0; k < taps; ++k) {
      const std::int64_t idx = fp.center - half + k;
      acc += coef[static_cast<std::size_t>(k)] * sample(idx, c);
    }
    out_frame[c] = acc;
  }
}

// The oldest absolute input index the filter support of output frame `n` reaches
// (center - half), and the newest (center + half + 1), for a `taps`-wide window.
// Shared so the streaming front-end's retain/ready bounds stay locked to the
// active bank's tap window (widened under decimation).
inline std::int64_t support_oldest(const FramePos& fp, std::int64_t taps) {
  return fp.center - (taps / 2 - 1);
}
inline std::int64_t support_newest(const FramePos& fp, std::int64_t taps) {
  return fp.center + (taps - 1) - (taps / 2 - 1);
}

// ---------------------------------------------------------------------------
// Ratio-scaled widened-lowpass decimation bank (D2/D3, Constraint 2).
// ---------------------------------------------------------------------------
// For `src_rate > dst_rate` the reconstruction/anti-alias lowpass is cut at the
// lower DEVICE Nyquist (`dst_rate/2`), not the fixed input Nyquist: the impulse
// response widens by the decimation ratio `src_rate/dst_rate` and the tap support
// scales with it so stopband attenuation is preserved. The bank is generated over
// the SAME Blackman-Harris windowed-sinc prototype and 32-phase bank as the frozen
// upsampling table -- one generator, not a second algorithm -- but its cutoff is
// ratio-dependent so it cannot be pre-frozen (a continuous device rate has no single
// table). Generation runs OFF the RT thread only (construction / the whole-stream
// oracle); it is the sole place libm's `std::sin`/`std::cos` are evaluated. The RT
// inner loop then MACs over the resident float32 table, no-libm (Constraint 1).
//
// Unlike the upsampling table there is NO forced integer-argument Kronecker delta: an
// aligned integer-ratio sample must NOT be reproduced verbatim (that would defeat the
// anti-aliasing) -- every phase is a genuine device-Nyquist lowpass. That single boolean
// is the ONLY difference between this bank and the frozen one; both come out of
// `resampler_prototype::generate` (arbc/media/resampler_prototype.hpp), which is what
// makes "one generator, not a second algorithm" a fact rather than an aspiration.
inline PolyphaseBank generate_decimation_bank(std::uint64_t src_rate, std::uint64_t dst_rate) {
  const std::uint64_t phases = static_cast<std::uint64_t>(k_resampler_phases);
  const std::uint64_t half_base = k_resampler_taps / 2; // 8 lobes/side at the input Nyquist
  // ceil(half_base * src/dst): scale the half support by the decimation ratio so the
  // widened sinc keeps the base filter's lobe count (its stopband quality).
  const std::uint64_t half_lobes = (half_base * src_rate + dst_rate - 1) / dst_rate;
  const std::int64_t taps = static_cast<std::int64_t>(2 * half_lobes);
  const double fc = static_cast<double>(dst_rate) / static_cast<double>(src_rate); // < 1
  return resampler_prototype::generate(phases, taps, fc, /*force_integer_zero=*/false);
}

} // namespace

// Hands the component's own tests the frozen table (internal header; not public API), so
// the generator that regenerates it is guarded against drifting away from it.
namespace resampler_prototype {

FrozenBank frozen_bank() {
  return FrozenBank{k_resampler_coeffs.data(), k_resampler_coeffs.size(),
                    static_cast<std::uint64_t>(k_resampler_phases),
                    static_cast<std::int64_t>(k_resampler_taps)};
}

} // namespace resampler_prototype

void resample_audio(const AudioBlock& in, AudioBlock& out) {
  // Both rate directions are reconstructed here; only the equal-rate / layout /
  // null shapes keep the caller's 1:1 path, so those remain a no-op (Constraint 3).
  if (in.rate == 0 || out.rate == 0 || in.rate == out.rate || in.layout != out.layout ||
      in.samples == nullptr || out.samples == nullptr) {
    return;
  }
  const std::uint32_t ch = channel_count(in.layout);
  const std::uint64_t src_rate = in.rate;
  const std::uint64_t dst_rate = out.rate;
  const std::int64_t in_frames = static_cast<std::int64_t>(in.frames);

  // Upsampling reuses the FROZEN input-Nyquist table; decimation generates the
  // ratio-scaled widened device-Nyquist bank once (off the RT thread -- this whole-
  // stream function is the oracle, never the callback). Both MAC over the same
  // exact-integer phase math (`frame_pos`), so the two directions differ only in the
  // active bank/tap-count (D1/D2).
  const bool decimating = src_rate > dst_rate;
  PolyphaseBank dec;
  const float* coeffs = k_resampler_coeffs.data();
  std::int64_t taps = static_cast<std::int64_t>(k_resampler_taps);
  if (decimating) {
    dec = generate_decimation_bank(src_rate, dst_rate);
    coeffs = dec.coeffs.data();
    taps = dec.taps;
  }

  for (std::uint32_t n = 0; n < out.frames; ++n) {
    const FramePos fp = frame_pos(static_cast<std::int64_t>(n), src_rate, dst_rate);
    mac_frame(
        fp, ch, coeffs, taps,
        [&](std::int64_t idx, std::uint32_t c) -> float {
          // Taps outside `in` read as zero (the defined edge convention).
          return (idx >= 0 && idx < in_frames) ? in.samples[static_cast<std::size_t>(idx) * ch + c]
                                               : 0.0F;
        },
        out.samples + static_cast<std::size_t>(n) * ch);
  }
}

void StreamingResampler::configure(std::uint32_t src_rate, std::uint32_t dst_rate,
                                   std::uint32_t channels, std::uint32_t block_frames) {
  d_src_rate = src_rate;
  d_dst_rate = dst_rate;
  d_channels = channels;
  // Select the active bank once, off the RT thread (D3/D5): the frozen input-Nyquist
  // table for upsampling / matched, the generated widened device-Nyquist bank for
  // decimation. The generated coefficients are owned here and never regenerated on
  // the RT path; `d_taps` is the widened tap count (== k_resampler_taps upsampling).
  if (src_rate > dst_rate) {
    d_dec_coeffs = generate_decimation_bank(src_rate, dst_rate).coeffs;
    d_taps = static_cast<std::uint32_t>(d_dec_coeffs.size() /
                                        (static_cast<std::size_t>(k_resampler_phases)));
  } else {
    d_dec_coeffs.clear();
    d_taps = static_cast<std::uint32_t>(k_resampler_taps);
  }
  // One pushed block plus the filter-support residual retained across a produce
  // boundary (<= 2*taps-1 live frames between pushes; D5 pre-sizing). The residual
  // scales with the widened tap count so decimation stays allocation-free (Constraint
  // 6). Storage is allocated here and never grows on the RT path.
  d_capacity_frames = block_frames + 2u * d_taps;
  d_history.assign(static_cast<std::size_t>(d_capacity_frames) * channels, 0.0F);
  reset();
}

void StreamingResampler::reset() noexcept ARBC_RT_NONBLOCKING {
  d_hist_len = 0;
  d_hist_base = 0;
  d_out_index = 0;
}

bool StreamingResampler::can_produce() const noexcept ARBC_RT_NONBLOCKING {
  if (d_dst_rate == 0) {
    return false;
  }
  const FramePos fp = frame_pos(d_out_index, d_src_rate, d_dst_rate);
  // Ready once the newest tap the next output frame reaches is resident.
  return d_hist_base + static_cast<std::int64_t>(d_hist_len) >
         support_newest(fp, static_cast<std::int64_t>(d_taps));
}

void StreamingResampler::produce(float* out_frame) noexcept ARBC_RT_NONBLOCKING {
  // Select the active bank without a stored pointer (copy-safe): the frozen
  // input-Nyquist table upsampling / matched, the generated widened bank decimating.
  const float* coeffs = d_dec_coeffs.empty() ? k_resampler_coeffs.data() : d_dec_coeffs.data();
  const FramePos fp = frame_pos(d_out_index, d_src_rate, d_dst_rate);
  mac_frame(
      fp, d_channels, coeffs, static_cast<std::int64_t>(d_taps),
      [&](std::int64_t idx, std::uint32_t c) -> float {
        const std::int64_t local = idx - d_hist_base;
        // idx < 0 (before the stream start) and any not-yet-resident tap read as
        // zero -- the same edge convention as `resample_audio`.
        if (local < 0 || local >= static_cast<std::int64_t>(d_hist_len)) {
          return 0.0F;
        }
        return d_history[static_cast<std::size_t>(local) * d_channels + c];
      },
      out_frame);
  ++d_out_index;
}

void StreamingResampler::push_input(const float* samples,
                                    std::uint32_t frames) noexcept ARBC_RT_NONBLOCKING {
  if (frames == 0) {
    return;
  }
  // Drop history frames no future output can reach: the oldest index the next
  // output frame needs is support_oldest(center(out_index)). Compaction is an
  // allocation-free memmove and preserves d_hist_base + d_hist_len == frames-ever-
  // pushed, so a bounded window stays within the configured capacity.
  const FramePos fp = frame_pos(d_out_index, d_src_rate, d_dst_rate);
  const std::int64_t retain = support_oldest(fp, static_cast<std::int64_t>(d_taps));
  if (d_hist_len > 0) {
    std::int64_t drop = retain - d_hist_base;
    if (drop > static_cast<std::int64_t>(d_hist_len)) {
      // Under decimation the output stride skips more than one input frame per output,
      // so the next needed input can lie entirely past the resident window: drop it
      // all (the base advances to the running push count -- the newly pushed frames'
      // absolute origin). Unreachable while upsampling (stride < 1).
      drop = static_cast<std::int64_t>(d_hist_len);
    }
    if (drop > 0) {
      const std::size_t off = static_cast<std::size_t>(drop) * d_channels;
      const std::size_t remain =
          static_cast<std::size_t>(d_hist_len - static_cast<std::uint32_t>(drop)) * d_channels;
      std::memmove(d_history.data(), d_history.data() + off, remain * sizeof(float));
      d_hist_len -= static_cast<std::uint32_t>(drop);
      d_hist_base += drop;
    }
  }
  const std::size_t dst = static_cast<std::size_t>(d_hist_len) * d_channels;
  std::memcpy(d_history.data() + dst, samples,
              static_cast<std::size_t>(frames) * d_channels * sizeof(float));
  d_hist_len += frames;
}

} // namespace arbc
