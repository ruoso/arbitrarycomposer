#pragma once

#include <arbc/arbc_api.h>
#include <arbc/model/model.hpp> // Model::RecoveredContentState

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace arbc {

class Registry;

// The runtime replay of the model's per-kind state-slab walk hook
// (model.persistent_state_walk_hook, issue #5). `Model::open` collects every
// reachable non-inert content `StateHandle` it cannot descend -- the model holds
// only the opaque slot and levelization forbids it naming the kind -- and exposes
// them via `Model::recovered_content_state()`. This replays them, once the
// `Document` and its document-level kind stores exist, through the per-kind
// `Registry` walkers so each kind rebuilds the slab refcounts a persisted handle
// keeps reachable: the recovery twin of the writer-owned `StateRefSink`
// retain/release seam, deferred because `open` runs before any Document.
struct RecoveredStateHooks {
  // The persisted per-load kind token -> its reverse-DNS kind id, or an EMPTY view
  // for an unresolvable token (the handle is then skipped, counted). A built-in
  // kind's token is stable, so a fresh `KindBridge::lookup` resolves it; a document
  // supplies the bridge it opened with.
  std::function<std::string_view(std::uint64_t kind)> kind_id_of;
  // A kind id -> the kind's document-level state store, type-erased for the walker
  // thunk to `static_cast` (may be null for a kind whose walker needs no store).
  // The `Document` owns these stores.
  std::function<void*(std::string_view kind_id)> store_of;
};

// A behavioral witness the recovery replay is exact (doc 16: counters, never
// wall-clock). `dispatched + skipped == recovered.size()` always.
struct RecoveredStateReplayStats {
  std::size_t dispatched{0}; // handles handed to a kind's registered walker
  std::size_t skipped{0};    // unresolvable token, or a kind with no walker
};

// Replay `recovered` through `registry`'s per-kind walkers using `hooks`. For each
// collected handle: resolve its token to a kind id (skip on an empty id), look up
// the kind's registered `KindStateWalker` (skip if the kind registered none), and
// invoke `walker.reach(store_of(kind_id), content, handle)` -- passing whatever
// store the hook returns, null included, for the walker to interpret. Returns the
// tallies. WRITER/OPEN-THREAD ONLY: runs during `Document::open`, single-threaded,
// before any reader pins the recovered version.
ARBC_API RecoveredStateReplayStats
replay_recovered_content_state(const std::vector<Model::RecoveredContentState>& recovered,
                               const Registry& registry, const RecoveredStateHooks& hooks);

} // namespace arbc
