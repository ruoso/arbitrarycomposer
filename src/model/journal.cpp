#include <arbc/model/journal.hpp>

#include <cstddef>
#include <utility>

namespace arbc {

std::size_t Journal::entry_cost(const JournalEntry& e) const {
  std::size_t cost = 0;
  // Record sizes the journal knows at L2: one slab per non-empty owning edge the
  // entry retains (an add has no before; a remove has no after).
  for (const ObjectEdit& oe : e.objects) {
    if (oe.before) {
      cost += sizeof(ObjectRecord);
    }
    if (oe.after) {
      cost += sizeof(ObjectRecord);
    }
  }
  // Content-state payload cost via the L3 seam; 0 when unregistered.
  if (d_cost_fn != nullptr) {
    for (const ContentStateEdit& ce : e.contents) {
      cost += d_cost_fn->cost(ce.before);
      cost += d_cost_fn->cost(ce.after);
    }
  }
  return cost;
}

void Journal::trim() {
  // Drop oldest entries from the front until within budget, never below one entry
  // (doc 14:173-179). Erasing a `Stored` runs `~JournalEntry`, whose `ObjectEdit`
  // Refs release -- version GC reclaims the uniquely-superseded records on drain.
  while (d_entries.size() > 1 && d_total_cost > d_budget) {
    d_total_cost -= d_entries.front().cost;
    d_entries.erase(d_entries.begin());
    // An applied entry left the front: the tip index shrinks with it. (Trimming
    // only runs right after an append, where the cursor is at the tip, so a front
    // pop always removes an applied entry.)
    if (d_cursor > 0) {
      --d_cursor;
    }
  }
}

void Journal::on_commit(JournalEntry entry) {
  const bool at_tip = d_cursor == d_entries.size();
  // Coalescing threads only at the tip: same non-zero key AND the cursor at the
  // tip entry (doc 14:86-91). A redo tail or a keyless commit breaks the run.
  if (entry.coalesce_key != k_no_coalesce && at_tip && !d_entries.empty() &&
      d_entries.back().entry.coalesce_key == entry.coalesce_key) {
    Stored& tip = d_entries.back();
    d_total_cost -= tip.cost;
    coalesce_entries(tip.entry, entry); // first-before / last-after, unioned sets
    tip.cost = entry_cost(tip.entry);
    d_total_cost += tip.cost;
    // No new slot, no cursor move: the merged gesture is still one undoable step.
    trim();
    return;
  }

  // A fresh non-coalescing commit while the cursor is not at the tip discards the
  // redo tail (doc 14:43 -- always consistent); their owning edges release.
  if (d_cursor < d_entries.size()) {
    for (std::size_t i = d_cursor; i < d_entries.size(); ++i) {
      d_total_cost -= d_entries[i].cost;
    }
    d_entries.erase(d_entries.begin() + static_cast<std::ptrdiff_t>(d_cursor), d_entries.end());
  }

  const std::size_t cost = entry_cost(entry);
  d_entries.push_back(Stored{std::move(entry), cost});
  d_total_cost += cost;
  d_cursor = d_entries.size(); // cursor follows to the new tip
  trim();
}

bool Journal::undo() {
  if (!can_undo()) {
    return false;
  }
  const JournalEntry& entry = d_entries[d_cursor - 1].entry;
  // Ordinary forward publish rebinding to each edit's *before* edge; the commit
  // sink (this journal) is not re-entered -- history is never mutated.
  if (!d_model->navigate(entry, Model::NavDirection::Undo)) {
    return false; // rare writer-path allocation failure: leave the cursor put
  }
  // The live L3 content follows to the *before* state (in addition to the already-
  // correct rebound record). No-op when no RestoreSink is registered.
  if (d_restore_sink != nullptr) {
    for (const ContentStateEdit& ce : entry.contents) {
      d_restore_sink->on_restore(ce.object, ce.before);
    }
  }
  --d_cursor;
  return true;
}

bool Journal::redo() {
  if (!can_redo()) {
    return false;
  }
  const JournalEntry& entry = d_entries[d_cursor].entry;
  if (!d_model->navigate(entry, Model::NavDirection::Redo)) {
    return false;
  }
  if (d_restore_sink != nullptr) {
    for (const ContentStateEdit& ce : entry.contents) {
      d_restore_sink->on_restore(ce.object, ce.after);
    }
  }
  ++d_cursor;
  return true;
}

} // namespace arbc
