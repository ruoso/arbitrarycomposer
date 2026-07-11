#include <arbc/runtime/filesystem_asset_source.hpp>

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace arbc {
namespace {

// v1 resolves relative paths; the scheme hook is the later extension point (doc 08
// Principle 3). `file://` is the one scheme a filesystem source can honestly claim, so it
// is stripped; every other spelling is taken as a plain path. A schemed URI this source
// cannot serve (http, a content store) simply names no file and reads as absent -- which
// the caller reports as unavailable, exactly as a missing file would be.
std::string_view strip_file_scheme(std::string_view uri) {
  constexpr std::string_view k_file = "file://";
  return uri.starts_with(k_file) ? uri.substr(k_file.size()) : uri;
}

} // namespace

void FilesystemAssetSource::request(std::string_view resolved_uri,
                                    std::function<void(std::string_view)> on_ready) {
  ++d_requests;

  // Errors are VALUES all the way down (doc 10): the non-throwing `std::filesystem`
  // overload, an `ifstream` whose failbit is never armed, and empty bytes on every
  // failure. No exception can escape into the loader, which is what
  // `08-serialization#loader-never-faults-on-hostile-input` needs from a source a hostile
  // document can point anywhere.
  const std::filesystem::path path{strip_file_scheme(resolved_uri)};
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    on_ready(std::string_view{}); // absent, a directory, or unstattable: unavailable
    return;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    on_ready(std::string_view{}); // unreadable (permissions, a race with a delete)
    return;
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) {
    // GCOV_EXCL_START -- a hardware/filesystem fault mid-read: real, and not reachable from a
    // test without a syscall fault injector (which `pool.workspace_file` owns, not this).
    on_ready(std::string_view{});
    return;
    // GCOV_EXCL_STOP
  }
  ++d_hits;
  // Inline, before `request` returns: the continuation reads the bytes while `bytes` is
  // still alive, and no copy crosses the seam.
  on_ready(std::string_view(bytes));
}

} // namespace arbc
