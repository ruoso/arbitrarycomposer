// The pure set-subtraction at the heart of the sweep (serialize.asset_gc Decision 3): present
// on disk minus referenced by any preserved document. No I/O, no filesystem, no raster type --
// so it is unit-testable at L4 in isolation from the runtime driver that feeds it.

#include <arbc/serialize/asset_reaper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace arbc;

namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

} // namespace

TEST_CASE("unreferenced_tiles returns exactly present minus referenced") {
  const std::vector<std::string> present = {"aa", "bb", "cc", "dd"};
  const std::unordered_set<std::string> referenced = {"bb", "dd"};

  const std::vector<std::string> dead = unreferenced_tiles(referenced, present);

  CHECK(dead.size() == 2);
  CHECK(contains(dead, "aa"));
  CHECK(contains(dead, "cc"));
  CHECK_FALSE(contains(dead, "bb")); // referenced: retained
  CHECK_FALSE(contains(dead, "dd"));
}

TEST_CASE("a referenced hash absent from disk matches nothing and is harmless") {
  const std::vector<std::string> present = {"aa", "bb"};
  // "zz" is referenced by a document but not on disk (e.g. another project's blob).
  const std::unordered_set<std::string> referenced = {"aa", "zz"};

  const std::vector<std::string> dead = unreferenced_tiles(referenced, present);

  CHECK(dead.size() == 1);
  CHECK(contains(dead, "bb")); // only the on-disk orphan; "zz" was never a candidate
}

TEST_CASE("nothing referenced sweeps everything; everything referenced sweeps nothing") {
  const std::vector<std::string> present = {"aa", "bb", "cc"};

  CHECK(unreferenced_tiles({}, present).size() == 3);
  CHECK(unreferenced_tiles({"aa", "bb", "cc"}, present).empty());
}

TEST_CASE("the delete list preserves the present order (deterministic reporting)") {
  const std::vector<std::string> present = {"dd", "aa", "cc", "bb"};
  const std::unordered_set<std::string> referenced = {"cc"};

  const std::vector<std::string> dead = unreferenced_tiles(referenced, present);

  REQUIRE(dead.size() == 3);
  CHECK(dead[0] == "dd");
  CHECK(dead[1] == "aa");
  CHECK(dead[2] == "bb");
}

TEST_CASE("an empty on-disk set yields an empty delete list") {
  CHECK(unreferenced_tiles({"aa"}, std::vector<std::string>{}).empty());
}

TEST_CASE("unreferenced_assets roots an authored URI by key, by tail, and by basename") {
  // Issue #30. The named-asset key space is not the tile one: a tile hash is a canonical name
  // that either matches or does not, while the SAME owned image is `assets/images/x.png` to a
  // document at the project root and `../proj/assets/images/x.png` to one beside it. All three
  // rules exist to keep a live reference from reading as an orphan -- the failure this half
  // must never have, because its consequence is data loss rather than a leak.
  const std::vector<std::string> present = {"assets/images/kept.png", "assets/images/tail.png",
                                            "assets/images/base.png", "assets/images/orphan.png"};
  const std::unordered_set<std::string> referenced = {
      "assets/images/kept.png",          // rule 1: the exact authored key
      "../other/assets/images/tail.png", // rule 2: the same tail, longer prefix
      "/home/u/pictures/base.png",       // rule 3: a shared basename, elsewhere entirely
  };

  const std::vector<std::string> dead = unreferenced_assets(referenced, present);

  REQUIRE(dead.size() == 1);
  CHECK(dead[0] == "assets/images/orphan.png"); // referenced by nothing, under any rule
}

TEST_CASE("unreferenced_assets sweeps everything when nothing is referenced") {
  const std::vector<std::string> present = {"assets/images/a.png", "assets/images/b.png"};
  CHECK(unreferenced_assets({}, present).size() == 2);
  CHECK(unreferenced_assets({"assets/images/a.png"}, present).size() == 1);
  CHECK(unreferenced_assets({"assets/images/a.png"}, std::vector<std::string>{}).empty());
}

TEST_CASE("a tiles-only reaper holds no named assets and is never asked to remove one") {
  // The defaults (issue #30): a reaper written before the named-asset half compiles unchanged
  // and reclaims nothing new. Size and remove answer with an ERROR rather than a fabricated
  // success, so a store that lists named assets but cannot act on them fails the sweep --
  // which deletes nothing -- instead of reporting a reclamation it never performed.
  class TilesOnly final : public AssetReaper {
  public:
    expected<std::vector<std::string>, AssetReaperError> list_tile_hashes() const override {
      return std::vector<std::string>{};
    }
    expected<std::uint64_t, AssetReaperError> tile_size(std::string_view) const override {
      return 0U;
    }
    expected<bool, AssetReaperError> remove_tile(std::string_view) override { return false; }
  };

  TilesOnly reaper;
  const expected<std::vector<std::string>, AssetReaperError> listed = reaper.list_asset_uris();
  REQUIRE(listed.has_value());
  CHECK(listed->empty());
  CHECK_FALSE(reaper.asset_size("assets/images/x.png").has_value());
  CHECK_FALSE(reaper.remove_asset("assets/images/x.png").has_value());
}

TEST_CASE("AssetReaperError compares by kind") {
  CHECK(AssetReaperError{AssetReaperError::Kind::EnumerateFailed} ==
        AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  CHECK_FALSE(AssetReaperError{AssetReaperError::Kind::EnumerateFailed} ==
              AssetReaperError{AssetReaperError::Kind::RemoveFailed});
}
