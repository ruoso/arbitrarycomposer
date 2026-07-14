// The loadable plugin's single translation unit: the extern "C" entry point the host
// resolves and calls to register `org.arbc.image` into a `Registry` (doc 03:164-171). No
// exceptions cross this boundary (doc 03:177-180) -- registration failures are values on
// the `Registry` result, and a missing/corrupt asset is an unavailable content, never a
// throw.
//
// Registering the FACTORY is all a plugin can do, and all this kind needs it to do: the
// serialize codec for `org.arbc.image` lives in `runtime` and is gated on this very
// registration (doc 17 "The codec line is a decoder line"). Without this plugin loaded,
// the runtime registers no image codec and an `org.arbc.image` layer round-trips verbatim
// as a `PlaceholderContent` -- a user without the plugin opens the document, saves, and
// loses nothing.
#include <arbc/contract/plugin.hpp>
#include <arbc/kind_image/image_content.hpp>

extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry) {
  registry.add(
      arbc::image::ImageContent::kind_id,
      [](arbc::ContentConfig config) { return arbc::image::make_image_content(config); },
      arbc::KindMetadata{"Image", "1"});
}
