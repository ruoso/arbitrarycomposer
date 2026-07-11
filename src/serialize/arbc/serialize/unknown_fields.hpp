#pragma once

// PUBLIC serialize header (serialize.unknown_field_preservation): the opaque carrier
// for doc 08 Principle 4's every-tier unknown-field preservation. It rides the public
// FILE_SET precisely BECAUSE it names no JSON type -- the JSON library stays private to
// `arbc::serialize` (doc 08:59-74 Principle 1; doc 17:58), and `arbc::runtime` (L5)
// must be able to own a store and copy it into a `ContentSnapshot` without ever naming
// `nlohmann::json`.
//
// The payload is therefore BYTES: canonical JSON object text produced by the reader and
// handed back, uninterpreted, to the writer. Every component between the two stores and
// copies it; only `arbc::serialize` parses it.

#include <arbc/base/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace arbc {

// One tier's preserved-and-ignored sibling fields (doc 08:88-98): the canonical JSON
// text of an object holding exactly the keys the core did not name at that tier, or an
// empty string for "nothing was preserved". Opaque to every component but
// `arbc::serialize`.
struct UnknownFields {
  std::string bytes;

  bool empty() const noexcept { return bytes.empty(); }

  friend bool operator==(const UnknownFields&, const UnknownFields&) = default;
};

// The document tiers that carry unknown siblings (Decision 3). `Content` covers a body
// standing alone in the `contents` table or in an `inputs` slot, plus the `params`
// interior of ANY body including a layer's inline one; a layer-position body's own
// unknown siblings are indistinguishable from unknown LAYER fields and are recorded as
// `Layer` (doc 08:101-105, Decision 5).
enum class UnknownScope : std::uint8_t {
  Document,    // the document root + its `arbc` envelope; keyed by `ObjectId{}`
  Composition, // keyed by the composition's ObjectId
  Layer,       // keyed by the layer's ObjectId
  Content,     // keyed by the content's ObjectId
};

// The reader->document->writer stash, keyed by `(scope, ObjectId)` -- NEVER by
// `Content*` (Decision 3). `Model::allocate_id` only ever increments a monotonic
// counter and has no free list, so an ObjectId is never reused and a stale entry can
// never alias a later object; a `Content*`-keyed store would instead rest on
// `Document::d_contents` never being pruned (Constraint 8).
//
// Copyable by design: `capture_snapshot` copies the live document's store into the
// immutable `ContentSnapshot` so the off-thread save reads no live editor state
// (Constraint 9, Decision 6). Cheap -- plain maps of `std::string`.
class UnknownFieldStore {
public:
  // Record `fields` for `(scope, id)`; empty fields erase the entry, so an empty store
  // is exactly today's lossy behavior.
  void set(UnknownScope scope, ObjectId id, UnknownFields fields);

  // The stash for `(scope, id)`, or nullptr when nothing was preserved there.
  const UnknownFields* find(UnknownScope scope, ObjectId id) const;

  bool empty() const noexcept { return d_entries.empty(); }
  std::size_t size() const noexcept { return d_entries.size(); }

private:
  using Key = std::pair<std::uint8_t, std::uint64_t>; // (scope, ObjectId::value)

  std::map<Key, UnknownFields> d_entries;
};

} // namespace arbc
