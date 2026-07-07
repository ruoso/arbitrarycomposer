#include <arbc/contract/registry.hpp>

#include <utility>

namespace arbc {

const Registry::Entry* Registry::find(std::string_view id) const {
  for (const Entry& entry : d_entries) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

expected<std::monostate, RegistryError> Registry::add(std::string_view id, ContentFactory factory,
                                                      KindMetadata metadata) {
  if (id.empty()) {
    return unexpected<RegistryError>(RegistryError::EmptyId);
  }
  if (find(id) != nullptr) {
    return unexpected<RegistryError>(RegistryError::DuplicateId);
  }
  d_entries.push_back(Entry{std::string(id), std::move(factory), std::move(metadata)});
  return std::monostate{};
}

const ContentFactory* Registry::factory(std::string_view id) const {
  const Entry* entry = find(id);
  return entry != nullptr ? &entry->factory : nullptr;
}

const KindMetadata* Registry::metadata(std::string_view id) const {
  const Entry* entry = find(id);
  return entry != nullptr ? &entry->metadata : nullptr;
}

std::vector<std::string_view> Registry::ids() const {
  std::vector<std::string_view> out;
  out.reserve(d_entries.size());
  for (const Entry& entry : d_entries) {
    out.emplace_back(entry.id);
  }
  return out;
}

} // namespace arbc
