#pragma once

// kinds.image_async_pending: the loader that turns an asset-referencing kind's authored URI
// into BYTES -- and, when the source does not answer inside `request()`, into a durable
// PENDING entry whose arrival is installed on the writer thread later (doc 08 Principle 3,
// doc 05:61-83).
//
// The sibling of `ExternalCompositionLoader`, deliberately and structurally. Doc 08:34-36
// says "one resolution seam serves both", and `runtime.async_external_load` already built
// the whole deferral substrate for the composition half: the three-state machine, the
// durable pending map, the mutex-guarded arrival queue, the `weak_ptr` teardown guard, the
// writer-thread settle loop and the frame-loop driver. This BORROWS all of it -- it is a
// second value shape over one `PendingExternalLoads`, not a second copy of the machinery.
//
// The asymmetry that remains is the asymmetry between the two targets, and it is why these
// are two types rather than one with an `if (is_asset)` through it: a composition arrival
// PARSES an `.arbc` and installs a graph, recursing, so it carries a depth and a cap; an
// asset arrival DECODES pixels, touches no graph, and does not recurse, so it carries
// neither. What an asset carries instead is a list of the contents awaiting it -- which is
// also its damage route, because an image IS the damaged object (Decision 4).
//
// THE PENDING/UNAVAILABLE SPLIT is `external_composition_loader.cpp:70-76`, copied exactly:
// the answer is taken from the mutex-guarded arrival queue, never from a captured stack flag,
// so "did the source answer?" is race-free against a source answering from another thread
// while `request()` is still returning. It is never "are the bytes empty?" -- conflating
// those is the bug `kinds.image` Decision 5 knowingly shipped and this task pays off.
//
// Names no JSON type, so it rides the runtime PUBLIC headers.

#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/pending_external_loads.hpp>
#include <arbc/serialize/load_context.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace arbc {

// Owned by one load (and by one settle), driven on one thread -- `LoadContext` is
// "single-writer, not thread safe: a load runs on one thread" and this inherits that. Built
// by `LoadAssembly` beside its composition sibling, over the same `Document`-owned durable
// state, so a settle fetches through exactly the loader a load would have.
class ExternalAssetLoader {
public:
  // The outcome of resolving and fetching one authored asset reference. THREE outcomes, not
  // two, and the third is the whole task:
  //
  //  - RESOLVED: the source answered inside `request()`. `bytes` holds them (possibly EMPTY,
  //    which is the source answering ABSENCE -- unavailable). `pending` is invalid.
  //  - UNAVAILABLE: an empty reference, or a source that answered absence. `bytes` is empty
  //    and `pending` is invalid, and the kind is minted with no pixels.
  //  - PENDING: the source has NOT ANSWERED YET. `bytes` is empty -- the same wire shape
  //    unavailable uses, so the kind needs no new state -- and `pending` is the VALID fetch
  //    id the arrival will land under. The caller must `await()` the minted content on it.
  //
  // `resolved` is the canonical identity in every case, because the kind's decode cache dedups
  // on it and a late install must key the very same entry.
  struct FetchedAsset {
    std::string resolved;
    std::string bytes;
    ObjectId pending{};
  };

  ExternalAssetLoader(Model& into, const std::shared_ptr<PendingExternalLoads>& state);

  // Resolve `authored` against `ctx`'s base URI, fetch its bytes through the one seam, and
  // report which of the three outcomes happened.
  //
  // Dedup is on the FETCH, by resolved URI: two layers spelling one file two ways
  // (`bg.png`, `./bg.png`) issue ONE `request()` and share ONE pending entry, so one arrival
  // settles both and one decode serves both -- which is what `#image-decodes-once-per-
  // resolved-uri` already promises and what a naive per-content fetch would break the moment
  // both deferred.
  //
  // ALLOCATE BEFORE FETCH, exactly as the composition loader does: the fetch id is taken from
  // `Model::allocate_id()` (a bare monotonic counter bump that installs no record) and the
  // pending entry registered BEFORE `request()` is issued, so a source that answers from
  // another thread mid-call lands in a queue that already exists.
  FetchedAsset fetch(LoadContext& ctx, std::string_view authored);

  // Note that `content` -- just minted by the kind's factory, its `ObjectId` not yet assigned
  // -- awaits `fetch`. The pointer is an in-load, same-thread key: it is never dereferenced
  // here and never outlives the load, and by the time `on_ready` may fire on another thread
  // the queue's key is the fetch id, never the pointer.
  void await(const Content* content, ObjectId fetch);

  // Bind the real `ObjectId` the `ContentSink` just assigned onto the pending entry the
  // content awaits (Decision 3). A no-op for a content awaiting nothing, which is every
  // content but a pending image. Thereafter the durable entry keys on the `ObjectId` alone:
  // it is the one fact the arrival's damage route needs, and the pointer is dropped.
  //
  // A content that was minted but never sunk (a load that failed downstream) is simply never
  // bound; its arrival then finds an entry awaiting nobody, installs into nobody, and costs
  // one map erase -- the same "nothing pending" idempotence `settle` already relies on.
  void bind(const Content* content, ObjectId id);

private:
  Model* d_into;
  std::shared_ptr<PendingExternalLoads> d_state;
  // LOAD-SCOPED, and deliberately not on the durable state: a raw `Content*` is meaningful
  // only between the factory call and the sink, both of which run on this thread inside this
  // load. `bind` drains it into the durable entry, which keys on `ObjectId` forever after.
  std::unordered_map<const Content*, ObjectId> d_awaiting;
};

} // namespace arbc
