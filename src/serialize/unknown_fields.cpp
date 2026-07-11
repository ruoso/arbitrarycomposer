// serialize.unknown_field_preservation: the `UnknownFieldStore` carrier plus the
// residual/merge core behind it (`unknown_json.hpp`). Both live in one TU because the
// store is trivial and the core is the only thing that ever looks inside its bytes.

#include <arbc/serialize/unknown_fields.hpp>
#include <arbc/serialize/unknown_json.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace arbc {

using json = nlohmann::json;

// ---- UnknownFieldStore ------------------------------------------------------

void UnknownFieldStore::set(UnknownScope scope, ObjectId id, UnknownFields fields) {
  const Key key{static_cast<std::uint8_t>(scope), id.value};
  if (fields.empty()) {
    d_entries.erase(key); // "nothing preserved" is the absence of an entry
    return;
  }
  d_entries[key] = std::move(fields);
}

const UnknownFields* UnknownFieldStore::find(UnknownScope scope, ObjectId id) const {
  const auto it = d_entries.find(Key{static_cast<std::uint8_t>(scope), id.value});
  return it != d_entries.end() ? &it->second : nullptr;
}

// ---- The residual/merge core ------------------------------------------------

bool names_key(std::span<const std::string_view> known, std::string_view key) {
  return std::find(known.begin(), known.end(), key) != known.end();
}

json unknown_residual(const json& obj, std::span<const std::string_view> known) {
  json out = json::object();
  if (!obj.is_object()) {
    return out;
  }
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    if (!names_key(known, std::string_view(it.key()))) {
      out[it.key()] = it.value(); // atomic: arrays and scalars are stashed whole
    }
  }
  return out;
}

json unknown_residual_at(const json& obj, std::string_view key,
                         std::span<const std::string_view> known) {
  if (!obj.is_object()) {
    return json::object();
  }
  const auto it = obj.find(std::string(key));
  if (it == obj.end() || !it->is_object()) {
    return json::object(); // absent, or a malformed KNOWN key -- leniency, not a stash
  }
  return unknown_residual(*it, known);
}

json unknown_residual_diff(const json& input, const json& produced) {
  json out = json::object();
  if (!input.is_object()) {
    return out;
  }
  if (!produced.is_object()) {
    return input; // the codec produced nothing object-shaped: every input key is residual
  }
  for (auto it = input.begin(); it != input.end(); ++it) {
    const auto pit = produced.find(it.key());
    if (pit == produced.end()) {
      out[it.key()] = it.value(); // a key the codec did not reproduce == did not consume
      continue;
    }
    if (pit->is_object() && it.value().is_object()) {
      json sub = unknown_residual_diff(it.value(), *pit);
      if (!sub.empty()) {
        out[it.key()] = std::move(sub);
      }
    }
    // else: the codec reproduced this key -- it consumed it, so it is NOT unknown.
  }
  return out;
}

UnknownFields to_unknown_fields(const json& residual) {
  if (!residual.is_object() || residual.empty()) {
    return UnknownFields{};
  }
  // Canonical (sorted keys, shortest round-trip numbers) and non-throwing: the replacing
  // error handler guarantees no nlohmann exception crosses the boundary, so a hostile
  // document can never fault the loader through the stash (Constraint 5/10).
  return UnknownFields{residual.dump(-1, ' ', false, json::error_handler_t::replace)};
}

void merge_unknown(json& known, const json& stash) {
  if (!known.is_object() || !stash.is_object()) {
    return;
  }
  for (auto it = stash.begin(); it != stash.end(); ++it) {
    const auto kit = known.find(it.key());
    if (kit == known.end()) {
      known[it.key()] = it.value();
    } else if (kit->is_object() && it.value().is_object()) {
      merge_unknown(*kit, it.value()); // recurse through a shared sub-object
    }
    // else: the KNOWN value wins outright -- a preserved unknown never shadows it
    // (doc 08:96; `working_space.primaries` is the live case).
  }
}

void merge_unknown_fields(json& known, const UnknownFields* fields) {
  if (fields == nullptr || fields->empty()) {
    return;
  }
  const json stash = json::parse(fields->bytes.begin(), fields->bytes.end(), /*cb=*/nullptr,
                                 /*allow_exceptions=*/false);
  if (stash.is_discarded() || !stash.is_object()) {
    return; // a malformed stash degrades to "no preserved fields", never to a fault
  }
  merge_unknown(known, stash);
}

} // namespace arbc
