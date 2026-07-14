// The tree's first `AssetSink` (serialize.raster_tile_store Decision 1, Constraint 6):
// write-if-absent, crash-atomic, and never deleting.

#include <arbc/runtime/filesystem_asset_sink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

using namespace arbc;

namespace {

class TempDir {
public:
  explicit TempDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_sink_" + name)) {
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

  std::string at(const std::string& rel) const { return (d_root / rel).string(); }
  const std::filesystem::path& root() const { return d_root; }

private:
  std::filesystem::path d_root;
};

std::vector<std::byte> bytes_of(const std::string& s) {
  return {reinterpret_cast<const std::byte*>(s.data()),
          reinterpret_cast<const std::byte*>(s.data()) + s.size()};
}

std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("the sink writes if absent and skips if present") {
  TempDir dir("write_if_absent");
  FilesystemAssetSink sink;
  const std::string path = dir.at("tiles/3f/3fa91c");

  CHECK_FALSE(sink.contains(path));

  const expected<bool, AssetSinkError> first = sink.put(path, bytes_of("tile-bytes"));
  REQUIRE(first.has_value());
  CHECK(*first); // newly written
  CHECK(sink.blobs_written() == 1);
  CHECK(sink.puts() == 1);
  CHECK(sink.bytes_written() == 10);
  CHECK(sink.contains(path));
  CHECK(read_all(path) == "tile-bytes");

  // The parent directories were created: the 2-hex fan-out means a save is routinely the
  // first thing to touch `tiles/3f/`.
  CHECK(std::filesystem::is_directory(dir.root() / "tiles" / "3f"));

  // The same name again writes NOTHING. Content-addressed: same name => same content, so
  // presence-by-name is sufficient proof the write is unnecessary. This is what makes an
  // incremental save incremental.
  const expected<bool, AssetSinkError> again = sink.put(path, bytes_of("tile-bytes"));
  REQUIRE(again.has_value());
  CHECK_FALSE(*again);
  CHECK(sink.blobs_written() == 1); // unchanged
  CHECK(sink.puts() == 2);          // but it WAS offered: puts - written == the dedup count
}

TEST_CASE("the sink leaves no temporary behind, and never deletes") {
  TempDir dir("atomic");
  FilesystemAssetSink sink;

  REQUIRE(sink.put(dir.at("tiles/aa/aaaa"), bytes_of("one")).has_value());
  REQUIRE(sink.put(dir.at("tiles/bb/bbbb"), bytes_of("two")).has_value());

  // Bytes go to a temp name in the TARGET directory and are renamed into place, so a crash
  // mid-write can never leave a truncated blob under a valid hash name (which would poison
  // every future save's write-if-absent check -- skipped forever, pixels silently gone).
  // Nothing of that machinery may survive a successful write.
  std::size_t files = 0;
  for (const auto& e : std::filesystem::recursive_directory_iterator(dir.root())) {
    if (e.is_regular_file()) {
      ++files;
      CHECK(e.path().filename().string().find("tmp") == std::string::npos);
    }
  }
  CHECK(files == 2);

  // A SAVE NEVER DELETES: writing a new blob leaves every prior one untouched.
  CHECK(read_all(dir.at("tiles/aa/aaaa")) == "one");
  CHECK(read_all(dir.at("tiles/bb/bbbb")) == "two");
}

TEST_CASE("an empty blob is a legal blob") {
  TempDir dir("empty");
  FilesystemAssetSink sink;
  const std::string path = dir.at("tiles/e3/e3b0c4");
  const expected<bool, AssetSinkError> put = sink.put(path, {});
  REQUIRE(put.has_value());
  CHECK(*put);
  CHECK(std::filesystem::exists(path));
  CHECK(sink.bytes_written() == 0);
}

TEST_CASE("an unwritable target is an error value, never a throw") {
  TempDir dir("unwritable");
  FilesystemAssetSink sink;

  // A path whose parent is an existing FILE cannot be created as a directory. Errors are
  // values all the way down (doc 10): no exception may escape into the writer, because the
  // one thing a hostile or broken filesystem must not do is take the process down.
  REQUIRE(sink.put(dir.at("blocker"), bytes_of("x")).has_value());
  const expected<bool, AssetSinkError> blocked =
      sink.put(dir.at("blocker/3f/3fa91c"), bytes_of("y"));
  REQUIRE_FALSE(blocked.has_value());
  CHECK(blocked.error() == AssetSinkError{AssetSinkError::Kind::WriteFailed});
}

TEST_CASE("the sink strips file:// exactly as the source does") {
  TempDir dir("scheme");
  FilesystemAssetSink sink;
  // The read side strips exactly this. The two must agree, or a blob written under one
  // spelling is unreadable under the other.
  const std::string path = dir.at("tiles/cc/cccc");
  REQUIRE(sink.put("file://" + path, bytes_of("z")).has_value());
  CHECK(std::filesystem::exists(path));
  CHECK(sink.contains("file://" + path));
  CHECK(sink.contains(path));
}
