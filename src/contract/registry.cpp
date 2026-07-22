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
                                                      KindMetadata metadata,
                                                      std::optional<KindCodec> codec,
                                                      std::optional<KindBinder> binder,
                                                      std::optional<KindStateWalker> state_walker) {
  if (id.empty()) {
    return unexpected<RegistryError>(RegistryError::EmptyId);
  }
  if (find(id) != nullptr) {
    // The whole entry is rejected: a duplicate add decorates nothing -- the earlier
    // registration keeps its factory, codec, binder and state walker intact.
    return unexpected<RegistryError>(RegistryError::DuplicateId);
  }
  d_entries.push_back(Entry{std::string(id), std::move(factory), std::move(metadata),
                            std::move(codec), std::move(binder), std::move(state_walker)});
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

const KindCodec* Registry::codec(std::string_view id) const {
  const Entry* entry = find(id);
  return entry != nullptr && entry->codec.has_value() ? &*entry->codec : nullptr;
}

const KindBinder* Registry::binder(std::string_view id) const {
  const Entry* entry = find(id);
  return entry != nullptr && entry->binder.has_value() ? &*entry->binder : nullptr;
}

const KindStateWalker* Registry::state_walker(std::string_view id) const {
  const Entry* entry = find(id);
  return entry != nullptr && entry->state_walker.has_value() ? &*entry->state_walker : nullptr;
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
