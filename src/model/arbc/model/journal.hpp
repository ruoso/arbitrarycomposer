#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/ids.hpp>
#include <arbc/model/journal_entry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>

#include <cstddef>
#include <limits>
#include <vector>

namespace arbc {

// Abstract, model-defined seam (pure change-notification, doc 02): the content
// contribution to the journal's byte budget. The journal knows record sizes at
// L2 but a content object's editable-state payload is an L3 concern, so it asks
// this coster for a `StateHandle`'s byte cost (-> `Editable::state_cost`,
// doc 14:111-122). `model.editable_facet` registers the concrete one from above;
// while none is registered a content handle contributes 0 and only record sizes
// bound the budget (doc 17:66-72, refinement Decisions).
// The owning `ObjectId` rides the call for the same reason it rides
// `StateRefSink` and `RestoreSink`: a bare `StateHandle` names a slot in some
// content's store but not WHICH content's, and the journal is already
// per-content -- a `ContentStateEdit` carries the owner (`journal_entry.hpp`).
class ARBC_API StateCostFn {
public:
  virtual ~StateCostFn() = default;
  virtual std::size_t cost(ObjectId content, const StateHandle& handle) const = 0;
};

// Abstract, model-defined seam (pure change-notification, doc 02): a navigation
// publish notifies here once per `ContentStateEdit` so the LIVE L3 content object
// adopts the target state (-> `Editable::restore`, doc 14:117). It is NOT on the
// output correctness path -- the rebound `ObjectRecord` already carries the target
// `StateHandle`, so render workers reading the pinned handle are correct with zero
// `Editable` calls; the restore only lets the live editor's next `capture()` build
// on the restored base. When none is registered it is a no-op.
class ARBC_API RestoreSink {
public:
  virtual ~RestoreSink() = default;
  virtual void on_restore(ObjectId content, StateHandle target) = 0;
};

// The document-wide history (doc 14 § History). A core-owned `arbc::model` (L2)
// `CommitSink` that stores the `JournalEntry` stream, navigates it with undo/redo
// as ORDINARY forward publishes (never mutating history), and bounds it by bytes
// with tail trimming. Single-writer: the store, the cursor, trimming, and every
// navigation publish are writer-thread only (doc 14:71, doc 15:137-143).
//
// Cursor model: `d_cursor` is the count of APPLIED entries in `[0, depth()]`. A
// commit appends at the tip and moves the cursor to the new tip; `undo()` steps
// the cursor back one entry (rebinding to its *before*), `redo()` forward one
// (rebinding to its *after*).
class ARBC_API Journal final : public CommitSink {
public:
  // A large default budget: trimming is opt-in via `byte_budget` so a journal that
  // does not configure a bound never trims.
  static constexpr std::size_t k_no_budget = std::numeric_limits<std::size_t>::max();

  explicit Journal(Model& model, std::size_t byte_budget = k_no_budget) noexcept
      : d_model(&model), d_budget(byte_budget) {}

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  // Register the L3 seams (writer-thread only; null clears). Consumed by
  // `model.editable_facet` from above.
  void set_state_cost_fn(StateCostFn* fn) noexcept { d_cost_fn = fn; }
  void set_restore_sink(RestoreSink* sink) noexcept { d_restore_sink = sink; }

  // CommitSink: append the committed entry (or fold it into the tip under a
  // matching coalescing key), drop any redo tail on a fresh non-coalescing commit,
  // then trim to the byte budget. WRITER-THREAD ONLY.
  void on_commit(JournalEntry entry) override;

  // Navigate one entry as an ordinary forward publish (revision +1) that rebinds
  // every touched object to the entry's *before* (undo) / *after* (redo) edge and
  // flushes its damage once, without touching history. Fires the `RestoreSink`
  // once per `ContentStateEdit`. Returns false when there is nothing to move to
  // (or, rarely, on a writer-path allocation failure -- the cursor stays put).
  // WRITER-THREAD ONLY.
  bool undo();
  bool redo();

  bool can_undo() const noexcept { return d_cursor > 0; }
  bool can_redo() const noexcept { return d_cursor < d_entries.size(); }

  // Number of stored (undoable + redoable) entries.
  std::size_t depth() const noexcept { return d_entries.size(); }
  // Applied-entry count -- the cursor position in `[0, depth()]`.
  std::size_t cursor() const noexcept { return d_cursor; }
  // The accumulated byte cost the budget bounds (record sizes + content cost).
  std::size_t byte_cost() const noexcept { return d_total_cost; }

  // Read a stored entry (history-inspection seam; `i` in `[0, depth())`).
  const JournalEntry& entry_at(std::size_t i) const { return d_entries[i].entry; }

private:
  // Byte cost of one entry: `sizeof(ObjectRecord)` per non-empty before/after edge
  // (known at L2) + the registered coster's cost of each content (before, after)
  // handle (0 when no coster is registered).
  std::size_t entry_cost(const JournalEntry& e) const;

  // Trim oldest entries from the front until within budget, never below one entry;
  // dropping an entry releases its owning edges (version GC reclaims the uniquely
  // superseded records on the next drain, doc 15:119-123).
  void trim();

  // One stored entry plus its memoized byte cost (so trimming and coalescing need
  // no re-coster pass -- the coster call-count stays a clean behavioral witness).
  struct Stored {
    JournalEntry entry;
    std::size_t cost{0};
  };

  Model* d_model;
  std::size_t d_budget;
  StateCostFn* d_cost_fn{nullptr};
  RestoreSink* d_restore_sink{nullptr};
  std::vector<Stored> d_entries;
  std::size_t d_cursor{0};
  std::size_t d_total_cost{0};
};

} // namespace arbc
