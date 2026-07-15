// The pure set-subtraction at the heart of the sweep (serialize.asset_gc Decision 3): present
// on disk minus referenced by any preserved document. No I/O, no filesystem, no raster type --
// so it is unit-testable at L4 in isolation from the runtime driver that feeds it.

#include <arbc/serialize/asset_reaper.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
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

TEST_CASE("AssetReaperError compares by kind") {
  CHECK(AssetReaperError{AssetReaperError::Kind::EnumerateFailed} ==
        AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
  CHECK_FALSE(AssetReaperError{AssetReaperError::Kind::EnumerateFailed} ==
              AssetReaperError{AssetReaperError::Kind::RemoveFailed});
}
