#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/compositor/anchored_viewports.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <span>

// Byte-exact backward-compatibility golden (doc 16:47-53): the generalized
// viewport-outward walk (`render_frame_anchored`) with `anchor == k_root_anchor`
// must reproduce `render_frame`'s output byte-for-byte. Rebase continuity itself
// is NOT a byte-exact golden -- doc 04:66 promises within-one-rounding, so it
// lands as the property test in src/compositor/t/anchored_viewports.t.cpp
// (refinement Decision 4).

namespace {

bool byte_identical(const arbc::Surface& lhs, const arbc::Surface& rhs) {
  const std::span<const float> a =
      std::as_const(lhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  const std::span<const float> b =
      std::as_const(rhs).span<arbc::PixelFormat::Rgba32fLinearPremul>();
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

} // namespace

// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("anchored walk over the root composition is byte-identical to render_frame") {
  arbc::Document document;
  const arbc::ObjectId root = document.add_composition(512.0, 512.0);
  const arbc::ObjectId back_c = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.25F, 0.5F, 0.75F, 1.0F}, arbc::Rect{0.0, 0.0, 512.0, 512.0}));
  const arbc::ObjectId back = document.add_layer(back_c, arbc::Affine::identity());
  const arbc::ObjectId front_c = document.add_content(std::make_shared<arbc::SolidContent>(
      arbc::Rgba{0.9F, 0.1F, 0.2F, 1.0F}, arbc::Rect{0.0, 0.0, 128.0, 128.0}));
  const arbc::ObjectId front = document.add_layer(front_c, arbc::Affine::translation(64.0, 96.0));
  document.attach_layer(root, back);
  document.attach_layer(root, front);

  arbc::CpuBackend backend;
  // Anchor at the root composition: the flat single-composition scene the offline
  // driver sources for itself, and the anchored walk descends the same members
  // (compositor.root_composition_frame_walk, doc 05:28-36).
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity(), root};

  // Reference: render_frame via the offline driver.
  const auto reference_result = arbc::render_offline(document, viewport, backend);
  REQUIRE(reference_result.has_value());
  const std::unique_ptr<arbc::Surface>& reference = *reference_result;

  // Candidate: the generalized anchored walk into a fresh target.
  const arbc::DocStatePtr state = document.pin();
  arbc::SurfacePool pool(backend);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> anchored =
      backend.make_surface(reference->width(), reference->height(), reference->format());
  REQUIRE(anchored.has_value());
  arbc::render_frame_anchored(
      *state, [&document](arbc::ObjectId id) { return document.resolve(id); }, viewport, backend,
      pool, **anchored);

  REQUIRE(byte_identical(*reference, **anchored));
}
