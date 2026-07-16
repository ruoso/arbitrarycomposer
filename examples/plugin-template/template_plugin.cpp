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
#include <span>
#include <string>
#include <string_view>

namespace {

std::unique_ptr<arbc::Content> make_template_content() {
  return std::make_unique<arbc::SolidContent>(arbc::Rgba{1.0F, 1.0F, 1.0F, 1.0F});
}

} // namespace

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  // Kind ids are reverse-DNS (doc 03:251-255): replace with a domain you control.
  // The entry point returns void; a failed `add` is a value the loader infers from
  // the registry not growing -- no exceptions cross this boundary (doc 03:177-180).
  //
  // Beside the factory, the SAME add optionally registers a JSON-free kind codec
  // (so your kind persists in a document rather than round-tripping as a
  // placeholder) and an operator binder (so an operator kind receives its render
  // services at attach) -- see doc 03 §Registry. Both hooks traffic in plain
  // text/function pointers: no JSON library ever enters your link line. The
  // template's codec is the minimal legal one -- this kind has no parameters and
  // no inputs, so it emits an empty params object and reconstructs from nothing.
  (void)registry.add(
      "org.example.template",
      [](arbc::ContentConfig config)
          -> arbc::expected<std::unique_ptr<arbc::Content>, std::string> {
        // Your factory: parse the opaque, kind-defined config string and construct
        // your Content. The template ignores it and hands back the shipped solid
        // fill (premultiplied opaque white) to stay minimal.
        (void)config;
        return make_template_content();
      },
      arbc::KindMetadata{"Template Plugin", "0.1.0"},
      arbc::KindCodec{// The persistent version written beside your kind id in every document.
                      "0.1.0",
                      // Your serialize hook: the live content -> your `params` as a
                      // JSON-object TEXT you author (the core parses and canonicalizes it).
                      [](const arbc::Content&) -> arbc::expected<std::string, std::string> {
                        return std::string("{}");
                      },
                      // Your deserialize hook: the canonical `params` text plus the
                      // already-built, core-owned input edges. Validate your own input arity
                      // here -- errors are values, never throws (doc 03:177-180).
                      [](std::string_view /*params_text*/, std::span<const arbc::ContentRef> inputs,
                         arbc::ObjectId /*composition*/)
                          -> arbc::expected<std::unique_ptr<arbc::Content>, std::string> {
                        if (!inputs.empty()) {
                          return arbc::unexpected<std::string>(
                              "org.example.template: expected no inputs");
                        }
                        return make_template_content();
                      }});
}
