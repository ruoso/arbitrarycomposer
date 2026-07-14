#include <arbc/runtime/external_asset_loader.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace arbc {

ExternalAssetLoader::ExternalAssetLoader(Model& into,
                                         const std::shared_ptr<PendingExternalLoads>& state)
    : d_into(&into), d_state(state) {}

ExternalAssetLoader::FetchedAsset ExternalAssetLoader::fetch(LoadContext& ctx,
                                                             std::string_view authored) {
  FetchedAsset out;
  if (authored.empty()) {
    // An absent or mistyped `params.source`: nothing to resolve, nothing to fetch. Not
    // pending -- there is no source to be waiting on.
    return out;
  }

  // The resolution choke point (doc 08:33-35). `resolved` is a canonical identity -- `bg.png`
  // and `./bg.png` arrive as one string -- and it is base-INDEPENDENT, which is why the dedup
  // map is keyed on it rather than on the `ResolvedRef` (an index into the owning context's
  // table, not comparable across contexts).
  const ResolvedRef ref = ctx.resolve(authored);
  out.resolved = ctx.resolved_uri(ref);

  if (ObjectId in_flight; d_state->find_asset(out.resolved, in_flight)) {
    // A second reference to a URI whose bytes are ALREADY IN FLIGHT. It joins that fetch
    // rather than issuing a second one -- one `request()`, one arrival, one decode, and the
    // single arrival damages both contents. The caller `await`s the minted content onto the
    // same id.
    out.pending = in_flight;
    return out;
  }

  // ALLOCATE BEFORE FETCH. `Model::allocate_id()` is a bare monotonic counter bump that
  // installs no record, so this mutates no `DocState` -- and registering the entry BEFORE
  // `request()` is issued is what makes the probe below race-free: a source that answers on
  // another thread while `request()` is still returning lands in a queue that already exists.
  const ObjectId id = d_into->allocate_id();
  d_state->add_pending_asset(id, out.resolved);

  // The callback is DURABLE: any thread, any time, possibly long after the `Document` that
  // started the fetch is gone. So it captures a `weak_ptr` -- never a `Document`, never a
  // `Model`, never a `Content`, never this loader, all of which are load-scoped -- and does
  // exactly one thing: copy the bytes into the completion queue under its mutex.
  const std::weak_ptr<PendingExternalLoads> weak = d_state;
  ctx.load_asset(ref, [weak, id](std::string_view bytes) {
    if (const std::shared_ptr<PendingExternalLoads> state = weak.lock()) {
      state->complete(id, bytes);
    }
  });

  // PENDING vs UNAVAILABLE is "did `on_ready` fire?", never "are the bytes empty?"
  // (Constraint 1). Asking the QUEUE rather than a captured stack flag is what makes the
  // question race-free: the queue's mutex orders the two threads. A source that answered with
  // EMPTY bytes answered -- that is unavailable, and it lands here with an empty `bytes`.
  std::string bytes;
  if (!d_state->take_arrival(id, bytes)) {
    out.pending = id; // not yet: the content is minted empty and gains its pixels at settle
    return out;
  }

  // The source answered inside `request()`: the synchronous path, unchanged in behaviour from
  // before this task. Drop the entry -- nothing is in flight, so nothing is pending.
  PendingExternalLoads::AssetEntry settled;
  static_cast<void>(d_state->take_pending_asset(id, settled));
  out.bytes = std::move(bytes);
  return out;
}

void ExternalAssetLoader::await(const Content* content, ObjectId fetch) {
  if (content == nullptr || !fetch.valid()) {
    return;
  }
  d_awaiting.insert_or_assign(content, fetch);
}

void ExternalAssetLoader::bind(const Content* content, ObjectId id) {
  const auto it = d_awaiting.find(content);
  if (it == d_awaiting.end()) {
    return; // awaits nothing: every content but a pending image
  }
  d_state->add_asset_waiter(it->second, id);
  d_awaiting.erase(it);
}

} // namespace arbc
