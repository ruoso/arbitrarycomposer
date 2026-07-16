// The one translation unit of a minimal third-party arbc plugin
// (packaging.plugin_helper; the registration seam of doc 03:227-234).
//
// It compiles against INSTALLED public headers only and exposes exactly the one
// extern "C" entry point the production loader resolves (doc 03:164-171). The
// factory delegates to a shipped built-in content so this stays a BUILD template:
// authoring a Content of your own is what the contract docs and the conformance
// suite (find_package(arbc COMPONENTS testing)) teach, not this file.

#include <arbc/contract/plugin.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>

#include <memory>
#include <string>

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  // Kind ids are reverse-DNS (doc 03:251-255): replace with a domain you control.
  // The entry point returns void; a failed `add` is a value the loader infers from
  // the registry not growing -- no exceptions cross this boundary (doc 03:177-180).
  (void)registry.add(
      "org.example.template",
      [](arbc::ContentConfig config) -> arbc::expected<std::unique_ptr<arbc::Content>,
                                                       std::string> {
        // Your factory: parse the opaque, kind-defined config string and construct
        // your Content. The template ignores it and hands back the shipped solid
        // fill (premultiplied opaque white) to stay minimal.
        (void)config;
        return std::unique_ptr<arbc::Content>(
            std::make_unique<arbc::SolidContent>(arbc::Rgba{1.0F, 1.0F, 1.0F, 1.0F}));
      },
      arbc::KindMetadata{"Template Plugin", "0.1.0"});
}
