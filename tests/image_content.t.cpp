// The plugin-local unit surface of org.arbc.image: the `ContentConfig` framing the core hands
// the factory, the immutable pyramid, and the resolved-URI decode cache.
//
// These sit below the document-level assertions in `image_serialize.t.cpp` (which drives the
// same code through a real load) and pin the seams that a load exercises only in passing --
// the framing's failure modes, the pyramid's rung structure, and what the cache does with the
// pyramids it OWNS. The BUDGET over that ownership -- LRU eviction, the residency pin, the
// re-decode -- is `image_budget.t.cpp`; the cache here is deliberately unbudgeted.

#include <arbc/base/expected.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_image/image_content.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <memory>
#include <span>
#include <string>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

std::span<const unsigned char> as_bytes(const std::string& s) {
  return {reinterpret_cast<const unsigned char*>(s.data()), s.size()};
}

} // namespace

TEST_CASE("the ContentConfig frame carries the authored URI, the resolved URI, and the bytes") {
  // The frame is "<authored>\n<resolved>\n<encoded-bytes>", split at the first two newlines
  // (Decision 5). Both URIs ride it because they answer different questions: the AUTHORED one
  // is what the content reads back for `params.source`, the RESOLVED one is the identity the
  // pyramid cache dedups the decode on. A URI cannot contain a raw newline and `normalize_uri`
  // is purely lexical, so the framing is unambiguous even though the payload is binary.
  const std::string config =
      arbc::image::image_config("assets/bg.ppm", "/proj/assets/bg.ppm", fix::fixture_bytes());

  const expected<std::unique_ptr<Content>, std::string> built =
      arbc::image::make_image_content(config);
  REQUIRE(built.has_value());
  const auto* image = dynamic_cast<const arbc::image::ImageContent*>(built->get());
  REQUIRE(image != nullptr);

  // The AUTHORED spelling is what the content keeps -- never the resolved one, which is what
  // makes a project directory relocatable (Constraint 5).
  CHECK(image->external_asset_ref() == "assets/bg.ppm");
  CHECK(image->available());
  CHECK(image->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});
}

TEST_CASE("empty bytes in the frame mean UNAVAILABLE, not an error") {
  // Empty bytes == absence (`load_context.hpp:35-38`), and absence is a perfectly ordinary
  // content: the URI is kept, there are no pixels, and the parent document still loads
  // (Constraint 6).
  //
  // The FRAME still spells absence as empty bytes, and a PENDING image is minted in this very
  // shape -- but which of the two it is, is a fact about the LOAD (did the source answer?), not
  // about the frame, and it lives in the core's `PendingExternalLoads` where the kind never sees
  // it (kinds.image_async_pending Decision 1). A pending content gains its pixels through
  // `install_asset`; an unavailable one never does.
  const expected<std::unique_ptr<Content>, std::string> built =
      arbc::image::make_image_content(arbc::image::image_config("assets/bg.ppm", "/p/bg.ppm", ""));
  REQUIRE(built.has_value());
  const auto* image = dynamic_cast<const arbc::image::ImageContent*>(built->get());
  REQUIRE(image != nullptr);
  CHECK_FALSE(image->available());
  CHECK(image->external_asset_ref() == "assets/bg.ppm");
  CHECK(image->bounds()->empty());
}

TEST_CASE("a malformed ContentConfig frame is an error VALUE, never a throw") {
  // A frame with neither delimiter, and one with only the first: a caller bug (the core always
  // builds a well-formed frame), never user data -- but it crosses the plugin boundary, so it
  // must be a value (doc 03:177-180). Distinct from an unavailable asset, which IS user data
  // and is accepted.
  CHECK_FALSE(arbc::image::make_image_content("assets/bg.ppm").has_value());
  CHECK_FALSE(arbc::image::make_image_content("assets/bg.ppm\n/proj/bg.ppm").has_value());
}

TEST_CASE("the pyramid is a half-band chain from the master down to a single pixel") {
  const arbc::image::PyramidPtr pyramid = fix::decode_fixture();
  REQUIRE(pyramid != nullptr);
  CHECK(pyramid->width() == fix::k_width);
  CHECK(pyramid->height() == fix::k_height);

  // 384x320 -> 192x160 -> 96x80 -> 48x40 -> 24x20 -> 12x10 -> 6x5 -> 3x3 -> 2x2 -> 1x1.
  // The chain runs to 1x1 (doc 14:219 scale rungs), so every rung a `BestEffort` request can
  // clamp onto exists.
  CHECK(pyramid->level_count() == 10);

  // Resident bytes span every level, not just the master -- the measuring instrument the
  // deferred `kinds.image_master_budget` needs to evict against a byte budget. The mips add
  // strictly to the master and strictly less than it (a geometric 1/4 series).
  const std::size_t master =
      static_cast<std::size_t>(fix::k_width) * fix::k_height * 4U * sizeof(float);
  CHECK(pyramid->resident_bytes() > master);
  CHECK(pyramid->resident_bytes() < 2 * master);

  // Clamp-to-edge, so the resampler's wider tap footprint reads the border rather than a zero
  // surround (which would darken every level edge).
  CHECK(pyramid->pixel(0, -4, -4) == pyramid->pixel(0, 0, 0));
  CHECK(pyramid->pixel(0, fix::k_width + 9, fix::k_height + 9) ==
        pyramid->pixel(0, fix::k_width - 1, fix::k_height - 1));
}

// enforces: 03-layer-plugin-interface#image-decodes-once-per-resolved-uri
TEST_CASE("the pyramid cache OWNS its pyramids, so a re-pull of a resident one decodes nothing") {
  const std::string bytes = fix::fixture_bytes();
  arbc::image::PyramidCache cache; // unbudgeted, as `Journal` is: eviction is opt-IN

  {
    const arbc::image::PyramidPtr first = cache.resolve("assets/bg.ppm", as_bytes(bytes));
    REQUIRE(first != nullptr);
    CHECK(cache.decodes_issued() == 1);

    // A second content against the same RESOLVED identity shares the decode.
    const arbc::image::PyramidPtr second = cache.resolve("assets/bg.ppm", as_bytes(bytes));
    CHECK(second.get() == first.get());
    CHECK(cache.decodes_issued() == 1);

    // A DIFFERENT resolved identity is a different image and decodes on its own.
    const arbc::image::PyramidPtr other = cache.resolve("assets/other.ppm", as_bytes(bytes));
    REQUIRE(other != nullptr);
    CHECK(other.get() != first.get());
    CHECK(cache.decodes_issued() == 2);
  }

  // Every transient hold is gone -- and the pyramids are STILL THERE, because the CACHE owns them
  // now (kinds.image_master_budget Decision 1). This is the inversion that made a byte budget
  // possible: while `ImageContent` held the strong reference and the cache held a `weak_ptr`,
  // evicting a map entry freed ZERO bytes for as long as the layer lived, so an LRU over it would
  // have been a counter with no memory behind it.
  CHECK(cache.resident_bytes() > 0);
  CHECK(cache.evictions() == 0);

  // So a re-pull issues ZERO further decodes -- "re-loading or re-rendering an unchanged image
  // issues no further decodes WHILE THE PYRAMID REMAINS RESIDENT", which is the half of
  // `#image-decodes-once-per-resolved-uri` the budget leaves untouched. Its complement -- an
  // evicted image costs exactly one further decode -- lives in `image_budget.t.cpp`.
  const arbc::image::PyramidPtr again = cache.resolve("assets/bg.ppm", as_bytes(bytes));
  REQUIRE(again != nullptr);
  CHECK(cache.decodes_issued() == 2);

  // An unbudgeted cache never evicts, however many distinct images it holds.
  CHECK(cache.budget() == arbc::image::PyramidCache::k_no_budget);
  CHECK(cache.evictions() == 0);
}

TEST_CASE("an undecodable or unidentified asset caches nothing and decodes nothing") {
  arbc::image::PyramidCache cache;

  // Corrupt bytes are the unavailable state, not an error -- and nothing is remembered, so a
  // later attempt at the same URI (with a file that has since been repaired) still decodes.
  const std::string corrupt = "P6 this is not an image at all";
  CHECK(cache.resolve("assets/bg.ppm", as_bytes(corrupt)) == nullptr);
  CHECK(cache.decodes_issued() == 0);

  // An empty resolved URI names no identity to dedup on (an absent or mistyped
  // `params.source`): decode nothing, cache nothing.
  const std::string bytes = fix::fixture_bytes();
  CHECK(cache.resolve("", as_bytes(bytes)) == nullptr);
  CHECK(cache.decodes_issued() == 0);

  // ...and the URI that failed on corrupt bytes decodes cleanly once the bytes are good.
  REQUIRE(cache.resolve("assets/bg.ppm", as_bytes(bytes)) != nullptr);
  CHECK(cache.decodes_issued() == 1);
}
