// The concrete filesystem reaper (serialize.asset_gc Decision 2), the exact mirror of
// `FilesystemAssetSink`: it enumerates the resolved `assets/tiles/` subtree, sizes a blob,
// removes a blob, and prunes an emptied fan-out directory -- all with errors as VALUES, never
// a throwing `directory_iterator`. This unit exercises the reaper against a temp dir; the
// document-driven GC claims live in `tests/asset_gc.t.cpp`.

#include <arbc/runtime/asset_gc.hpp>
#include <arbc/serialize/tile_blob.hpp> // tile_blob_uri (place blobs where the reaper looks)

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

using namespace arbc;

namespace {

class TempDir {
public:
  explicit TempDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_reaper_" + name)) {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
    REQUIRE(std::filesystem::create_directories(d_root, ec));
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  // The tiles base the reaper resolves against, with the trailing slash the fan-out expects.
  std::string tiles_base() const { return (d_root / "assets" / "tiles").string() + "/"; }

  // Drop `content` into the fan-out slot the store derives for `hash`.
  void write_blob(const std::string& hash, const std::string& content) const {
    const std::filesystem::path p{std::string(strip(tile_blob_uri(tiles_base(), hash)))};
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
  }

  bool blob_exists(const std::string& hash) const {
    const std::filesystem::path p{std::string(strip(tile_blob_uri(tiles_base(), hash)))};
    return std::filesystem::exists(p);
  }

  const std::filesystem::path& root() const { return d_root; }

private:
  static std::string_view strip(std::string_view uri) {
    constexpr std::string_view k_file = "file://";
    return uri.starts_with(k_file) ? uri.substr(k_file.size()) : uri;
  }
  std::filesystem::path d_root;
};

// A well-formed 32-lowercase-hex blob name whose leading two hex give the fan-out directory.
std::string hash_name(char fill) { return std::string(k_tile_hash_chars, fill); }

} // namespace

TEST_CASE("the reaper lists exactly the valid tile-hash blobs, skipping non-tile names") {
  TempDir dir("list");
  dir.write_blob(hash_name('a'), "one");
  dir.write_blob(hash_name('b'), "two");

  // A non-tile name under tiles/ (an is_tile_hash miss): never listed, never a candidate.
  {
    const std::filesystem::path stray = dir.root() / "assets" / "tiles" / "aa" / "not-a-hash.png";
    std::error_code ec;
    std::filesystem::create_directories(stray.parent_path(), ec);
    std::ofstream(stray) << "x";
  }

  FilesystemAssetReaper reaper(dir.tiles_base());
  const expected<std::vector<std::string>, AssetReaperError> listed = reaper.list_tile_hashes();
  REQUIRE(listed.has_value());
  CHECK(listed->size() == 2);
}

TEST_CASE("an absent tiles subtree is an empty store, not an error") {
  TempDir dir("absent"); // never writes a single blob, so assets/tiles/ does not exist
  FilesystemAssetReaper reaper(dir.tiles_base());
  const expected<std::vector<std::string>, AssetReaperError> listed = reaper.list_tile_hashes();
  REQUIRE(listed.has_value());
  CHECK(listed->empty());
}

TEST_CASE("tile_size reports the blob's byte size") {
  TempDir dir("size");
  dir.write_blob(hash_name('c'), "twelve bytes"); // 12 chars

  FilesystemAssetReaper reaper(dir.tiles_base());
  const expected<std::uint64_t, AssetReaperError> size = reaper.tile_size(hash_name('c'));
  REQUIRE(size.has_value());
  CHECK(*size == 12);
}

TEST_CASE("a missing blob's size is an error value, never a throw") {
  TempDir dir("size_missing");
  FilesystemAssetReaper reaper(dir.tiles_base());
  const expected<std::uint64_t, AssetReaperError> size = reaper.tile_size(hash_name('d'));
  REQUIRE_FALSE(size.has_value());
  CHECK(size.error() == AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
}

TEST_CASE("remove_tile deletes the blob and prunes the emptied fan-out directory") {
  TempDir dir("remove");
  dir.write_blob(hash_name('e'), "gone soon");
  REQUIRE(dir.blob_exists(hash_name('e')));

  FilesystemAssetReaper reaper(dir.tiles_base());
  const expected<bool, AssetReaperError> removed = reaper.remove_tile(hash_name('e'));
  REQUIRE(removed.has_value());
  CHECK(*removed);
  CHECK_FALSE(dir.blob_exists(hash_name('e')));

  // The two-hex fan-out directory `ee/` held only that one blob, so it is pruned.
  CHECK_FALSE(std::filesystem::exists(dir.root() / "assets" / "tiles" / "ee"));
}

TEST_CASE("removing an already-absent blob reports false, not an error") {
  TempDir dir("remove_absent");
  FilesystemAssetReaper reaper(dir.tiles_base());
  const expected<bool, AssetReaperError> removed = reaper.remove_tile(hash_name('f'));
  REQUIRE(removed.has_value());
  CHECK_FALSE(*removed); // idempotent: a re-run simply finds it gone
}

TEST_CASE("a shared fan-out directory survives removing one of its blobs") {
  TempDir dir("prune_shared");
  // Two blobs whose names begin `aa`: they share the `aa/` fan-out directory.
  const std::string keep = "aa" + std::string(k_tile_hash_chars - 2, '0');
  const std::string drop = "aa" + std::string(k_tile_hash_chars - 2, '1');
  dir.write_blob(keep, "keep");
  dir.write_blob(drop, "drop");

  FilesystemAssetReaper reaper(dir.tiles_base());
  REQUIRE(reaper.remove_tile(drop).has_value());

  CHECK(dir.blob_exists(keep)); // the sibling keeps the directory alive
  CHECK(std::filesystem::exists(dir.root() / "assets" / "tiles" / "aa"));
}
