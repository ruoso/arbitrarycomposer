// Per-layer blend modes (issue #36), end to end: a layer says HOW its colour combines with
// what is under it, the compositor evaluates it in the composition's working space, and the
// document carries it.
//
// The oracle is computed IN-TEST from the reference formula, never by re-running the kernel:
// the whole claim is that arbc's `multiply` is the `multiply` every other tool means, so an
// assertion that only compared the compositor against itself would prove nothing about that.
//
// Cross-component (a runtime `Document` over the real `CpuBackend`, plus the serializer), so it
// lives here rather than in one component's `t/`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/blend_mode.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/offline.hpp>
#include <arbc/serialize/reader.hpp>
#include <arbc/serialize/writer.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

using namespace arbc;
using Catch::Approx;

namespace {

// The scene both the render and the oracle are computed over: an opaque backdrop with a
// half-covering foreground on top, both premultiplied and both bounded so nothing depends on
// unbounded-solid behaviour.
constexpr Rgba k_backdrop{0.6F, 0.3F, 0.2F, 1.0F};
constexpr Rgba k_foreground{0.4F, 0.8F, 0.5F, 1.0F};
constexpr int k_dim = 64;

// `Document` is neither copyable nor movable, so the scene is built INTO a caller-owned one
// and only the two ids come back.
struct Scene {
  ObjectId composition{};
  ObjectId top_layer{};
};

Scene build_scene(Document& document, double dim = k_dim) {
  Scene s;
  const ObjectId back =
      document.add_content(std::make_shared<SolidContent>(k_backdrop, Rect{0.0, 0.0, dim, dim}));
  const ObjectId front =
      document.add_content(std::make_shared<SolidContent>(k_foreground, Rect{0.0, 0.0, dim, dim}));
  s.composition = document.add_composition(dim, dim);
  document.attach_layer(s.composition, document.add_layer(back, Affine::identity()));
  s.top_layer = document.add_layer(front, Affine::identity());
  document.attach_layer(s.composition, s.top_layer);
  return s;
}

void set_blend(Document& doc, ObjectId layer, BlendMode mode) {
  Model::Transaction txn = doc.transact();
  txn.set_blend(layer, mode);
  REQUIRE(txn.commit().has_value());
}

// One premultiplied RGBA sample out of the rendered frame's working buffer.
std::array<float, 4> pixel_at(const Surface& surface, int x, int y) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  const std::size_t at =
      4 * ((static_cast<std::size_t>(y) * static_cast<std::size_t>(surface.width())) +
           static_cast<std::size_t>(x));
  return {px[at], px[at + 1], px[at + 2], px[at + 3]};
}

// The reference composite, written out here rather than shared with the kernel:
//
//   co = (1 - ab)*as*cs + ab*as*B(cb, cs) + (1 - as)*ab*cb
//   ao = as + ab*(1 - as)
//
// over UNPREMULTIPLIED colour, with both inputs opaque in this scene so `ao == 1`.
std::array<float, 4> reference_composite(Rgba backdrop, Rgba source, BlendMode mode,
                                         float opacity) {
  const float ab = backdrop.a;
  const float as = source.a * opacity;
  std::array<float, 4> out{};
  const std::array<float, 3> cb{backdrop.r / backdrop.a, backdrop.g / backdrop.a,
                                backdrop.b / backdrop.a};
  const std::array<float, 3> cs{source.r / source.a, source.g / source.a, source.b / source.a};
  for (std::size_t k = 0; k < 3; ++k) {
    const float mixed = blend_channel(mode, cb[k], cs[k]);
    out[k] = ((1.0F - ab) * as * cs[k]) + (ab * as * mixed) + ((1.0F - as) * ab * cb[k]);
  }
  out[3] = as + (ab * (1.0F - as));
  return out;
}

std::string save(const Document& doc, const KindBridge& bridge) {
  const expected<std::string, SerializeError> out = save_document(doc, bridge);
  REQUIRE(out.has_value());
  return *out;
}

} // namespace

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("a layer's blend mode combines it with the backdrop by the reference formula") {
  CpuBackend backend;
  // Every mode, against one hand-written oracle. Driving all twelve rather than a favourite
  // few is what makes the claim "the vocabulary", not "multiply works".
  for (std::size_t i = 0; i < k_blend_mode_count; ++i) {
    const auto mode = static_cast<BlendMode>(i);
    INFO(blend_mode_name(mode));
    Document document;
    const Scene scene = build_scene(document);
    set_blend(document, scene.top_layer, mode);

    const Viewport viewport{k_dim, k_dim, Affine::identity(), scene.composition};
    const auto frame = render_offline(document, viewport, backend);
    REQUIRE(frame.has_value());

    const std::array<float, 4> got = pixel_at(**frame, k_dim / 2, k_dim / 2);
    const std::array<float, 4> want = reference_composite(k_backdrop, k_foreground, mode, 1.0F);
    for (std::size_t k = 0; k < 4; ++k) {
      CHECK(got[k] == Approx(want[k]).margin(1e-5));
    }
  }
}

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("Normal is source-over exactly, and it is what an untouched layer gets") {
  // The compatibility half. A layer that never asked for a mode must composite the bytes it
  // always did -- which is why the kernel keeps a separate arm for `Normal` rather than
  // reaching the same answer through the general formula.
  CpuBackend backend;
  Document untouched_doc;
  Document normal_doc;
  const Scene untouched = build_scene(untouched_doc);
  const Scene explicit_normal = build_scene(normal_doc);
  set_blend(normal_doc, explicit_normal.top_layer, BlendMode::Normal);

  const Viewport a{k_dim, k_dim, Affine::identity(), untouched.composition};
  const Viewport b{k_dim, k_dim, Affine::identity(), explicit_normal.composition};
  const auto lhs = render_offline(untouched_doc, a, backend);
  const auto rhs = render_offline(normal_doc, b, backend);
  REQUIRE(lhs.has_value());
  REQUIRE(rhs.has_value());

  const std::array<float, 4> got = pixel_at(**lhs, 4, 4);
  // An opaque source-over lands the source exactly.
  CHECK(got[0] == Approx(k_foreground.r).margin(1e-6));
  CHECK(got[1] == Approx(k_foreground.g).margin(1e-6));
  CHECK(got[2] == Approx(k_foreground.b).margin(1e-6));
  CHECK(got[3] == Approx(1.0F).margin(1e-6));
  CHECK(pixel_at(**rhs, 4, 4) == got);

  // And the record's default IS Normal, without anyone setting it.
  const DocStatePtr pin = untouched_doc.pin();
  const LayerRecord* const layer = pin->find_layer(untouched.top_layer);
  REQUIRE(layer != nullptr);
  CHECK(layer->blend() == BlendMode::Normal);
}

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("opacity fades a blended layer toward the backdrop, not toward a different blend") {
  // Opacity and blend are one placement and two questions: how much lands, and how what lands
  // combines. The reference divides opacity out before evaluating `B`, so a half-opacity
  // multiply is halfway between the backdrop and the full multiply -- not a multiply of a
  // half-strength colour, which is what a naive implementation that blended the premultiplied
  // value would give.
  CpuBackend backend;
  Document document;
  const Scene scene = build_scene(document);
  set_blend(document, scene.top_layer, BlendMode::Multiply);
  {
    Model::Transaction txn = document.transact();
    txn.set_opacity(scene.top_layer, 0.5);
    REQUIRE(txn.commit().has_value());
  }

  const Viewport viewport{k_dim, k_dim, Affine::identity(), scene.composition};
  const auto frame = render_offline(document, viewport, backend);
  REQUIRE(frame.has_value());

  const std::array<float, 4> got = pixel_at(**frame, 8, 8);
  const std::array<float, 4> want =
      reference_composite(k_backdrop, k_foreground, BlendMode::Multiply, 0.5F);
  for (std::size_t k = 0; k < 4; ++k) {
    CHECK(got[k] == Approx(want[k]).margin(1e-5));
  }
  // Halfway between the untouched backdrop and the full multiply, on the red channel.
  const float full = reference_composite(k_backdrop, k_foreground, BlendMode::Multiply, 1.0F)[0];
  CHECK(got[0] == Approx(0.5F * (k_backdrop.r + full)).margin(1e-5));
}

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("a blended layer wider than one tile blends each pixel exactly once") {
  // The tiled driver composites a layer tile by tile. Under source-over a doubly-painted seam
  // pixel was a visible-but-subtle 0.75; under `multiply` it would be squared, so this is the
  // case where the apron window's paint-once partition stops being an optimisation and starts
  // being correctness. 512 wide crosses the 256px rung-0 cell boundary at x == 256.
  constexpr int k_wide = 512;
  CpuBackend backend;
  Document document;
  const Scene scene = build_scene(document, k_wide);
  set_blend(document, scene.top_layer, BlendMode::Multiply);

  const Viewport viewport{k_wide, k_wide, Affine::identity(), scene.composition};
  const auto frame = render_offline(document, viewport, backend);
  REQUIRE(frame.has_value());

  const std::array<float, 4> want =
      reference_composite(k_backdrop, k_foreground, BlendMode::Multiply, 1.0F);
  // Straddle the seam: the last pixel of tile 0, the first of tile 1, and one well inside each.
  for (const int x : {4, 255, 256, 400}) {
    INFO("x = " << x);
    const std::array<float, 4> got = pixel_at(**frame, x, 100);
    for (std::size_t k = 0; k < 4; ++k) {
      CHECK(got[k] == Approx(want[k]).margin(1e-5));
    }
  }
}

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("a layer's blend mode round-trips through the document, by name") {
  KindBridge bridge;
  Document doc;
  const ObjectId content =
      doc.add_content(std::make_shared<SolidContent>(k_foreground, Rect{0.0, 0.0, 16.0, 16.0}),
                      bridge.intern(SolidContent::kind_id, "1"));
  const ObjectId comp = doc.add_composition(16.0, 16.0);
  const ObjectId layer = doc.add_layer(content, Affine::identity());
  doc.attach_layer(comp, layer);

  // A source-over layer emits NO `blend` key: omit-on-default is what keeps every document
  // written before this existed byte-identical after a load/save round trip.
  CHECK(save(doc, bridge).find("blend") == std::string::npos);

  set_blend(doc, layer, BlendMode::ColorDodge);
  const std::string with_blend = save(doc, bridge);
  CHECK(with_blend.find(R"("blend": "color-dodge")") != std::string::npos);

  // Reload: the mode is back on the record, and a re-save is byte-identical.
  KindBridge reload_bridge;
  Document reloaded;
  Registry registry;
  REQUIRE(load_document(with_blend, reloaded, reload_bridge, registry).has_value());
  const DocStatePtr pin = reloaded.pin();
  ObjectId reloaded_layer{};
  ObjectId root{};
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  pin->for_each_layer_in(root, [&](ObjectId lid) { reloaded_layer = lid; });
  REQUIRE(reloaded_layer.valid());
  const LayerRecord* const lr = pin->find_layer(reloaded_layer);
  REQUIRE(lr != nullptr);
  CHECK(lr->blend() == BlendMode::ColorDodge);
  CHECK(save(reloaded, reload_bridge) == with_blend);
}

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("a blend name this build does not know is preserved, not lost and not fatal") {
  // Doc 08 Principle 4 applied to a VALUE rather than a key, which is the case it exists for:
  // a document written by a later version naming a mode this build cannot perform. Refusing
  // the document would cost a user their project over one layer's appearance; dropping the
  // name would lose it at the next save. So the layer renders source-over and the token
  // survives verbatim.
  const std::string doc_text =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)"
      R"({"blend":"luminosity","kind":"org.arbc.solid","kind_version":"1",)"
      R"("params":{"color":[1.0,0.0,0.0,1.0]}}]}})";

  KindBridge bridge;
  Document doc;
  Registry registry;
  REQUIRE(load_document(doc_text, doc, bridge, registry).has_value()); // NOT a reader error

  const DocStatePtr pin = doc.pin();
  ObjectId root{};
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  ObjectId layer{};
  pin->for_each_layer_in(root, [&](ObjectId lid) { layer = lid; });
  REQUIRE(layer.valid());
  REQUIRE(pin->find_layer(layer) != nullptr);
  CHECK(pin->find_layer(layer)->blend() == BlendMode::Normal); // rendered honestly

  // And re-emitted unchanged: the residual carries it, and the writer omits its own `normal`.
  CHECK(save(doc, bridge).find(R"("blend": "luminosity")") != std::string::npos);
}

// enforces: 07-color-and-pixel-formats#layer-blend-mode-composites-in-working-space
TEST_CASE("setting a blend mode disturbs no other placement state") {
  // The mode lives in the flag word's upper bits, so the one thing worth pinning about the
  // encoding is that writing it leaves visibility and audibility exactly where they were.
  Document doc;
  const ObjectId content =
      doc.add_content(std::make_shared<SolidContent>(k_foreground, Rect{0.0, 0.0, 8.0, 8.0}));
  const ObjectId comp = doc.add_composition(8.0, 8.0);
  const ObjectId layer = doc.add_layer(content, Affine::identity(), 0.25);
  doc.attach_layer(comp, layer);
  {
    Model::Transaction txn = doc.transact();
    txn.set_visible(layer, false);
    txn.set_audible(layer, false);
    REQUIRE(txn.commit().has_value());
  }
  set_blend(doc, layer, BlendMode::Screen);

  const LayerRecord* const after = doc.pin()->find_layer(layer);
  REQUIRE(after != nullptr);
  CHECK(after->blend() == BlendMode::Screen);
  CHECK_FALSE(after->visible());
  CHECK_FALSE(after->audible());
  CHECK(after->opacity == 0.25);

  // Setting it back to Normal clears the field rather than leaving a stale one.
  set_blend(doc, layer, BlendMode::Normal);
  REQUIRE(doc.pin()->find_layer(layer) != nullptr);
  CHECK(doc.pin()->find_layer(layer)->blend() == BlendMode::Normal);
  CHECK_FALSE(doc.pin()->find_layer(layer)->visible());
}
