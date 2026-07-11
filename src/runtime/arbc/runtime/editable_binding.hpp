#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/journal.hpp> // StateCostFn, RestoreSink
#include <arbc/model/model.hpp>   // Model, StateRefSink
#include <arbc/model/records.hpp> // StateHandle

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace arbc {

// The runtime half of doc 14's editable-state contract: it joins the model's
// state sinks (L2: `StateRefSink` on the `Model`, `StateCostFn`/`RestoreSink` on
// the `Journal`) to a content's `Editable` facet (L3), so an editable content
// instantiated into a live document participates in the document's history
// automatically -- no per-kind wiring by the host.
//
// Levelization (doc 17:66-72): binding happens in `runtime`, and it reaches the
// content ONLY through the `contract`-owned `Editable` facet -- never a concrete
// kind type, never a `dynamic_cast`. That keeps the runtime kind-agnostic and
// dlopen-safe: virtual facet dispatch works across a shared-library boundary
// where a `dynamic_cast` to a plugin's concrete type would be fragile.

// The routing table behind the document's single sink trio: `ObjectId` ->
// `Editable*`. Every state seam names its owner (doc 14 § Runtime binding of the
// facet), so a call arriving at the trio is dispatched to the ONE content that
// owns the handle -- which is what lets a document hold any number of editable
// contents on one document-wide journal. Plain per-`Document` state, never a
// static or global registry (doc 15:73-76, doc 17:96-101).
//
// WRITER/DRAIN-THREAD ONLY. Render workers read pinned handles off `DocState`
// (`DocRoot::content_state`) and never touch this table, which is what keeps the
// render path lock-free.
class EditableRouter {
public:
  // Dispatch `content` to its facet, or NULLPTR if it has no row.
  //
  // A miss on a handle that names real state is a defect, never a silent no-op:
  // it means a content's state outlived its binding, and a release routed to the
  // wrong content would free the wrong content's pixels (since
  // `kinds.raster_pool_backing` a raster `StateHandle` transitively owns its tile
  // blobs). So it is COUNTED, and every test asserts the count is zero. It is
  // deliberately NOT thrown: `ContentStateReclaimSink::on_zero` runs on the drain
  // thread, where an escaping exception would tear down reclamation. Correct
  // construction makes the counter unreachable -- `unbind()` drains before it
  // drops a row, so no queued release can outlive its route.
  //
  // A miss on the INERT handle is not a defect and is not counted: the inert
  // sentinel names no state, so nothing can be misrouted and nothing can be
  // freed. That is the ordinary non-editable content, whose record carries the
  // inert handle and whose journal entries stay byte-identical to the
  // pre-binding runtime (doc 14:176-182).
  Editable* route(ObjectId content, const StateHandle& handle) const noexcept {
    const auto it = d_table.find(content);
    if (it != d_table.end()) {
      return it->second;
    }
    if (handle.has_state()) {
      ++d_unrouted;
    }
    return nullptr;
  }

  void insert(ObjectId content, Editable& editable) {
    d_table.insert_or_assign(content, &editable);
  }
  void erase(ObjectId content) noexcept { d_table.erase(content); }
  void clear() noexcept { d_table.clear(); }

  bool contains(ObjectId content) const noexcept { return d_table.contains(content); }
  std::size_t size() const noexcept { return d_table.size(); }

  // The Constraint-4 tripwire (doc 16: behavioral counters, never wall-clock).
  std::uint64_t unrouted_state_calls() const noexcept { return d_unrouted; }

private:
  std::unordered_map<ObjectId, Editable*> d_table;
  // Mutable because `StateCostFn::cost` is const and can miss like any other seam.
  mutable std::uint64_t d_unrouted{0};
};

// Retain/release a version's state handle as its embedding `ContentRecord` is
// published and reclaimed (doc 14:173-176), routed to the content that owns it.
// WRITER/DRAIN-THREAD ONLY.
class EditableStateRefSink final : public StateRefSink {
public:
  explicit EditableStateRefSink(EditableRouter& router) noexcept : d_router(&router) {}

  void retain(ObjectId content, StateHandle handle) override {
    if (Editable* const editable = d_router->route(content, handle)) {
      editable->retain(handle);
    }
  }
  void release(ObjectId content, StateHandle handle) override {
    if (Editable* const editable = d_router->route(content, handle)) {
      editable->release(handle);
    }
  }

private:
  EditableRouter* d_router;
};

// The content's byte contribution to the journal's budget (doc 14:120-122): the
// journal knows record sizes at L2 and asks the owning content's facet for the
// state payload. An inert handle costs nothing and names no owner, so it short-
// circuits ahead of the route -- that is the inert path's byte-identical budget.
class EditableStateCostFn final : public StateCostFn {
public:
  explicit EditableStateCostFn(EditableRouter& router) noexcept : d_router(&router) {}

  std::size_t cost(ObjectId content, const StateHandle& handle) const override {
    if (!handle.has_state()) {
      return 0;
    }
    Editable* const editable = d_router->route(content, handle);
    return editable != nullptr ? editable->state_cost(handle) : 0;
  }

private:
  EditableRouter* d_router;
};

// Rebase the live content when a navigation publish (undo/redo) moves its state
// (doc 14:117). This seam always named its owner; now every seam does, so it
// routes through the same table as the rest instead of filtering against a single
// bound id. A navigation event for a non-editable content targets the inert
// handle and routes nowhere, exactly as its old owner filter did.
class EditableRestoreSink final : public RestoreSink {
public:
  explicit EditableRestoreSink(EditableRouter& router) noexcept : d_router(&router) {}

  void on_restore(ObjectId content, StateHandle target) override {
    if (Editable* const editable = d_router->route(content, target)) {
      editable->restore(target);
    }
  }

private:
  EditableRouter* d_router;
};

// The per-`Document` editable-content multiplexer: ONE sink trio for the whole
// document, installed onto the live `Model`/`Journal` at `attach()` and holding
// the routing table above. `bind()` is then a table insert, and every
// retain/release/cost/restore dispatches to the content that owns the handle. A
// document may hold any number of editable contents on its one journal.
//
// This lifts the v1 single-editable-content limit (`kinds.raster_runtime_binding`
// Constraint 9), which existed only because two of the four state seams had
// dropped the owning `ObjectId`, and a bare `StateHandle` cannot name its content.
// The seams now carry it, so the routing is exact rather than impossible.
//
// A trio PER CONTENT was rejected: the model/journal would then have to hold N
// sink pointers, pushing per-content knowledge into L2 and reintroducing the
// `Content`-vtable proximity doc 17:66-72 forbids. Routing in L5 keeps the
// model's half a single type-erased pointer and puts the N in the one component
// that already owns N contents.
//
// Lifetime: the binding must OUTLIVE the `Model`/`Journal` it registers onto,
// because the model's final reclamation drain (`~Model`) releases each content
// record's state handle THROUGH the installed sink. `Document` guarantees this by
// member declaration order; see `document.hpp`. Consequently `~EditableBinding`
// deliberately touches neither the model nor the journal -- their sink slots die
// with them. The explicit teardown is `unbind()` / `unbind_all()`.
class EditableBinding {
public:
  EditableBinding() = default;

  EditableBinding(const EditableBinding&) = delete;
  EditableBinding& operator=(const EditableBinding&) = delete;

  // Point the binding at the document's model and journal AND install the sink
  // trio -- once, for the document's whole life, however many editable contents it
  // goes on to hold. Called from the owning `Document`'s constructor (the binding
  // is declared before them so it outlives them, so it cannot take them as
  // constructor references). WRITER-THREAD ONLY.
  void attach(Model& model, Journal& journal) noexcept;

  // Route `id` to `content`'s state through the already-installed trio, and return
  // its `Editable` facet -- or NULLPTR for a content that has none (every leaf/live
  // kind), which routes NOTHING and keeps the byte-identical inert path
  // (doc 14:176-182). The caller publishes the returned facet's `capture()` onto
  // the content's record in the SAME transaction that mints it (see
  // `Document::add_content`), so the record names the content's real initial state
  // instead of an inert handle -- which is what gives the first edit an undo target
  // and what earns that state its first `retain`. WRITER-THREAD ONLY.
  Editable* bind(ObjectId id, Content& content);

  // Release ONE content's routing row: drain FIRST, with the row still installed,
  // so every reclaim the model has queued for that content still reaches its facet
  // and releases its pinned state handles (the doc 14:173-176 refcount promise
  // fires exactly once, at the reclaim boundary) -- and only then drop the row, so
  // nothing afterwards can route to it. Dropping the row first would strand the
  // content's queued releases and leak its pool blocks. The named, reusable
  // teardown a future per-content removal path drives. WRITER-THREAD ONLY.
  void unbind(ObjectId id);

  // Document-wide teardown: drain with the whole table live, then clear it and
  // uninstall the trio. `~Document` does NOT call this -- its declaration-order
  // contract keeps the binding alive through `~Model`'s final drain instead
  // (document.hpp). This is the explicit path; a later `attach()` re-installs.
  // WRITER-THREAD ONLY.
  void unbind_all();

  // --- Routing state + behavioral counters (doc 16), read by tests ---

  bool bound(ObjectId id) const noexcept { return d_router.contains(id); }
  std::size_t bound_count() const noexcept { return d_router.size(); }

  // State calls whose owning `ObjectId` had no row and carried real state -- the
  // Constraint-4 tripwire. Zero in any correct document.
  std::uint64_t unrouted_state_calls() const noexcept { return d_router.unrouted_state_calls(); }

  // Every call this binding has made to a `Model`/`Journal` state-seam setter:
  // three at `attach()`, three more at `unbind_all()`. It does NOT grow with the
  // number of bound contents -- that is the proof the trio is per-document rather
  // than per-content, and the counter a per-content implementation would fail.
  std::uint64_t seam_registrations() const noexcept { return d_registrations; }

private:
  Model* d_model{nullptr};
  Journal* d_journal{nullptr};

  EditableRouter d_router;
  EditableStateRefSink d_ref_sink{d_router};
  EditableStateCostFn d_cost_fn{d_router};
  EditableRestoreSink d_restore_sink{d_router};

  std::uint64_t d_registrations{0};
};

} // namespace arbc
