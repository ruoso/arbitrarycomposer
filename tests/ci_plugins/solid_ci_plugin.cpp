// CI-only dual-build module for org.arbc.solid (`kinds.dual_build`, doc 17:129-132).
// One translation unit, one `extern "C"` entry point, exactly one registered kind
// id -- so the driver's `registry().size() == 1` assertion after a load actually
// discriminates "this kind registered" from "this kind was dragged in by another"
// (Constraint 4). Never installed; built only under `BUILD_TESTING`.

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>

#include "ci_kinds.hpp"

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  // The entry point returns void (contract/plugin.hpp:20): a failed `add` is a
  // value the loader infers from the registry not growing, never a throw.
  (void)registry.add(
      arbc::SolidContent::kind_id,
      [](arbc::ContentConfig config) { return arbc_ci::make_solid(config); },
      arbc_ci::metadata("Solid Fill"));
}
