#include <arbc/runtime/recovered_state_replay.hpp>

#include <arbc/contract/registry.hpp>

namespace arbc {

RecoveredStateReplayStats
replay_recovered_content_state(const std::vector<Model::RecoveredContentState>& recovered,
                               const Registry& registry, const RecoveredStateHooks& hooks) {
  RecoveredStateReplayStats stats;
  for (const Model::RecoveredContentState& entry : recovered) {
    // Resolve the persisted per-load kind token to its durable kind id. An
    // unresolvable token (an empty view) is skipped: the kind is not one this build
    // knows, so no walker can own its slab. Counted, never a hard error -- a
    // recovered document opens even holding a since-removed kind's inert remnants.
    const std::string_view kind_id = hooks.kind_id_of ? hooks.kind_id_of(entry.kind) : std::string_view{};
    if (kind_id.empty()) {
      ++stats.skipped;
      continue;
    }
    // A kind that registered no walker cannot participate: skip (counted). Every
    // kind today is here -- the replay is a no-op until the first persistent kind
    // registers a walker, byte-identical to the pre-hook reopen.
    const KindStateWalker* const walker = registry.state_walker(kind_id);
    if (walker == nullptr || walker->reach == nullptr) {
      ++stats.skipped;
      continue;
    }
    // Hand the kind's document-level state store (type-erased, possibly null) and
    // the persisted handle to the walker, which rebuilds the slab refcounts the
    // handle keeps reachable. This is the recovery twin of `StateRefSink::retain`.
    void* const store = hooks.store_of ? hooks.store_of(kind_id) : nullptr;
    walker->reach(store, entry.content, entry.state);
    ++stats.dispatched;
  }
  return stats;
}

} // namespace arbc
