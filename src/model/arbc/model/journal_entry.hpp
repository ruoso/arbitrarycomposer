#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/ids.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/refs.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace arbc {

// A gesture key (doc 14:86-91). Consecutive commits sharing a non-zero key
// merge into one undo unit; `0` means "no coalescing".
using CoalesceKey = std::uint64_t;
inline constexpr CoalesceKey k_no_coalesce = 0;

// Owning (before, after) object-record edges for one touched object. An empty
// `before` means the object was newly added; an empty `after` means it was
// removed. The owning `Ref`s keep the superseded records memory-live for undo
// (doc 15:119-123); edges are shared by identity, never deep-copied
// (doc 14:175-178). Transient handle type, never an in-arena record.
struct ObjectEdit {
  ObjectId object{};
  Ref<ObjectRecord> before{};
  Ref<ObjectRecord> after{};
};

// A content object's (before, after) state-handle pair (doc 14:133-135). Undo
// of content is an `Editable::restore(before)` at a higher layer, not a record
// swap, so the handles ride alongside the object edges.
struct ContentStateEdit {
  ObjectId object{};
  StateHandle before{};
  StateHandle after{};
};

// The unit the journal stores (doc 14:164): name, coalescing key, per-object
// (before, after) record edges, per-content (before, after) state handles, and
// the damage set. It carries enough for a pure inverse publish so `model.journal`
// implements undo without re-reading the live objects.
struct JournalEntry {
  std::string name;
  CoalesceKey coalesce_key{k_no_coalesce};
  std::vector<ObjectEdit> objects;
  std::vector<ContentStateEdit> contents;
  std::vector<Damage> damage;
};

// Merge `follow` into `base` under coalescing (doc 14:86-91): first entry's
// *before* + last entry's *after*, unioned damage, unioned object/content sets.
// A pure value operation defined here (this task owns "coalescing"); WHICH
// entries are consecutive in history is `model.journal`'s concern.
ARBC_API void coalesce_entries(JournalEntry& base, const JournalEntry& follow);

// Abstract, model-defined commit seam (pure change-notification, doc 02): a
// non-coalesced commit notifies here exactly once with the assembled entry. The
// concrete consumer (the byte-budgeted journal) is wired from above at L3.
class ARBC_API CommitSink {
public:
  virtual ~CommitSink() = default;
  virtual void on_commit(JournalEntry entry) = 0;
};

} // namespace arbc
