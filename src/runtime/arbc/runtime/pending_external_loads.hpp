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

#include <arbc/base/ids.hpp>
#include <arbc/serialize/load_context.hpp> // AssetSource

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbc {

class PendingExternalLoads {
public:
  // One arrival: the child `ObjectId` the loader pre-allocated before the fetch, and the
  // bytes `on_ready` delivered. EMPTY bytes mean the source answered ABSENCE -- unavailable,
  // not an error (doc 05:50-52) -- which is a different thing from never having answered.
  struct Arrival {
    ObjectId child{};
    std::string bytes;
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
  std::size_t pending() const noexcept { return d_pending.size(); }

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

private:
  struct Entry {
    std::string uri;
    std::size_t depth{0};
  };

  // Writer/load-thread-only state.
  std::unordered_map<std::string, ObjectId> d_by_uri; // resolved URI -> child root composition
  std::unordered_map<ObjectId, Entry> d_pending;      // child id -> {resolved URI, reached depth}
  AssetSource* d_source{nullptr};

  // The one cross-thread channel.
  std::mutex d_mutex;
  std::vector<Arrival> d_ready;
};

} // namespace arbc
