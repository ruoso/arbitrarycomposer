// CI-only dual-build module for org.arbc.nested (`kinds.dual_build`, doc 17:129-132).
// The service-injection case: the `Registry` factory carries no service handles, so
// the content is constructed UNATTACHED across the boundary and the host injects its
// `PullService`, `Backend`, `NestedResolver` and pinned `DocRoot` afterwards
// (nested_content.hpp:68) -- exactly as the runtime binders do in-lib. The child is
// named by `ObjectId` only; resolution is entirely host-side.

#include "ci_kinds.hpp"

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_nested/nested_content.hpp>

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  (void)registry.add(
      arbc::NestedContent::kind_id,
      [](arbc::ContentConfig config) { return arbc_ci::make_nested(config); },
      arbc_ci::metadata("Nested Composition"));
}
