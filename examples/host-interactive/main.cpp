// examples/host-interactive/main.cpp -- the interactive embedding shape
// (packaging.examples).
//
// What a windowed arbc host looks like with the window removed (doc 00:76-79:
// windowing, input handling, and widgets belong to the host): a HostViewport
// bound to a Document (doc 01:112-121, the host's single wiring step), an
// InteractiveRenderer frame loop rendering into ONE persistent caller-owned
// surface (doc 02:83-87), and pan/zoom applied as CAMERA-TRANSFORM edits (doc
// 01:108) through HostViewport::set_camera over the anchored (anchor node,
// matrix) camera (doc 04:82-84).
//
// The driver is a deterministic scripted GESTURE TAPE, so the example runs
// headlessly on every CI lane with zero windowing dependencies. The tape is
// the ONLY thing a real host replaces -- see README.md for the swap point.
//
// Like host-offline, the CI scene is hand-computable: CI validates the
// final-frame PNG byte-exactly (tests/consumer/host_example_artifacts.cpp).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/builtin_kinds.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/surface/surface_pool.hpp>

#include "../common/png_writer.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

namespace {

// Device-space gestures: the transforms a toolkit's mouse/touch handlers would
// derive from input events. A gesture composes onto the camera's LEFT (the
// device side), so it stays valid whatever anchor the viewport has rebased the
// camera to (doc 04:82-84) -- the host never needs to know about rebasing.
arbc::Affine pan(double dx, double dy) { return arbc::Affine::translation(dx, dy); }

arbc::Affine zoom_about(double factor, double cx, double cy) {
  return compose(arbc::Affine::translation(cx, cy), compose(arbc::Affine::scaling(factor, factor),
                                                            arbc::Affine::translation(-cx, -cy)));
}

// Step until the loop is genuinely settled: no follow-up frame owed and no
// async tile render still in flight. A real event loop does the same thing by
// re-invoking step() from its idle/timer hook while follow-ups are owed.
bool settle(arbc::HostViewport& view, const arbc::InteractiveRenderer& renderer) {
  constexpr int k_max_steps = 64; // a convergence bound, never a timing assumption
  for (int i = 0; i < k_max_steps; ++i) {
    const arbc::HostViewport::StepOutcome outcome = view.step();
    if (!outcome.schedule_follow_up && renderer.pending().tiles.empty()) {
      return true;
    }
  }
  return false;
}

} // namespace

int main(int argc, char** argv) {
  const char* out_path = argc > 1 ? argv[1] : "out.png";

  // Kind bootstrap + document assembly, exactly as in host-offline: an opaque
  // dark-blue backdrop through the registry factory, a half-opacity gray panel
  // constructed directly. The panel's bounds are 256x256 composition units --
  // deliberately aligned to the interactive renderer's tile grid, because a
  // solid trusts the pipeline to request only in-bounds regions and the tile
  // path plans (but does not clip) whole tiles; tile-aligned bounds keep the
  // CI artifact hand-computable.
  arbc::Registry registry;
  arbc::register_builtin_kinds(registry);

  arbc::Document document;
  const arbc::ObjectId comp = document.add_composition(512.0, 512.0);

  const arbc::ContentFactory* solid = registry.factory("org.arbc.solid");
  if (solid == nullptr) {
    std::puts("host-interactive: org.arbc.solid is not registered");
    return 1;
  }
  arbc::expected<std::unique_ptr<arbc::Content>, std::string> backdrop = (*solid)("0,0,0.25,1");
  if (!backdrop.has_value()) {
    std::printf("host-interactive: backdrop construction failed: %s\n", backdrop.error().c_str());
    return 1;
  }
  const arbc::ObjectId backdrop_layer =
      document.add_layer(document.add_content(std::move(*backdrop)), arbc::Affine::identity());
  document.attach_layer(comp, backdrop_layer);

  const arbc::ObjectId panel = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.5F, 0.5F, 0.5F, 0.5F}, arbc::Rect{0.0, 0.0, 256.0, 256.0}));
  document.attach_layer(comp, document.add_layer(panel, arbc::Affine::identity()));

  // The interactive wiring (doc 01:112-121): backend, pool, tile cache, ONE
  // persistent target surface the frames render into (doc 02:83-87 -- the
  // surface is the host's, allocated once, reused every frame; a windowed host
  // would blit or texture-upload it after each step), renderer, viewport.
  arbc::CpuBackend backend;
  arbc::SurfacePool pool(backend);
  arbc::TileCache cache(64U * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(512, 512, document.pin()->working_space());
  if (!target.has_value()) {
    std::puts("host-interactive: the backend could not allocate the target surface");
    return 1;
  }

  arbc::InteractiveRenderer renderer;
  arbc::HostViewport::Config config;
  config.viewport = arbc::Viewport{512, 512, arbc::Affine::identity()};
  // A real host budgets a frame interval (the default is 16ms) and lets a slow
  // frame degrade to placeholders under deadline pressure (doc 02). This
  // example's artifact must be the fully refined frame regardless of machine
  // load, so the budget is effectively unbounded.
  config.budget = std::chrono::hours(1);
  // DocumentBinding{} is the right shape for a programmatically-built document
  // with no external references. The constructor installs the document's
  // damage sink; edits from here on reach this viewport's frame loop.
  arbc::HostViewport view(renderer, document, arbc::HostViewport::DocumentBinding{}, backend, pool,
                          cache, **target, {}, config);
  // A static scene: pin the playhead so pixels are a pure function of the
  // camera. A media host would instead let the owned transport free-run.
  view.set_playhead_source([] { return arbc::Time::zero(); });

  // KNOWN GAP the next lambda compensates for: doc 02 lists "camera changes"
  // among a frame's damage sources, but the runtime does not yet synthesize
  // damage from a camera delta -- a camera-only step() issues a frame that
  // repaints nothing, so a fully static scene would pan as a frozen image.
  // Until that seam lands, the host forces the repaint itself: re-adding a
  // fully transparent content emits structural content-keyed damage, which
  // maps to a whole-viewport repaint at the CURRENT camera. A pixel-exact
  // no-op -- compositing a transparent solid changes nothing.
  arbc::ObjectId driver_content{};
  arbc::ObjectId driver_layer{};
  const auto force_repaint = [&] {
    if (driver_layer.valid()) {
      document.remove_content(driver_content, comp, driver_layer);
    }
    driver_content = document.add_content(std::make_shared<arbc::SolidContent>(
        arbc::Rgba{0.0F, 0.0F, 0.0F, 0.0F}, arbc::Rect{0.0, 0.0, 256.0, 256.0}));
    driver_layer = document.add_layer(driver_content, arbc::Affine::identity());
    document.attach_layer(comp, driver_layer);
  };

  // The scene above was committed before the viewport existed, so those
  // commits predate its damage sink; the driver commit is the edit that
  // damages the scene and bootstraps the first frame.
  force_repaint();
  if (!settle(view, renderer)) {
    std::puts("host-interactive: the first frame never settled");
    return 1;
  }

  // The gesture tape: pan, zoom in about the viewport center, zoom back out,
  // pan again -- ending at a net pan of (-64, -64). Each iteration is EXACTLY
  // what a real host's input handler does: compose the gesture onto the
  // current camera, set it, and let the frame loop render.
  const arbc::Affine tape[] = {
      pan(32.0, 32.0),
      zoom_about(2.0, 256.0, 256.0),
      zoom_about(0.5, 256.0, 256.0),
      pan(-96.0, -96.0),
  };
  for (const arbc::Affine& gesture : tape) {
    view.set_camera(compose(gesture, view.camera()));
    force_repaint(); // see the KNOWN GAP note above
    if (!settle(view, renderer)) {
      std::puts("host-interactive: a gesture's frame never settled");
      return 1;
    }
  }
  std::printf("host-interactive: %llu frames issued across the tape\n",
              static_cast<unsigned long long>(view.frames_issued()));

  // Final frame -> PNG, the same working-space -> straight-alpha sRGB8
  // conversion host-offline documents.
  const arbc::Surface& surface = **target;
  const std::span<const float> pixels = surface.span<arbc::PixelFormat::Rgba32fLinearPremul>();
  if (pixels.empty()) {
    std::puts("host-interactive: the target surface has no CPU-readable float pixels");
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
    std::printf("host-interactive: writing %s failed\n", out_path);
    return 1;
  }
  std::printf("host-interactive: wrote the final %dx%d frame to %s\n", width, height, out_path);
  return 0;
}
