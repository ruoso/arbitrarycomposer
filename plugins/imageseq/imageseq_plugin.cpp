// The loadable plugin's single translation unit: the extern "C" entry point the
// host (or imageseq's own dlopen end-to-end test, until runtime.plugin_loading
// lands in M8) resolves and calls to register `org.arbc.imageseq` into a
// `Registry` (doc 03:164-171). No exceptions cross this boundary
// (doc 03:177-180) -- registration failures are values on the `Registry` result.
#include <arbc/contract/plugin.hpp>
#include <arbc/kind_imageseq/imageseq_content.hpp>

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  registry.add(
      arbc::imageseq::ImageSeqContent::kind_id,
      [](arbc::ContentConfig config) { return arbc::imageseq::make_imageseq_content(config); },
      arbc::KindMetadata{"Image Sequence", "1"});
}
