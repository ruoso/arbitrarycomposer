#pragma once

// INTERNAL serialize-component header (deliberately NOT in the component's
// PUBLIC_HEADERS FILE_SET): it names `nlohmann::json`, which is linked PRIVATE so it
// never rides libarbc's public interface (serialize.writer Decision 3, doc 08:59-74).
// Only serialize-component translation units include it.
//
// The residual/merge core of doc 08 Principle 4 (serialize.unknown_field_preservation):
// one pair of operations applied uniformly at every tier.
//
//   * RESIDUAL (load): `input_object - k_<tier>_keys`, a subtraction against a
//     compile-time known-key set (Decision 1). Arrays and scalars are atomic -- a key is
//     either wholly known or wholly stashed -- and the recursion into a known sub-object
//     is spelled at the call site (`unknown_residual_at`), so an unknown key INSIDE a
//     known sub-object (`working_space`, `time_map`, the `arbc` envelope) survives too.
//     "Unknown" is decided by KEY NAME ONLY: a known key carrying a malformed value
//     stays known and the reader's leniency substitutes its default (Decision 2).
//
//   * MERGE (save): the writer builds its known object FIRST, then folds the stash in.
//     A stashed key already present in the known object LOSES; where both sides are
//     objects the merge recurses (doc 08:96). Merging into an `nlohmann::json` object (a
//     `std::map`) yields ascending-key order for free, so Principle 5's canonical output
//     needs no extra work.
//
// Errors stay values (Constraint 10): a stash whose bytes fail to re-parse, or that
// re-parses to a non-object, is treated as EMPTY -- never as a `ReaderError` /
// `SerializeError`, so the loader-never-faults guarantee is not weakened.

#include <arbc/serialize/unknown_fields.hpp>

#include <nlohmann/json.hpp>

#include <span>
#include <string_view>

namespace arbc {

// Whether `known` names `key` (the key-name-only test Decision 2 rests on).
bool names_key(std::span<const std::string_view> known, std::string_view key);

// `obj - known`: the keys present in `obj` that the core does not name at this tier.
// A non-object `obj` yields an empty object.
nlohmann::json unknown_residual(const nlohmann::json& obj, std::span<const std::string_view> known);

// The residual of a KNOWN sub-object: `obj[key] - known`, empty when `key` is absent or
// is not an object. The caller nests it under `key` in the parent tier's stash, so the
// never-shadow merge on save recurses back into the writer's own sub-object.
nlohmann::json unknown_residual_at(const nlohmann::json& obj, std::string_view key,
                                   std::span<const std::string_view> known);

// The codec residual (Decision 4): `input - produced`, i.e. the keys of the input
// `params` that the codec's own re-serialization did not reproduce -- exactly the set it
// did not consume. Recurses where BOTH sides are objects; anything else is atomic.
// Computed at LOAD time, before any edit can touch the content, so clearing an optional
// param never resurrects it.
nlohmann::json unknown_residual_diff(const nlohmann::json& input, const nlohmann::json& produced);

// The canonical byte carrier for a residual object: its canonical `dump()`, or empty
// `UnknownFields` when the residual holds nothing. Dumped with the replacing error
// handler so no nlohmann exception can escape (doc 10 errors-as-values).
UnknownFields to_unknown_fields(const nlohmann::json& residual);

// The never-shadow merge (doc 08:96): fold `stash` into the already-built `known`
// object. A key `known` already carries WINS outright; where both sides are objects the
// merge recurses.
void merge_unknown(nlohmann::json& known, const nlohmann::json& stash);

// `merge_unknown` over the byte carrier: parse `fields` (non-throwing) and fold it in.
// A null / empty / unparseable / non-object stash is a no-op (Constraint 10).
void merge_unknown_fields(nlohmann::json& known, const UnknownFields* fields);

} // namespace arbc
