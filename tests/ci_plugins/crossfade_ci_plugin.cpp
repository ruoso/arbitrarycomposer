// CI-only dual-build module for org.arbc.crossfade (`kinds.dual_build`, doc 17:129-132).
// The two-input operator case. Like fade, the module owns the input edges the v1
// `ContentFactory` config cannot carry (Decision 3); unlike fade, they must expose a
// bounded `Timed` audio extent, because crossfade's audio facet is always `Timed`
// and its extent is the pure union of its inputs' -- see `arbc_ci::BoundedAudioLeaf`.

#include "ci_kinds.hpp"

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>

namespace {

// Module-local: destroyed at image unload, after every operator borrowing these
// inputs (runtime/plugin_host.hpp:154-155 dlcloses last).
arbc_ci::InputOwner& module_inputs() {
  static arbc_ci::InputOwner owner;
  return owner;
}

} // namespace

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  (void)registry.add(
      arbc::CrossfadeContent::kind_id,
      [](arbc::ContentConfig config) { return arbc_ci::make_crossfade(config, module_inputs()); },
      arbc_ci::metadata("Crossfade"));
}
