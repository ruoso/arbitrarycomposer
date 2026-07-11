// CI-only dual-build module for org.arbc.fade (`kinds.dual_build`, doc 17:129-132).
//
// The operator case, and the one that makes the v1 seam's gap concrete: an input
// edge is a raw `ContentRef` (contract/content.hpp:212) and a `ContentConfig` is a
// string, so `FadeContent`'s input CANNOT cross the entry point. The module
// therefore owns the input it hands its operator (Decision 3), in a module-local
// holder destroyed at image unload -- strictly after the operator that borrows it,
// because `PluginHost` dlcloses last (runtime/plugin_host.hpp:154-155). Widening
// the seam so an out-of-lib operator can declare its inputs (and its codec, and its
// binder) is the registered follow-up `runtime.plugin_operator_registration`.

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_fade/fade_content.hpp>

#include "ci_kinds.hpp"

namespace {

// Module-local. A function-local static's destructor is registered against this
// image's DSO handle, so it runs during `dlclose` -- after every operator the host
// built from this factory has already been destroyed.
arbc_ci::InputOwner& module_inputs() {
  static arbc_ci::InputOwner owner;
  return owner;
}

} // namespace

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  (void)registry.add(
      arbc::FadeContent::kind_id,
      [](arbc::ContentConfig config) { return arbc_ci::make_fade(config, module_inputs()); },
      arbc_ci::metadata("Fade"));
}
