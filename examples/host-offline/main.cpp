// examples/host-offline/main.cpp -- the minimal arbc embedding
// (packaging.examples).
//
// The offline renderer is just a viewport with no deadline (doc 01:124-125):
// build a Document, render ONE exact frame, convert the working-space pixels
// to straight-alpha sRGB8, and write a PNG. Everything this program links is
// arbc::arbc -- no codec, no GPU SDK, no GUI toolkit (doc 10:34-35); the PNG
// writer is example-local code (../common/png_writer.hpp).
//
// Errors are values throughout (doc 10, doc 02:406-412): every fallible step
// returns a value this program checks and reports; nothing throws.
//
// The scene is deliberately hand-computable -- axis-aligned solids with exact
// binary-float colors -- because CI validates the emitted PNG byte-exactly
// against expectations derived from first principles
// (tests/consumer/host_example_artifacts.cpp). Change the scene and that test
// changes with it.
//
// The [readme-quickstart:*] anchors below mark the region README.md's
// quickstart lifts byte-identically; tests/readme_quickstart.t.cpp enforces
// the sync (claim 16-sdlc-and-quality#readme-quickstart-is-the-shipped-example).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/builtin_kinds.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>
#include <arbc/version.hpp>

#include "../common/png_writer.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

int main(int argc, char** argv) {
  const char* out_path = argc > 1 ? argv[1] : "out.png";

  // The version pair <arbc/version.hpp> exists for: what this program compiled
  // against (the header's macro) vs what it linked (a symbol in the library).
  std::printf("host-offline: arbc header %s, library %s\n", ARBC_VERSION_STRING,
              arbc::linked_version_string());

  // [readme-quickstart:embed]
  // 1. Kind bootstrap: one call presents every built-in kind through the same
  //    Registry surface loaded plugins register into (doc 03 § Registry).
  arbc::Registry registry;
  arbc::register_builtin_kinds(registry);

  // 2. The document: a root composition and two solid layers, bottom-to-top in
  //    attach order (doc 05). The backdrop goes through the registry's factory
  //    -- the path a config-driven host takes; the overlay is constructed
  //    directly -- the programmatic host's path. Solid colors are
  //    PREMULTIPLIED working-space values (doc 07).
  arbc::Document document;
  const arbc::ObjectId comp = document.add_composition(32.0, 32.0);

  const arbc::ContentFactory* solid = registry.factory("org.arbc.solid");
  if (solid == nullptr) {
    std::puts("host-offline: org.arbc.solid is not registered");
    return 1;
  }
  // Opaque red, unbounded extent: the factory grammar is "r,g,b,a".
  arbc::expected<std::unique_ptr<arbc::Content>, std::string> backdrop = (*solid)("1,0,0,1");
  if (!backdrop.has_value()) {
    std::printf("host-offline: backdrop construction failed: %s\n", backdrop.error().c_str());
    return 1;
  }
  document.attach_layer(comp, document.add_layer(document.add_content(std::move(*backdrop)),
                                                 arbc::Affine::identity()));

  // Half-opacity green over the top-left quadrant: unit-square bounds scaled
  // to 16x16 composition units by the layer transform.
  const arbc::ObjectId overlay = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.0F, 0.5F, 0.0F, 0.5F}, arbc::Rect{0.0, 0.0, 1.0, 1.0}));
  document.attach_layer(comp, document.add_layer(overlay, arbc::Affine::scaling(16.0, 16.0)));

  // 3. One exact frame (doc 02:241-253). The target arrives in the
  //    composition's working space; a backend that cannot store that format
  //    reports a SurfaceError value, never an abort.
  arbc::CpuBackend backend;
  const arbc::Viewport viewport{32, 32, arbc::Affine::identity()};
  const arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame =
      arbc::render_offline(document, viewport, backend);
  if (!frame.has_value()) {
    std::puts("host-offline: render_offline could not produce the target surface");
    return 1;
  }
  const arbc::Surface& surface = **frame;
  // [/readme-quickstart:embed]

  // 4. Working space -> PNG. The frame is premultiplied linear-light float
  //    (doc 07); PNG wants straight-alpha sRGB8.
  //    PixelTraits<Rgba8Srgb>::encode is the library's own unpremultiply +
  //    linear->sRGB encode -- reuse it rather than hand-rolling the conversion
  //    (doing it by hand, e.g. premultiplying gamma-encoded samples, is where
  //    embedders usually get this subtly wrong).
  const std::span<const float> pixels = surface.span<arbc::PixelFormat::Rgba32fLinearPremul>();
  if (pixels.empty()) {
    std::puts("host-offline: the target surface has no CPU-readable float pixels");
    return 1;
  }
  using Srgb8 = arbc::PixelTraits<arbc::PixelFormat::Rgba8Srgb>;
  const int width = surface.width();
  const int height = surface.height();
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * 4);
  for (std::size_t i = 0; i < rgba.size() / 4; ++i) {
    const arbc::WorkingPixel working{pixels[4 * i], pixels[4 * i + 1], pixels[4 * i + 2],
                                     pixels[4 * i + 3]};
    Srgb8::encode(working, &rgba[4 * i]);
  }

  if (!png_writer::write_rgba8(out_path, width, height, rgba)) {
    std::printf("host-offline: writing %s failed\n", out_path);
    return 1;
  }
  std::printf("host-offline: wrote a %dx%d frame to %s\n", width, height, out_path);
  return 0;
}
