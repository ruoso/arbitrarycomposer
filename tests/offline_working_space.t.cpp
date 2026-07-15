#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>

namespace {

// A leaf content that records the tag triple of the surface it is asked to
// render into (the per-layer temp the compositor allocates). It witnesses that
// the temp carries the composition's working space; it produces no pixels of
// interest, only the tag observation.
class TagCapturingContent final : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 8.0, 8.0}; }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    d_target_format = request.target.format();
    d_rendered = true;
    arbc::RenderResult result;
    result.achieved_scale = request.scale;
    return result;
  }

  bool rendered() const { return d_rendered; }
  arbc::SurfaceFormat target_format() const { return d_target_format; }

private:
  arbc::SurfaceFormat d_target_format{};
  bool d_rendered{false};
};

} // namespace

// enforces: 07-color-and-pixel-formats#compositing-in-working-space
TEST_CASE("render_offline allocates target and temps in the composition's working space") {
  arbc::Document document;
  // Configure a working space distinct from the 32f default so the assertions
  // witness the configured value, not the hardcode that used to live in the
  // driver. The 8-bit sRGB fast mode is a storable working space (doc 07 rule 3).
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  document.set_working_space(comp, arbc::k_fast_rgba8srgb);

  const auto content = std::make_shared<TagCapturingContent>();
  const arbc::ObjectId cid = document.add_content(content);
  // The frame walk is composition-scoped: attach the layer to the composition the
  // offline driver sources, or the frame renders nothing
  // (compositor.root_composition_frame_walk, doc 05:28-36).
  document.attach_layer(comp, document.add_layer(cid, arbc::Affine::identity()));

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity()};
  const auto out = render_offline(document, viewport, backend);

  REQUIRE(out.has_value());
  // The target the compositor composites into carries the working-space tags.
  REQUIRE((*out)->format() == arbc::k_fast_rgba8srgb);
  // The per-layer temp the compositor allocated carries them too (witnessed at
  // render time from the request's target surface).
  REQUIRE(content->rendered());
  REQUIRE(content->target_format() == arbc::k_fast_rgba8srgb);
}

// enforces: 07-color-and-pixel-formats#compositing-in-working-space
TEST_CASE("render_offline surfaces an unstorable working space as an error, not a crash") {
  arbc::Document document;
  // A valid tag triple the reference backend cannot store: linear-premul pixels
  // reinterpreted in gamma-space sRGB. `make_surface` faults it as a value
  // (capability honesty, doc 07), which the driver must propagate.
  const arbc::SurfaceFormat unstorable{arbc::PixelFormat::Rgba32fLinearPremul, arbc::k_srgb,
                                       arbc::Premultiplied::Yes};
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  document.set_working_space(comp, unstorable);

  arbc::CpuBackend backend;
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity()};
  const auto out = render_offline(document, viewport, backend);

  REQUIRE_FALSE(out.has_value());
  REQUIRE(out.error() == arbc::SurfaceError::UnsupportedFormat);
}
