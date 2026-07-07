#include <arbc/kind_tone/tone_content.hpp>

#include <arbc/media/audio_block.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/surface/typed_span.hpp>

#include <cstddef>
#include <cstdint>

namespace arbc {

ToneContent::ToneContent(std::uint32_t frequency_hz, float amplitude)
    : d_facet(frequency_hz, amplitude) {}

// Present-but-empty bounds (doc 12:85-87): a zero-area `Rect{}` is the culled
// signal, so the compositor drops tone from every visual pass. `std::nullopt`
// would mean *unbounded* -- the opposite signal (paints everywhere), which
// would defeat the "costs every visual pass nothing" guarantee.
std::optional<Rect> ToneContent::bounds() const { return Rect{}; }

Stability ToneContent::stability() const { return Stability::Static; }

std::optional<TimeRange> ToneContent::time_extent() const { return std::nullopt; }

// Culled stub: empty bounds mean the compositor never requests a visual region,
// but the visual conformance families still render an arbitrary region and
// expect transparent output (any region is outside empty bounds). Fill the
// target all-zero (transparent working-space pixels) and settle INLINE,
// mirroring SolidContent::render.
std::optional<RenderResult> ToneContent::render(const RenderRequest& request,
                                                std::shared_ptr<RenderCompletion>) {
  const WorkingPixel transparent{0.0F, 0.0F, 0.0F, 0.0F};
  visit_surface(request.target, [&](auto typed) {
    using Traits = PixelTraits<decltype(typed)::format>;
    for (std::size_t i = 0; i + Traits::channels <= typed.data.size(); i += Traits::channels) {
      Traits::encode(transparent, &typed.data[i]);
    }
  });
  return RenderResult{request.scale, true, std::nullopt};
}

AudioFacet* ToneContent::audio() { return &d_facet; }

ToneContent::ToneFacet::ToneFacet(std::uint32_t frequency_hz, float amplitude)
    : d_frequency_hz(frequency_hz), d_amplitude(amplitude) {}

// A tone plays identically for all time with no natural start or end, so it
// declares no extent (defined everywhere -- a bounded extent would falsely
// silence it) and Static stability (doc 12:26-29, "a tone is Static"). Static
// does not mean constant samples: the samples still vary with absolute time; it
// means the facet is a fixed, deterministic, unquantized function of
// (window, rate, layout).
std::optional<TimeRange> ToneContent::ToneFacet::audio_extent() const { return std::nullopt; }

Stability ToneContent::ToneFacet::audio_stability() const { return Stability::Static; }

namespace {

// Byte-exact, platform-portable tone sample at absolute media time `t` (flicks).
//
// The phase is reduced with EXACT integer arithmetic: the fractional cycle is
// `(frequency_hz * t) mod flicks_per_second`, over `flicks_per_second`.
// Reducing `t` modulo `flicks_per_second` first keeps the product inside int64
// range for any audio-range frequency, so there is no float accumulation and no
// overflow; a frame's sample depends only on its absolute time `t`, never on
// its position within a block.
//
// The waveform is a fixed parabolic sine -- the classic
// `sin(x) ~= (4/pi)x - (4/pi^2)x|x|` over `x in [-pi, pi]`, here in reduced
// phase `p in [-1, 1)` (with `x = pi*p`) collapsing to `s = 4*p*(1 - |p|)` --
// evaluated with pure IEEE-754 basic operations (multiply, subtract), NEVER
// std::sin. libm's sin varies by ULPs across platforms/versions and would force
// a golden tolerance; these basic operations are correctly-rounded by
// IEEE-754, so the golden is byte-exact with no tolerance and identical across
// toolchains. The waveform is written in FACTORED form `4*p*(1 - |p|)`, not the
// algebraically-equal sum form `4p - 4p|p|`, precisely so no `a*b + c` pattern
// exists for the compiler to contract into an FMA -- FMA fusion would
// reintroduce platform-dependent rounding and break byte-exactness.
float tone_sample(std::uint32_t frequency_hz, float amplitude, std::int64_t t) {
  constexpr std::int64_t fps = Time::flicks_per_second;
  std::int64_t tm = t % fps;
  if (tm < 0) {
    tm += fps; // fold negative time into [0, fps) before the multiply
  }
  const std::int64_t num = static_cast<std::int64_t>(frequency_hz) * tm;
  const std::int64_t r = num % fps; // fractional-cycle numerator in [0, fps)
  const double frac = static_cast<double>(r) / static_cast<double>(fps);

  double p = 2.0 * frac; // phase in units of pi, [0, 2)
  if (p > 1.0) {
    p -= 2.0; // reduce to [-1, 1)
  }
  const double abs_p = p < 0.0 ? -p : p;
  const double s = 4.0 * p * (1.0 - abs_p); // parabolic sine, [-1, 1]
  return static_cast<float>(static_cast<double>(amplitude) * s);
}

} // namespace

// Rate independence (doc 12:23-28), the procedural side of the resolution/rate
// symmetry: a procedural source synthesizes directly at whatever rate is
// requested and never bottoms out at a native rate, so it always reports
// achieved_rate == request.sample_rate and exact == true, under both BestEffort
// and Exact. Each frame's sample is a pure function of its absolute
// content-local time `t_f = window.start + f * (flicks_per_second /
// sample_rate)` -- never of the frame's position in the block, never of
// accumulated phase -- so two identical requests are bit-identical and a window
// split at a frame boundary concatenates bit-identically to the single-window
// render (the check_audio_facet_consistency contract). All channels of a frame
// carry the same value (a mono tone spread across the layout). Settles INLINE;
// the completion is unused.
std::optional<AudioResult> ToneContent::ToneFacet::render_audio(const AudioRequest& request,
                                                                std::shared_ptr<AudioCompletion>) {
  const std::uint32_t ch = channel_count(request.layout);
  const std::int64_t fpf =
      Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  for (std::uint32_t f = 0; f < request.target.frames; ++f) {
    const std::int64_t t = request.window.start.flicks + static_cast<std::int64_t>(f) * fpf;
    const float value = tone_sample(d_frequency_hz, d_amplitude, t);
    for (std::uint32_t c = 0; c < ch; ++c) {
      request.target.samples[static_cast<std::size_t>(f) * ch + c] = value;
    }
  }
  return AudioResult{request.sample_rate, true};
}

} // namespace arbc
