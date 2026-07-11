// CI-only dual-build module for org.arbc.raster (`kinds.dual_build`, doc 17:129-132).
// The `Editable` case: `RasterContent` derives from both `Content` and `Editable`
// (raster_content.hpp:225), so the driver's `editable()` call across the boundary
// exercises the plugin-side second-base-subobject pointer adjustment.
//
// Codec-free (Constraint 3): the pixels are synthesized arithmetically from a
// `<width>x<height>` config. The only artifact in the tree that carries a decode
// dependency is `arbc-plugin-imageseq` (doc 17:167-174, the codec line).

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_raster/raster_content.hpp>

#include "ci_kinds.hpp"

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  (void)registry.add(
      arbc::RasterContent::kind_id,
      [](arbc::ContentConfig config) { return arbc_ci::make_raster(config); },
      arbc_ci::metadata("Raster Image"));
}
