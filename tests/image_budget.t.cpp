// The byte budget over org.arbc.image's decoded pyramids (kinds.image_master_budget), and the
// two properties that make it more than an accounting exercise:
//
//   * It genuinely FREES. The cache owns its pyramids strongly, so evicting an entry drops
//     ~512 MB of a 24 MP photograph rather than dropping a `weak_ptr` that never held anything.
//     Ownership is the task; the LRU is the easy part.
//   * It is INVISIBLE. An evicted image keeps its bounds, renders byte-identical pixels off a
//     re-decode, bumps no revision and emits no damage -- so composed tiles keyed on
//     (content id, revision, ...) stay valid across it (doc 02:257). Residency is not
//     composition state.
//
// Everything here is a BEHAVIORAL COUNTER (doc 16:54-62): `decodes_issued()`, `evictions()`,
// `resident_bytes()`. Never a wall-clock assertion, never "it felt like it evicted".
//
// The budgets below are expressed in whole pyramids (`fix::pyramid_bytes()`), so the fixture can
// change size without turning these into magic numbers.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/pixel_traits.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/serialize/load_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

using arbc::image::ImageContent;
using arbc::image::Pyramid;
using arbc::image::PyramidCache;
using arbc::image::PyramidPin;
using arbc::image::PyramidPtr;

namespace {

std::span<const unsigned char> as_bytes(const std::string& s) {
  return {reinterpret_cast<const unsigned char*>(s.data()), s.size()};
}

// One decoded fixture pyramid's resident bytes -- the unit every budget below is expressed in.
std::size_t pyramid_bytes() {
  static const std::size_t bytes = fix::decode_fixture()->resident_bytes();
  return bytes;
}

// Admit `uri` into `cache` and drop the returned hold, leaving the cache the only owner. The
// shape a load has: the factory resolves, the content reads the extent, the hold dies.
void admit(PyramidCache& cache, const std::string& uri, const std::string& bytes) {
  const PyramidPtr admitted = cache.resolve(uri, as_bytes(bytes));
  REQUIRE(admitted != nullptr);
}

// Render one fixed tile of `content` and return the pixels it PROVIDED.
std::vector<float> render_tile(ImageContent& content, Backend& backend) {
  constexpr int k_edge = 48;
  auto target = backend.make_surface(k_edge, k_edge, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{Rect{64.0, 96.0, 112.0, 144.0},
                              1.0,
                              Time::zero(),
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  const std::optional<RenderResult> r = content.render(request, done);
  REQUIRE(r.has_value());
  REQUIRE(r->provided.has_value());
  const std::span<const float> px = r->provided->surface().span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

Registry image_registry() {
  Registry registry;
  REQUIRE(registry
              .add(
                  ImageContent::kind_id,
                  [](ContentConfig config) { return arbc::image::make_image_content(config); },
                  KindMetadata{"Image", "1"})
              .has_value());
  return registry;
}

std::string image_doc(const std::string& source) {
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,384,320],"layers":[)"
         R"({"kind":"org.arbc.image","kind_version":"1","params":{"source":")" +
         source + R"("}}]}})";
}

// An `AssetSource` that answers INLINE from an in-memory table -- no file I/O, so the document
// below loads with pixels and the only thing under test is what the cache does with them.
class InlineAssetSource final : public AssetSource {
public:
  void put(std::string uri, std::string bytes) { d_files.insert_or_assign(uri, std::move(bytes)); }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    const auto it = d_files.find(std::string(resolved_uri));
    on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
  }

private:
  std::unordered_map<std::string, std::string> d_files;
};

// Records every `flush` a commit makes, so "eviction emitted NO damage" is a checkable statement
// and not a hope (`image_async_pending.t.cpp:135-142`).
class RecordingDamageSink final : public DamageSink {
public:
  void flush(const std::vector<Damage>& damage) override { d_flushes.push_back(damage); }
  const std::vector<std::vector<Damage>>& flushes() const noexcept { return d_flushes; }

private:
  std::vector<std::vector<Damage>> d_flushes;
};

} // namespace

// enforces: 15-memory-model#image-pyramid-evicts-under-byte-budget
TEST_CASE("a third pyramid past a two-pyramid budget evicts the least-recently-pinned one") {
  const std::string bytes = fix::fixture_bytes();
  const std::size_t budget = 2 * pyramid_bytes() + pyramid_bytes() / 2; // admits exactly two
  PyramidCache cache(budget);
  CHECK(cache.budget() == budget);

  admit(cache, "assets/a.ppm", bytes);
  admit(cache, "assets/b.ppm", bytes);
  CHECK(cache.decodes_issued() == 2);
  CHECK(cache.evictions() == 0);
  CHECK(cache.resident_bytes() == 2 * pyramid_bytes());

  // The third does not fit, so the LRU victim goes -- and it goes for REAL: the cache held the
  // only strong reference, so `resident_bytes()` drops by a whole pyramid.
  admit(cache, "assets/c.ppm", bytes);
  CHECK(cache.decodes_issued() == 3);
  CHECK(cache.evictions() == 1);
  CHECK(cache.resident_bytes() <= cache.budget());
  CHECK(cache.resident_bytes() == 2 * pyramid_bytes());
}

// enforces: 15-memory-model#image-pyramid-evicts-under-byte-budget
// enforces: 03-layer-plugin-interface#image-decodes-once-per-resolved-uri
TEST_CASE(
    "re-pulling an evicted image costs EXACTLY ONE decode; re-pulling a resident one costs ZERO") {
  const std::string bytes = fix::fixture_bytes();
  PyramidCache cache(2 * pyramid_bytes() + pyramid_bytes() / 2);

  admit(cache, "assets/a.ppm", bytes); // a is the LRU victim below
  admit(cache, "assets/b.ppm", bytes);
  admit(cache, "assets/c.ppm", bytes);
  REQUIRE(cache.evictions() == 1);
  const std::uint64_t after_evict = cache.decodes_issued();
  REQUIRE(after_evict == 3);

  // The evicted one: EXACTLY ONE further decode, rebuilt from the RETAINED encoded bytes (the
  // plugin performs no file I/O, so bytes it did not keep are bytes it could never get back).
  {
    const PyramidPin re_pulled = cache.pin("assets/a.ppm");
    REQUIRE(re_pulled);
    CHECK(cache.decodes_issued() == after_evict + 1);
  }

  // A still-resident one: ZERO. This is the half of `#image-decodes-once-per-resolved-uri` that
  // survives the budget intact -- "no further decodes WHILE THE PYRAMID REMAINS RESIDENT".
  const std::uint64_t after_re_pull = cache.decodes_issued();
  {
    const PyramidPin resident = cache.pin("assets/c.ppm");
    REQUIRE(resident);
    CHECK(cache.decodes_issued() == after_re_pull);
  }
  // ...and again, twice over, to make "zero" mean zero rather than "one, memoized".
  {
    const PyramidPin resident = cache.pin("assets/c.ppm");
    REQUIRE(resident);
    CHECK(cache.decodes_issued() == after_re_pull);
  }
}

// enforces: 15-memory-model#image-pyramid-evicts-under-byte-budget
TEST_CASE("recency is by PIN, not by insert order") {
  const std::string bytes = fix::fixture_bytes();
  PyramidCache cache(2 * pyramid_bytes() + pyramid_bytes() / 2);

  admit(cache, "assets/a.ppm", bytes);
  admit(cache, "assets/b.ppm", bytes);
  // Pin A after B was admitted: by INSERT order A is oldest, by PIN order it is now the newest.
  // An LRU over inserts would evict A here; an LRU over what renders actually READ evicts B.
  {
    const PyramidPin touch = cache.pin("assets/a.ppm");
    REQUIRE(touch);
  }
  REQUIRE(cache.decodes_issued() == 2); // the pin was a hit -- it changed recency, not residency
  REQUIRE(cache.evictions() == 0);

  admit(cache, "assets/c.ppm", bytes);
  CHECK(cache.evictions() == 1);

  // B is gone (its next pull re-decodes); A is not (its next pull is free).
  const std::uint64_t before = cache.decodes_issued();
  {
    const PyramidPin a = cache.pin("assets/a.ppm");
    REQUIRE(a);
  }
  CHECK(cache.decodes_issued() == before); // A survived: the victim was chosen by PIN recency
  {
    const PyramidPin b = cache.pin("assets/b.ppm");
    REQUIRE(b);
  }
  CHECK(cache.decodes_issued() == before + 1); // B was the victim
}

// enforces: 15-memory-model#image-pyramid-evicts-under-byte-budget
TEST_CASE("a pyramid larger than the entire budget stays resident and renders (soft budget)") {
  // Doc 02:278-284: "the byte budget is a SOFT TARGET, not a hard allocator cap ... the pinned
  // working set is never dropped to honor the budget -- correctness outranks the budget -- so
  // resident bytes may transiently exceed the budget when the pinned set alone does." Mirrors
  // `Journal`, which never trims below one entry (`journal.hpp:104`). A cache that answered "I
  // cannot afford this photograph" would be a cache that cannot open the document.
  CpuBackend backend;
  PyramidCache cache(1); // one byte: no pyramid ever fits
  REQUIRE(pyramid_bytes() > cache.budget());

  auto content = fix::make_cached_content(cache, "budget/oversized.ppm");
  REQUIRE(content->available());

  const std::uint64_t before_evictions = cache.evictions();
  const PyramidPin pin = content->pixels();
  REQUIRE(pin);
  // Resident, pinned, and OVER budget -- the soft overshoot, exactly as designed.
  CHECK(cache.resident_bytes() == pyramid_bytes());
  CHECK(cache.resident_bytes() > cache.budget());
  CHECK(cache.evictions() == before_evictions); // never a victim while pinned (doc 02:268-277)

  // ...and it renders. The whole point: the budget bounds memory, it does not refuse pixels.
  const std::vector<float> got = render_tile(*content, backend);
  auto reference = fix::make_content();
  CHECK(got == render_tile(*reference, backend));
}

// enforces: 15-memory-model#image-pyramid-evicts-under-byte-budget
TEST_CASE("an entry evicted while pinned keeps its pixels readable and byte-correct") {
  // The memory-safety rule of the whole task. The pin is an OWNING `shared_ptr`, not a bare
  // index, so it is strictly stronger than `KeyedStore`'s `CacheHold`: even after the cache has
  // dropped the entry, the pixels a render is mid-way through reading survive until that render
  // finishes. This is what lets eviction run on one thread while renders run on others.
  const std::string bytes = fix::fixture_bytes();
  PyramidCache cache(2 * pyramid_bytes() + pyramid_bytes() / 2);
  admit(cache, "assets/a.ppm", bytes);

  const PyramidPin held = cache.pin("assets/a.ppm");
  REQUIRE(held);
  const Pyramid* pixels = held.get();
  const WorkingPixel corner = pixels->pixel(0, 7, 11);

  // Force eviction PAST the pinned entry: two more pyramids, a budget that admits two total.
  // The pinned entry cannot be the victim, so the budget overshoots rather than dropping it.
  admit(cache, "assets/b.ppm", bytes);
  admit(cache, "assets/c.ppm", bytes);
  CHECK(cache.evictions() >= 1);

  // The pin still reads the SAME object, and it reads the SAME pixels: no use-after-free (ASan
  // would catch that) and no torn value (this would catch that).
  CHECK(held.get() == pixels);
  CHECK(held->pixel(0, 7, 11) == corner);
  CHECK(held->width() == fix::k_width);
  CHECK(held->height() == fix::k_height);
}

// enforces: 15-memory-model#image-pyramid-evicts-under-byte-budget
// enforces: 08-serialization#unavailable-asset-is-not-a-read-error
TEST_CASE("bounds() of an EVICTED image is unchanged and non-empty, and decodes nothing") {
  // The vanishing-layer regression, and the reason `bounds()` had to stop reading the pyramid.
  // `bounds()` is on the compositor's CULL path: an evicted image that reported empty bounds
  // would cull ITSELF out of the composition, and a photograph would disappear because memory
  // got tight -- the worst possible failure mode of a memory policy.
  CpuBackend backend;
  PyramidCache cache(1); // every unpinned pyramid is evicted the moment nothing reads it
  auto content = fix::make_cached_content(cache, "budget/bounds.ppm");

  REQUIRE(cache.resident_bytes() == 0); // genuinely evicted, not merely "over budget"
  CHECK(cache.evictions() >= 1);

  const std::uint64_t before = cache.decodes_issued();
  CHECK(content->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});
  CHECK(content->available()); // EVICTED is not UNAVAILABLE: it has an extent and it has pixels
  CHECK(cache.decodes_issued() == before); // ...and reading the extent took no pin and no decode

  // The two states are DISTINGUISHABLE, which is the sharp edge of
  // `#unavailable-asset-is-not-a-read-error`: only a genuinely unavailable asset -- no bytes,
  // or undecodable ones -- has empty bounds and renders nothing.
  const auto missing = fix::make_unavailable_content();
  CHECK_FALSE(missing->available());
  CHECK(missing->bounds()->empty());

  // ...and the evicted one still renders, byte-identically, off a re-decode.
  auto reference = fix::make_content();
  CHECK(render_tile(*content, backend) == render_tile(*reference, backend));
  CHECK(cache.decodes_issued() > before); // it DID have to rebuild: this was not a stale hit
}

// enforces: 08-serialization#unavailable-asset-is-not-a-read-error
TEST_CASE("an owning cache never answers ABSENCE with someone else's decode") {
  // The regression the ownership inversion introduced, and the sharpest edge in the task. Entries
  // used to be `weak_ptr`, so a cache entry died with the last content holding it; now the cache
  // OWNS them and they live forever. That makes a hit possible where one was previously
  // impossible: a PENDING content, minted with EMPTY bytes because its source has not answered,
  // whose resolved URI another document already decoded.
  //
  // Serving it would make a still-downloading photograph render the picture -- and collapse doc
  // 08 Principle 3's three load states into "did some other layer open this file first". Empty
  // bytes are ABSENCE (`load_context.hpp:35-38`), and absence is answered from nowhere.
  const std::string bytes = fix::fixture_bytes();
  PyramidCache cache;
  admit(cache, "shared/photo.ppm", bytes);
  REQUIRE(cache.resident_bytes() > 0); // resident, owned, and hit-able by anyone asking WITH bytes

  const std::uint64_t before = cache.decodes_issued();
  CHECK(cache.resolve("shared/photo.ppm", as_bytes(std::string{})) == nullptr);
  CHECK(cache.decodes_issued() == before); // and it did not decode anything to say so

  // ...so a pending content over that very URI is still PENDING: empty bounds, no pixels, exactly
  // as if the file had never been decoded by anyone.
  const auto pending = std::make_unique<ImageContent>(
      std::string("assets/photo.ppm"), std::string("shared/photo.ppm"), PyramidPtr{}, cache);
  CHECK_FALSE(pending->available());
  CHECK(pending->bounds()->empty());
  CHECK(pending->pyramid() == nullptr);

  // ...and its arrival, when it comes, installs normally and shares the resident decode.
  CHECK(pending->install_asset(bytes));
  CHECK(pending->available());
  CHECK(pending->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});
  CHECK(cache.decodes_issued() == before); // the dedup held: it shared the pyramid already there
}

// enforces: 03-layer-plugin-interface#image-re-decode-is-byte-identical-and-model-invisible
TEST_CASE("eviction and re-decode are model-invisible: no revision bump, no damage") {
  // Doc 02:257 keys a composed tile on (content id, revision, scale rung, tile coords). Eviction
  // changes NOTHING that key names: the pixels are identical (a decode is a pure function of the
  // retained bytes over media's frozen kernels) and the bounds are unchanged. So a revision bump
  // here would orphan every cached tile in the document to announce that some pixels moved from
  // RAM to recomputable -- the exact pathology `model.per_object_revision` was created to fix.
  //
  // RESIDENCY IS NOT COMPOSITION STATE. That sentence is the claim.
  CpuBackend backend;
  InlineAssetSource source;
  source.put("budget/model/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc("photo.ppm"), doc, bridge, registry, "budget/model/project.arbc",
                        &source)
              .has_value());

  ImageContent* image = nullptr;
  doc.for_each_content([&image](ObjectId, Content* c) {
    if (auto* const found = dynamic_cast<ImageContent*>(c); found != nullptr) {
      image = found;
    }
  });
  REQUIRE(image != nullptr);
  REQUIRE(image->available());

  const std::vector<float> resident = render_tile(*image, backend);
  const std::uint64_t revision = doc.pin()->revision();
  const std::optional<Rect> bounds = image->bounds();

  // Squeeze the process-wide cache the loaded document resolved through down to nothing, then
  // render again: every pull now evicts and every render re-decodes.
  PyramidCache& cache = arbc::image::default_pyramid_cache();
  const std::size_t restore = cache.budget();
  RecordingDamageSink sink;
  doc.set_damage_sink(&sink); // installed AFTER the load: measure only the eviction

  cache.set_byte_budget(1);
  const std::uint64_t before = cache.decodes_issued();
  const std::vector<float> after_eviction = render_tile(*image, backend);
  const std::vector<float> again = render_tile(*image, backend);

  doc.set_damage_sink(nullptr);
  cache.set_byte_budget(restore);

  // It really did evict and re-decode -- twice, once per render.
  CHECK(cache.decodes_issued() == before + 2);
  // BYTE-IDENTICAL pixels: a re-decode is a pure function of the retained encoded bytes.
  CHECK(after_eviction == resident);
  CHECK(again == resident);
  // ...and nothing the model can see moved.
  CHECK(doc.pin()->revision() == revision); // NO revision bump
  CHECK(sink.flushes().empty());            // NO damage
  CHECK(image->bounds() == bounds);         // NO geometry change
  CHECK(image->available());
}
