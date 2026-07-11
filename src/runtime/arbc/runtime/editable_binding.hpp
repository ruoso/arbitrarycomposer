#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/model/journal.hpp> // StateCostFn, RestoreSink
#include <arbc/model/model.hpp>   // Model, StateRefSink
#include <arbc/model/records.hpp> // StateHandle

#include <cstddef>
#include <optional>

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

// Retain/release a version's state handle as its embedding `ContentRecord` is
// published and reclaimed (doc 14:173-176). WRITER/DRAIN-THREAD ONLY.
class EditableStateRefSink final : public StateRefSink {
public:
  explicit EditableStateRefSink(Editable& editable) noexcept : d_editable(&editable) {}

  void retain(StateHandle handle) override { d_editable->retain(handle); }
  void release(StateHandle handle) override { d_editable->release(handle); }

private:
  Editable* d_editable;
};

// The content's byte contribution to the journal's budget (doc 14:120-122): the
// journal knows record sizes at L2 and asks the facet for the state payload.
class EditableStateCostFn final : public StateCostFn {
public:
  explicit EditableStateCostFn(Editable& editable) noexcept : d_editable(&editable) {}

  std::size_t cost(const StateHandle& handle) const override {
    return d_editable->state_cost(handle);
  }

private:
  Editable* d_editable;
};

// Rebase the live content when a navigation publish (undo/redo) moves its state
// (doc 14:117). Bound to the owning `ObjectId` because `RestoreSink` is the one
// seam that names the content; a navigation event for any other object is a
// no-op here.
class EditableRestoreSink final : public RestoreSink {
public:
  EditableRestoreSink(Editable& editable, ObjectId owner) noexcept
      : d_editable(&editable), d_owner(owner) {}

  void on_restore(ObjectId content, StateHandle target) override {
    if (content == d_owner) {
      d_editable->restore(target);
    }
  }

private:
  Editable* d_editable;
  ObjectId d_owner;
};

// The per-`Document` editable-content binding: installs the three adapters above
// onto the live `Model`/`Journal` when an editable content is instantiated, and
// clears them when it is released.
//
// v1 binds ONE editable content per document. The model/journal seams are single
// pointer slots and a bare `StateHandle` does not name its owning content
// (`records.hpp`: it is just a slot index, local to that content's store), so a
// second binding could not route retain/release/cost back to the right content.
// `bind()` therefore THROWS `std::logic_error` on a second editable content
// rather than silently overwriting the first and misrouting its state lifecycle
// (doc 16: never a silent truncation). Lifting the limit -- by tagging the
// handle with its owner, or carrying the `ObjectId` on the retain/cost seams as
// `RestoreSink` already does -- is `runtime.editable_sink_multiplex`.
//
// Lifetime: the binding must OUTLIVE the `Model`/`Journal` it registers onto,
// because the model's final reclamation drain (`~Model`) releases each content
// record's state handle THROUGH the installed sink. `Document` guarantees this by
// member declaration order; see `document.hpp`. Consequently `~EditableBinding`
// deliberately touches neither the model nor the journal -- their sink slots die
// with them. The explicit teardown is `unbind()`.
class EditableBinding {
public:
  EditableBinding() = default;

  EditableBinding(const EditableBinding&) = delete;
  EditableBinding& operator=(const EditableBinding&) = delete;

  // Point the binding at the document's model and journal. Called once, from the
  // owning `Document`'s constructor (the binding is declared before them so it
  // outlives them, so it cannot take them as constructor references).
  void attach(Model& model, Journal& journal) noexcept;

  // Register `content`'s state sinks onto the live model/journal, bound to the
  // just-minted `id`, and return its `Editable` facet -- or NULLPTR for a content
  // that has none (every leaf/live kind), which registers NOTHING and keeps the
  // byte-identical inert path (doc 14:176-182). The caller publishes the returned
  // facet's `capture()` onto the content's record in the SAME transaction that
  // mints it (see `Document::add_content`), so the record names the content's real
  // initial state instead of an inert handle -- which is what gives the first edit
  // an undo target and what earns that state its first `retain`.
  //
  // Throws `std::logic_error` if a different editable content is already bound
  // (the v1 single-editable-content limit above). WRITER-THREAD ONLY.
  Editable* bind(ObjectId id, Content& content);

  // Release the binding: drain FIRST, with the sinks still installed, so the
  // reclamation of the unbound content's records still releases their pinned
  // state handles through the live sink (the doc 14:173-176 refcount promise
  // fires exactly once, at the reclaim boundary) -- and only then clear the three
  // slots, so nothing calls through a stale sink afterwards. The named, reusable
  // teardown a future per-content removal path drives. WRITER-THREAD ONLY.
  void unbind();

  bool bound() const noexcept { return d_ref_sink.has_value(); }
  // The bound content's id (a default `ObjectId` when nothing is bound).
  ObjectId owner() const noexcept { return d_owner; }

private:
  Model* d_model{nullptr};
  Journal* d_journal{nullptr};
  ObjectId d_owner{};

  std::optional<EditableStateRefSink> d_ref_sink;
  std::optional<EditableStateCostFn> d_cost_fn;
  std::optional<EditableRestoreSink> d_restore_sink;
};

} // namespace arbc
