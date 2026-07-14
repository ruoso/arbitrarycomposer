#include <arbc/runtime/filesystem_asset_sink.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace arbc {
namespace {

// The read side strips exactly this and takes everything else as a plain path
// (`filesystem_asset_source.cpp`). The two must agree, or a blob written under one
// spelling is unreadable under the other.
std::string_view strip_file_scheme(std::string_view uri) {
  constexpr std::string_view k_file = "file://";
  return uri.starts_with(k_file) ? uri.substr(k_file.size()) : uri;
}

// A temporary name in the TARGET directory -- it must be on the same filesystem as the
// final path, or `rename` is not atomic (and may not even succeed). Unique per thread and
// per call, so two concurrent saves of two different documents into one shared asset
// directory never scribble on each other's partial file.
std::filesystem::path temp_path(const std::filesystem::path& target) {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
  const std::size_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
  std::string name = ".arbc-tmp-";
  name += std::to_string(tid);
  name += '-';
  name += std::to_string(n);
  return target.parent_path() / name;
}

unexpected<AssetSinkError> write_failed() {
  return unexpected(AssetSinkError{AssetSinkError::Kind::WriteFailed});
}

} // namespace

bool FilesystemAssetSink::contains(std::string_view resolved_uri) const {
  std::error_code ec;
  const std::filesystem::path path{strip_file_scheme(resolved_uri)};
  return std::filesystem::exists(path, ec) && !ec;
}

expected<bool, AssetSinkError> FilesystemAssetSink::put(std::string_view resolved_uri,
                                                        std::span<const std::byte> bytes) {
  ++d_puts;

  // Errors are VALUES all the way down (doc 10): the non-throwing `std::filesystem`
  // overloads, an `ifstream`/`ofstream` whose failbit is never armed, and an
  // `AssetSinkError` on every failure. No exception escapes into the writer.
  const std::filesystem::path path{strip_file_scheme(resolved_uri)};

  // WRITE-IF-ABSENT: the store is content-addressed, so the name IS the content, and a
  // name already on disk needs nothing done to it.
  std::error_code ec;
  if (std::filesystem::exists(path, ec) && !ec) {
    return false;
  }

  // The two-hex fan-out means a save is routinely the first thing to touch `tiles/3f/`.
  // `create_directories` on an existing directory is not an error; a genuine failure is.
  const std::filesystem::path parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (!std::filesystem::is_directory(parent, ec) || ec) {
      return write_failed();
    }
  }

  // TEMP + RENAME (Constraint 6). A truncated blob under a valid hash name would poison
  // every future save's write-if-absent check -- it would be skipped forever, and the
  // user's pixels would be silently gone. So the bytes become durable under a name nobody
  // will ever look for, and only then does the name appear.
  const std::filesystem::path tmp = temp_path(path);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return write_failed();
    }
    if (!bytes.empty()) {
      out.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(tmp, ec); // do not leave the partial behind
      return write_failed();
    }
  }

  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    // Lost a race against a concurrent saver that landed the same blob first? Then the
    // name is present and correct, which is exactly what we wanted -- content-addressing
    // makes a duplicate write benign. Anything else is a real failure.
    std::error_code exists_ec;
    const bool landed = std::filesystem::exists(path, exists_ec) && !exists_ec;
    std::filesystem::remove(tmp, ec);
    if (landed) {
      return false;
    }
    return write_failed();
  }

  ++d_written;
  d_bytes += static_cast<std::uint64_t>(bytes.size());
  return true;
}

} // namespace arbc
