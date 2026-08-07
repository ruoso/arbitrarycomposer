// The content-resample verb (issue #31): a host asks a content to grow its own WORKING GRID,
// and discovers whether it can through a null-default `Content` virtual rather than a per-kind
// allowlist. These pin both halves -- the discovery and the verb -- and the property that makes
// the verb worth having: the resampled pixels are the ones a render at that density produces,
// so "resample to crisp" is not a second sampling policy to keep in agreement with the first.

#include <arbc/contract/resampleable.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace {

using namespace arbc;

// A 16x16 checkerboard at tile edge 4: a FLAT field would resample to itself under any filter
// and hide everything this file is about.
constexpr int k_dim = 16;
constexpr int k_edge = 4;

DecodedImage checker() {
  DecodedImage img;
  img.width = k_dim;
  img.height = k_dim;
  img.format = k_working_rgba32f;
  std::vector<float> f;
  for (int y = 0; y < k_dim; ++y) {
    for (int x = 0; x < k_dim; ++x) {
      const float chk = ((x + y) % 2 == 0) ? 0.875F : 0.125F;
      f.insert(f.end(),
               {chk, static_cast<float>(x) * 0.03125F, static_cast<float>(y) * 0.03125F, 1.0F});
    }
  }
  img.bytes.resize(f.size() * sizeof(float));
  std::memcpy(img.bytes.data(), f.data(), img.bytes.size());
  return img;
}

// A content with no working grid of its own: the shape of every kind that keeps the null
// default, and of a REFERENCED image whose pixels are its source file's.
class SourceLimited final : public Content {
public:
  std::optional<Rect> bounds() const override { return Rect{0.0, 0.0, 8.0, 8.0}; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    return RenderResult{request.scale, true, std::nullopt};
  }
};

// A CPU-backed rgba32f surface, the same shape `raster_goldens.t.cpp` renders into.
class MemSurface final : public Surface {
public:
  MemSurface(int w, int h)
      : d_w(w), d_h(h), d_bytes(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) *
                                    bytes_per_pixel(k_working_rgba32f.pixel_format),
                                std::byte{0}) {}

  int width() const override { return d_w; }
  int height() const override { return d_h; }
  SurfaceFormat format() const override { return k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }

private:
  int d_w;
  int d_h;
  std::vector<std::byte> d_bytes;
};

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

// Render `region` of `content` at `scale` into a `dim x dim` rgba32f target.
std::vector<std::byte> render_bytes(RasterContent& content, int dim, const Rect& region,
                                    double scale) {
  MemSurface target(dim, dim);
  RenderRequest req{region,           scale,           Time::zero(), StateHandle{}, target,
                    Exactness::Exact, Deadline::none()};
  const std::optional<RenderResult> r = content.render(req, std::make_shared<RenderCompletion>());
  REQUIRE(r.has_value());
  return bytes_of(target);
}

} // namespace

// enforces: 03-layer-plugin-interface#resample-facet-is-discovered-not-allowlisted
TEST_CASE("a content that owns its pixels advertises the resample facet; one that does not, does "
          "not") {
  // The discovery half is half the value (issue #31, the `Registry::insert_schema` pattern): a
  // host reads `resampleable() == nullptr` as "source-limited", greys its "resample to crisp"
  // action and gives an honest reason -- without naming a single kind.
  RasterContent painted(checker(), k_edge);
  REQUIRE(painted.resampleable() != nullptr);
  CHECK(painted.resampleable()->working_grid() == Resampleable::Grid{k_dim, k_dim});

  SourceLimited referenced;
  CHECK(referenced.resampleable() == nullptr);
}

// enforces: 03-layer-plugin-interface#resample-publishes-one-undoable-version
TEST_CASE("resampling publishes one new content state, damages the new extent, and grows bounds") {
  RasterContent content(checker(), k_edge);
  Model model;

  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const StateHandle before = content.base_handle();

  Resampleable* facet = content.resampleable();
  REQUIRE(facet != nullptr);
  std::optional<Resampleable::Grid> achieved;
  {
    auto txn = model.transact("resample");
    achieved = facet->resample(txn, cid, Resampleable::Grid{k_dim * 2, k_dim * 2});
    REQUIRE(txn.commit().has_value());
  }

  REQUIRE(achieved.has_value());
  CHECK(*achieved == Resampleable::Grid{k_dim * 2, k_dim * 2});
  // ONE new version, published as the content's state -- the same discipline `paint` has, so a
  // host's command is one journal entry, undoable, with the ObjectId preserved.
  CHECK(content.base_handle() != before);
  CHECK(model.current()->content_state(cid) == content.base_handle());
  // The local extent FOLLOWS the grid: a raster's local space is its own pixels, and the host
  // scales the layer transform in the same transaction to keep the cell where the user put it.
  CHECK(content.bounds() == Rect{0.0, 0.0, k_dim * 2.0, k_dim * 2.0});
  CHECK(facet->working_grid() == Resampleable::Grid{k_dim * 2, k_dim * 2});
}

// enforces: 03-layer-plugin-interface#resample-matches-a-render-at-that-density
TEST_CASE("the resampled grid holds exactly what a render at that density produced") {
  // The property that makes the verb safe to offer: resampling to 2x and rendering the result
  // 1:1 gives the same pixels as rendering the ORIGINAL at 2x. One sampling policy, not two --
  // so a resample can never disagree with what the user was already looking at.
  RasterContent content(checker(), k_edge);

  // The camera density the host's detail-floor readout reported: the whole image at 2x.
  const std::vector<std::byte> magnified =
      render_bytes(content, k_dim * 2, Rect{0.0, 0.0, k_dim, k_dim}, 2.0);

  Model model;
  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  {
    auto txn = model.transact("resample");
    REQUIRE(content.resampleable()
                ->resample(txn, cid, Resampleable::Grid{k_dim * 2, k_dim * 2})
                .has_value());
    REQUIRE(txn.commit().has_value());
  }

  // The resampled content rendered 1:1 over its NEW extent.
  const std::vector<std::byte> resampled =
      render_bytes(content, k_dim * 2, Rect{0.0, 0.0, k_dim * 2.0, k_dim * 2.0}, 1.0);
  REQUIRE(resampled.size() == magnified.size());
  CHECK(std::memcmp(resampled.data(), magnified.data(), resampled.size()) == 0);
}

// enforces: 03-layer-plugin-interface#resample-publishes-one-undoable-version
TEST_CASE("a declined resample publishes nothing at all") {
  // An empty or negative target is a value, not a throw and not a half-applied edit: nothing is
  // interned, nothing is published, and the content is exactly what it was.
  RasterContent content(checker(), k_edge);
  Model model;
  ObjectId cid{};
  {
    auto txn = model.transact("add");
    cid = txn.add_content(1);
    REQUIRE(txn.commit().has_value());
  }
  model.drain();
  const StateHandle before = content.base_handle();

  {
    auto txn = model.transact("resample");
    CHECK_FALSE(content.resampleable()->resample(txn, cid, Resampleable::Grid{0, 8}).has_value());
    CHECK_FALSE(content.resampleable()->resample(txn, cid, Resampleable::Grid{8, -1}).has_value());
    REQUIRE(txn.commit().has_value());
  }

  CHECK(content.base_handle() == before);
  CHECK(content.bounds() == Rect{0.0, 0.0, k_dim, k_dim});
}
