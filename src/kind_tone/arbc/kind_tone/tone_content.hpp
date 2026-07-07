#pragma once

#include <arbc/contract/content.hpp>

#include <cstdint>
#include <memory>
#include <optional>

namespace arbc {

// Reference kind org.arbc.tone (doc 12:226-232): the "hello world" of the audio
// facet and the audio sibling of org.arbc.solid. Audio-only content -- it
// reports present-but-empty visual bounds (culled from every visual pass) and
// exposes a Static procedural AudioFacet that synthesizes a deterministic tone
// at any requested sample rate. Every sample is a pure function of absolute
// content-local time, so the kind is stateless, block-continuous, and trivially
// thread-safe.
class ToneContent final : public Content {
public:
  // frequency_hz is an integer number of cycles per second so the synthesis
  // phase reduces with exact integer arithmetic (see tone_content.cpp);
  // amplitude scales the unit waveform.
  ToneContent(std::uint32_t frequency_hz, float amplitude);

  // Visual surface -- a culled stub (doc 12:85-87). Empty bounds drop tone from
  // every visual pass; the transparent render only has to satisfy the visual
  // conformance families.
  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  // Audio surface -- the non-null facet that makes tone participate in the mix.
  AudioFacet* audio() override;

  static constexpr const char* kind_id = "org.arbc.tone";

private:
  // Static procedural tone generator. Holds only immutable construction params;
  // render_audio is a pure function of the request, so the facet is stateless,
  // block-continuous, and thread-safe. Byte-exact and portable: the waveform is
  // a fixed parabolic sine over an exact integer flick phase, never std::sin.
  class ToneFacet final : public AudioFacet {
  public:
    ToneFacet(std::uint32_t frequency_hz, float amplitude);

    std::optional<TimeRange> audio_extent() const override;
    Stability audio_stability() const override;
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion> done) override;

  private:
    std::uint32_t d_frequency_hz;
    float d_amplitude;
  };

  ToneFacet d_facet;
};

} // namespace arbc
