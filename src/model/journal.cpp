#include <arbc/model/journal.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace arbc {

// The any-thread enable reads promise lock-free (doc 14 § The enable state is
// published): the published pair has to fit in one lock-free word, or `can_undo()`
// on a UI thread would take a lock behind the host's back.
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

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
      cost += d_cost_fn->cost(ce.object, ce.before);
      cost += d_cost_fn->cost(ce.object, ce.after);
    }
  }
  return cost;
}

void Journal::publish_history() {
  // Reuse an unchanged row by POINTER: the entry vector is append-at-tip and
  // trim-at-front, so on a commit every surviving row is the same object it was and
  // only the tip is new. Comparing name and cost is cheaper than allocating a row,
  // and it makes a republish after an undo (which changes no entry at all, only the
  // cursor) allocate nothing.
  const std::shared_ptr<const HistoryView> prev = d_history.load(std::memory_order_relaxed);
  auto next = std::make_shared<HistoryView>();
  next->reserve(d_entries.size());
  for (std::size_t i = 0; i < d_entries.size(); ++i) {
    const Stored& stored = d_entries[i];
    if (prev != nullptr && i < prev->size()) {
      const std::shared_ptr<const HistoryRow>& old = (*prev)[i];
      if (old != nullptr && old->name == stored.entry.name && old->byte_cost == stored.cost) {
        next->push_back(old);
        continue;
      }
    }
    next->push_back(std::make_shared<const HistoryRow>(HistoryRow{stored.entry.name, stored.cost}));
  }
  d_history.store(std::shared_ptr<const HistoryView>(std::move(next)), std::memory_order_release);
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
  // Publish once, after the vector has stopped moving -- an intermediate depth is
  // never a state a reader could act on. Both counts go out in one store, so a
  // reader never sees a pair the writer did not hold (`journal.hpp`, `publish`).
  publish();
}

void Journal::on_commit(JournalEntry entry) {
  const std::size_t cursor_now = d_cursor;
  const bool at_tip = cursor_now == d_entries.size();
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
    trim(); // ends every commit path with the publish
    return;
  }

  // A fresh non-coalescing commit while the cursor is not at the tip discards the
  // redo tail (doc 14:43 -- always consistent); their owning edges release.
  if (cursor_now < d_entries.size()) {
    for (std::size_t i = cursor_now; i < d_entries.size(); ++i) {
      d_total_cost -= d_entries[i].cost;
    }
    d_entries.erase(d_entries.begin() + static_cast<std::ptrdiff_t>(cursor_now), d_entries.end());
  }

  const std::size_t cost = entry_cost(entry);
  d_entries.push_back(Stored{std::move(entry), cost});
  d_total_cost += cost;
  d_cursor = d_entries.size(); // the cursor follows to the new tip
  // Nothing is published between the push and the trim: `trim()` may move the pair
  // again, and the only state a reader may act on is the settled one it publishes.
  trim();
}

bool Journal::undo() {
  // The writer re-checks against its OWN counts, not the published (saturating)
  // view -- this is the check a stale reader's dispatched undo lands on.
  if (d_cursor == 0) {
    return false;
  }
  const std::size_t cursor_now = d_cursor;
  const JournalEntry& entry = d_entries[cursor_now - 1].entry;
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
  d_cursor = cursor_now - 1;
  publish();
  return true;
}

bool Journal::redo() {
  if (d_cursor >= d_entries.size()) { // as `undo()`: the writer's own counts
    return false;
  }
  const std::size_t cursor_now = d_cursor;
  const JournalEntry& entry = d_entries[cursor_now].entry;
  if (!d_model->navigate(entry, Model::NavDirection::Redo)) {
    return false;
  }
  if (d_restore_sink != nullptr) {
    for (const ContentStateEdit& ce : entry.contents) {
      d_restore_sink->on_restore(ce.object, ce.after);
    }
  }
  d_cursor = cursor_now + 1;
  publish();
  return true;
}

} // namespace arbc
