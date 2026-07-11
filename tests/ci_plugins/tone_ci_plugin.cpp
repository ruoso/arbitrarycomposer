// CI-only dual-build module for org.arbc.tone (`kinds.dual_build`, doc 17:129-132).
// The audio-facet case: the driver reaches `ToneContent::audio()` across the image
// boundary and drives the audio conformance families over the plugin-side facet.

#include "ci_kinds.hpp"

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_tone/tone_content.hpp>

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  (void)registry.add(
      arbc::ToneContent::kind_id,
      [](arbc::ContentConfig config) { return arbc_ci::make_tone(config); },
      arbc_ci::metadata("Tone Generator"));
}
