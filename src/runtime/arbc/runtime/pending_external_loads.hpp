#pragma once

// runtime.async_external_load: the load state that OUTLIVES one load (doc 05:56-74, doc
// 17:60 -- runtime is "Document (arenas + model + registry + loaders)").
//
// Every object a synchronous external load needs is a stack local of `runtime::load_document`
// -- the `LoadContext`, the `CodecTable`, the `ExternalCompositionLoader`, the `ContentSink`
// -- and all four are destroyed before a DEFERRING `AssetSource`'s `on_ready` can fire. This
// is the part that must survive: the resolved-identity dedup map, the pending entries, and
// the completion queue the arrival lands in.
//
// It is NOT the loader. `ExternalCompositionLoader` keeps its character -- constructed per
// load and per settle, driven on one thread -- and BORROWS this map instead of owning one
// (Decision 2). What is durable is the identity map, not the machinery.
//
// THREADING (Decision 4, Constraint 4). `Model::Transaction::commit()` and `DamageRouter`
// are writer-thread-only, and the router explicitly carries "NO cross-thread channel". So
// this object is the task's ONE cross-thread channel, and it is deliberately tiny: the
// mutex guards the completion queue and NOTHING else. `complete()` -- the only method an
// `on_ready` calls -- copies the bytes under that mutex and returns, touching no `Model`, no
// `Document`, no `LoadContext`. Every other method (the dedup map, the pending set, the
// asset source) is load/writer-thread-only, exactly as `LoadContext` is
// ("single-writer, not thread safe: a load runs on one thread").
//
// LIFETIME (Constraint 6). A network fetch can outlive the document that started it. The
// `Document` owns this by `shared_ptr` and every `on_ready` closure captures a `weak_ptr`;
// a callback whose queue has expired drops its bytes and returns. No use-after-free, no
// resurrection of a dead document.
//
// Names no JSON type, so it rides the runtime PUBLIC headers.

#include <arbc/arbc_api.h>
#include <arbc/base/ids.hpp>
#include <arbc/serialize/load_context.hpp> // AssetSource

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbc {

class ARBC_API PendingExternalLoads {
public:
  // One arrival: the `ObjectId` the loader pre-allocated before the fetch, and the bytes
  // `on_ready` delivered. EMPTY bytes mean the source answered ABSENCE -- unavailable, not
  // an error (doc 05:50-52) -- which is a different thing from never having answered.
  //
  // ONE queue serves BOTH arrival kinds (kinds.image_async_pending Decision 2). A composition
  // child's id and an asset FETCH's id are both drawn from `Model::allocate_id()`'s monotonic
  // space, so they cannot collide and need no discriminant on the wire: a settle asks the
  // asset map first and the composition map second. That is what lets the queue, the mutex,
  // the `weak_ptr` guard and the loop-to-quiescence settle be reused rather than duplicated --
  // and it is what lets a pending IMAGE inside a pending nested CHILD interleave in one loop.
  struct Arrival {
    ObjectId child{};
    std::string bytes;
  };

  // A pending ASSET fetch: the resolved URI whose bytes are in flight, and the contents
  // awaiting them. Usually one content, more when several layers spell one URI several ways
  // -- they share one `request()` and one decode, and the single arrival damages all of them
  // (Decision 4). The list is also the damage route: an image IS the damaged object, so
  // unlike a composition arrival there is no embedding indirection to reverse.
  struct AssetEntry {
    std::string uri; // resolved
    std::vector<ObjectId> awaiting;
  };

  // --- The load / writer thread half. NOT thread-safe (a load runs on one thread). ------

  // The asset source the host load was driven with. A settle re-uses it: a child that lands
  // late may itself hold external references, and its own fetches go through the same source,
  // on a later revision. It must therefore outlive the `Document` -- the queue's `weak_ptr`
  // protects the reverse direction (a callback outliving the document), not this one.
  //
  // The document's BASE URI is deliberately not kept: a settle parses each child against the
  // child's OWN resolved URI, which rides its pending entry, because a child's relative
  // references resolve against THE CHILD (doc 08 Principle 3).
  void set_source(AssetSource* source) noexcept { d_source = source; }
  AssetSource* source() const noexcept { return d_source; }

  // The resolved-identity dedup map (doc 08 Principle 3). An entry mapping to an INVALID id
  // is a remembered unavailability. Because the id is recorded before the FETCH (not merely
  // before the parse), a back-edge reaching a URI whose bytes are still IN FLIGHT resolves to
  // the in-flight id and issues no second request -- which is what makes a DEFERRING cycle
  // terminate (Constraint 8).
  bool find(const std::string& uri, ObjectId& child) const;
  void record(std::string uri, ObjectId child);

  // A reference whose source has not answered yet. `depth` is the loader recursion depth at
  // which the reference was REACHED: a live recursion counter is meaningless across an async
  // boundary (the parse resumes on a fresh stack), so the depth is carried on the entry and
  // restored at settle. Without it a hostile chain that defers at every link would look like
  // depth 0 forever and the cap would silently stop capping (Decision 5).
  void add_pending(ObjectId child, std::string uri, std::size_t depth);
  bool take_pending(ObjectId child, std::string& uri, std::size_t& depth);

  // --- the ASSET half (kinds.image_async_pending) --------------------------------------

  // An in-flight asset fetch, keyed by the RESOLVED URI so two authored spellings of one
  // file join one request. Only IN-FLIGHT fetches are here: `take_pending_asset` drops the
  // entry and its URI together, so a settled (or inline-answered) URI leaves no trace and a
  // later reference re-fetches it honestly. Dedup lives on the FETCH, not on the content --
  // a naive per-content fetch would break `#image-decodes-once-per-resolved-uri` the moment
  // both contents deferred.
  bool find_asset(const std::string& uri, ObjectId& fetch) const;
  void add_pending_asset(ObjectId fetch, std::string uri);

  // Bind a content's `ObjectId` onto the fetch it awaits. Called from the load's `ContentSink`,
  // which is the exact and only point where MINTING meets IDENTITY: the codec builds the
  // content but the sink assigns its id (Decision 3).
  void add_asset_waiter(ObjectId fetch, ObjectId content);

  // Pop the pending asset entry for `fetch`, if any. Absent means the fetch already settled
  // (a duplicate arrival), was answered inline, or was never an asset at all -- which is how
  // a settle tells an asset arrival from a composition one, and how a duplicate arrival
  // installs nothing.
  bool take_pending_asset(ObjectId fetch, AssetEntry& entry);

  // Every reference this document is still waiting on -- compositions AND assets. The
  // counter's name always meant "external fetches I am waiting on"; the asset arm is what
  // made the second half of it real.
  std::size_t pending() const noexcept { return d_pending.size() + d_pending_assets.size(); }

  // --- The queue. `complete` is ANY-THREAD; the takers are writer-thread. ---------------

  // Deliver a fetched asset. Called from `on_ready`, on whatever thread the source chose,
  // possibly long after `load_document` returned. Copies `bytes` into an owned `std::string`
  // (Constraint 5: the view is valid only for the callback) and returns.
  void complete(ObjectId child, std::string_view bytes);

  // Pop the arrival for exactly `child`, if it is already there. This is how the loader asks
  // "did `on_ready` fire INSIDE `request()`?" -- the pending/unavailable split is a property
  // of whether the source answered, never of the bytes being empty (Constraint 2), and
  // routing the inline answer through the same queue is what makes that question race-free
  // against a source that answers from another thread.
  bool take_arrival(ObjectId child, std::string& bytes);

  // Drain every arrival delivered so far (the settle step's ready queue).
  std::vector<Arrival> take_ready();

  // How many arrivals are sitting in the queue, waiting for a writer-thread settle to install
  // them. ANY THREAD, and deliberately lock-free and allocation-free: this is what a
  // render-thread frame loop polls to learn that a settle is OWED without taking the queue's
  // mutex on every idle frame and without publishing anything (issue #13). It shadows the
  // queue rather than measuring it -- one relaxed counter maintained under the same mutex the
  // queue is -- so a reader can observe it between the counter store and the queue push in
  // either order; both are transient states of "an arrival is landing", and the next poll
  // settles it. Never used to DECIDE what to install; the settle drains the queue itself.
  std::size_t ready() const noexcept { return d_ready_count.load(std::memory_order_relaxed); }

private:
  struct Entry {
    std::string uri;
    std::size_t depth{0};
  };

  // Writer/load-thread-only state.
  std::unordered_map<std::string, ObjectId> d_by_uri; // resolved URI -> child root composition
  std::unordered_map<ObjectId, Entry> d_pending;      // child id -> {resolved URI, reached depth}
  // The asset half. Two maps rather than one because an asset URI and a composition URI are
  // different namespaces reached by different seams, and a fetch id is not a composition id.
  std::unordered_map<std::string, ObjectId> d_asset_by_uri;  // resolved URI -> IN-FLIGHT fetch id
  std::unordered_map<ObjectId, AssetEntry> d_pending_assets; // fetch id -> {URI, awaiting contents}
  AssetSource* d_source{nullptr};

  // The one cross-thread channel.
  std::mutex d_mutex;
  std::vector<Arrival> d_ready;
  // `d_ready.size()`, readable without the mutex (`ready()`). Maintained under it.
  std::atomic<std::size_t> d_ready_count{0};
};

} // namespace arbc
