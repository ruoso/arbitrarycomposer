#include <arbc/serialize/asset_reaper.hpp>

#include <string_view>

namespace arbc {
namespace {

// The basename of an authored URI: everything after the last `/`. No `std::filesystem` --
// this component is filesystem-free by design (L4), and a URI's separator is `/` on every
// platform regardless of what the local path separator is.
std::string_view basename_of(std::string_view uri) {
  const std::size_t slash = uri.rfind('/');
  return slash == std::string_view::npos ? uri : uri.substr(slash + 1);
}

} // namespace

std::vector<std::string> unreferenced_tiles(const std::unordered_set<std::string>& referenced,
                                            std::span<const std::string> present) {
  std::vector<std::string> dead;
  for (const std::string& name : present) {
    if (referenced.find(name) == referenced.end()) {
      dead.push_back(name);
    }
  }
  return dead;
}

std::vector<std::string> unreferenced_assets(const std::unordered_set<std::string>& referenced,
                                             std::span<const std::string> present) {
  std::vector<std::string> dead;
  for (const std::string& key : present) {
    bool rooted = referenced.find(key) != referenced.end(); // rule 1: the exact authored key
    if (!rooted) {
      const std::string_view key_base = basename_of(key);
      const std::string tail = "/" + key;
      for (const std::string& ref : referenced) {
        // rule 2: the same tail under a longer prefix; rule 3: a shared basename.
        if (ref.ends_with(tail) || basename_of(ref) == key_base) {
          rooted = true;
          break;
        }
      }
    }
    if (!rooted) {
      dead.push_back(key);
    }
  }
  return dead;
}

// The tiles-only defaults (issue #30): a reaper that predates the named-asset half holds no
// named assets, so it lists none -- and can therefore never be ASKED to size or remove one.
// Those two answer with an error rather than a fabricated success, so a partially-implemented
// store fails the sweep (which then deletes nothing) instead of reporting a phantom
// reclamation.
expected<std::vector<std::string>, AssetReaperError> AssetReaper::list_asset_uris() const {
  return std::vector<std::string>{};
}

expected<std::uint64_t, AssetReaperError> AssetReaper::asset_size(std::string_view) const {
  return unexpected(AssetReaperError{AssetReaperError::Kind::EnumerateFailed});
}

expected<bool, AssetReaperError> AssetReaper::remove_asset(std::string_view) {
  return unexpected(AssetReaperError{AssetReaperError::Kind::RemoveFailed});
}

} // namespace arbc
