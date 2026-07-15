#include <arbc/serialize/asset_reaper.hpp>

namespace arbc {

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

} // namespace arbc
