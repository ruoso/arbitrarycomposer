#pragma once

#include <arbc/arbc_api.h>
#include <arbc/compositor/compositor.hpp> // ContentResolver
#include <arbc/model/model.hpp>           // DocRoot, DocStatePtr

#include <cstddef>
#include <utility>
#include <vector>

// The runtime-side content binding seam (`operators.fade_runtime_binding`,
// `kinds.nested_runtime_binding`, doc 13:69-71, doc 17:60/66-72/143-148). A content
// that needs RENDER-TIME SERVICES -- doc 17:143-148 names exactly `{kind-fade,
// kind-crossfade, kind-nested}` -- is constructed unattached (the `Registry` factory
// carries no service handles) and borrows its services at attach: an operator
// (`org.arbc.fade`, ...) borrows a live `PullService` + `Backend` and pulls its input
// only through them (doc 13:69-83); `org.arbc.nested` additionally borrows an
// id->`Content*` resolver and the driver's PINNED `DocRoot` (doc 05:71-75). No
// production code attached any of them before this seam -- every attach lived in a
// test double. This header lets the one runtime TU that legally sees a concrete kind
// (its codec TU, or `binder_nested.cpp` for the kind that has none yet, doc 17:60)
// register a typed attach/detach thunk, and lets the render drivers bind every such
// content in a document to their live services before render and tear the binding
// down on scope exit (Constraint 3), all without the driver naming any concrete kind
// (Decision 2). The `OperatorBinder`/`bind_operators` names are historical: the
// registry binds operator AND kind contents alike.
//
// Why the dispatch is a registered typed thunk and not a driver-side cast chain or
// a model-kind lookup: a document's interned `uint64` kind is per-load and
// non-durable (`document_serialize` `KindBridge`), so it is no use as a render-time
// key; the only render-time kind knowledge is the concrete C++ type, which only the
// codec TU may name. Registering the typed match there keeps the driver kind-
// agnostic -- a sibling runtime-binding task registers its kind's thunk without
// touching any driver (Constraint 7).

namespace arbc {

class Content;
class PullService;
class Backend;
class Document;

// The render-time services a binder injects into one content
// (`kinds.nested_runtime_binding` Decision 2). Every field is BORROWED for the life
// of the binding scope and no longer:
//  - `pull`/`backend`  the driver's live services (every operator kind needs these);
//  - `resolve`         the id->`Content*` binding, synthesized by `bind_operators`
//                      from `Document::resolve` (the same `std::function<Content*(
//                      ObjectId)>` type as `NestedResolver`, so a thunk converts with
//                      no adapter). The binding lives in `runtime` by construction
//                      (doc 17:66-72), which is why a kind takes it through attach;
//  - `doc`            the DRIVER'S PIN -- never a freshly-pinned snapshot. A nested
//                     content reads its child's membership from the same version the
//                     frame renders against, or a Droste scene is not self-consistent
//                     within the frame (doc 05:71-75). The owning `OperatorBindingScope`
//                     holds the `DocStatePtr`, so the snapshot outlives every content
//                     it was injected into (Constraint 4).
struct BindContext {
  PullService& pull;
  Backend& backend;
  const ContentResolver& resolve;
  const DocRoot& doc;
};

// A per-kind attach/detach thunk pair, registered by the one runtime TU that sees the
// concrete kind type. `try_attach` injects `ctx` into `content` IFF `content` is this
// binder's kind (a typed match, mirroring the codec's own `dynamic_cast`, e.g.
// `codec_fade.cpp` `serialize_fade`), returning true; a non-matching content is left
// untouched and returns false. A thunk takes only the services its kind's `attach`
// declares -- fade ignores `ctx.resolve`/`ctx.doc`. `detach` clears the borrowed
// pointers a prior matched `try_attach` set (its precondition is that `content` is
// this binder's kind, so it may `static_cast`).
struct OperatorBinder {
  bool (*try_attach)(Content& content, const BindContext& ctx);
  void (*detach)(Content& content) noexcept;
};

// Register `binder` for `kind_id` (the durable reverse-DNS string, e.g.
// `FadeContent::kind_id`). Idempotent per `kind_id` (first registration wins). Not
// itself thread-safe: reach it only through `register_builtin_operator_binders`,
// which runs the registration exactly once under a thread-safe guard.
ARBC_API void register_operator_binder(const char* kind_id, OperatorBinder binder);

// Register every built-in operator kind's binder exactly once, thread-safely (a
// function-local-static guard). The render drivers call this before `bind_operators`
// so the registry is fully populated (and read-only) before any bind or worker
// dispatch.
ARBC_API void register_builtin_operator_binders();

// The RAII binding scope (Decision 1): owns the set of contents attached by
// `bind_operators` and detaches every one on destruction, so no borrowed service is
// dereferenced after the scope (the frame or the driver) ends (Constraint 3). It
// also OWNS THE PIN it injected (`kinds.nested_runtime_binding` Constraint 4), so
// the snapshot a nested content borrows cannot die under it and no caller can forget
// to keep it alive.
// The live services MUST outlive this scope (Constraint 4): declare the
// `PullServiceImpl` and `Backend` BEFORE the scope so they destruct AFTER it.
class ARBC_API OperatorBindingScope {
public:
  OperatorBindingScope() = default;
  OperatorBindingScope(OperatorBindingScope&& other) noexcept;
  OperatorBindingScope& operator=(OperatorBindingScope&& other) noexcept;
  OperatorBindingScope(const OperatorBindingScope&) = delete;
  OperatorBindingScope& operator=(const OperatorBindingScope&) = delete;
  ~OperatorBindingScope();

  // The number of contents currently bound (observability for the teardown test:
  // nonzero after a bind, and every one cleared once the scope is released).
  std::size_t size() const noexcept { return d_bound.size(); }

  // Detach every bound content now, drop the pin, and empty the scope (idempotent).
  // The destructor calls this; a caller may end the binding early. The pin is
  // released only AFTER every borrowed `DocRoot` pointer has been cleared.
  void release() noexcept;

  // Record that `content` was attached and must be cleared with `detach` (used by
  // `bind_operators`).
  void record(Content* content, void (*detach)(Content&) noexcept);

  // Take ownership of the snapshot injected into the bound contents (used by
  // `bind_operators`).
  void adopt_pin(DocStatePtr pin) noexcept { d_pin = std::move(pin); }

private:
  std::vector<std::pair<Content*, void (*)(Content&) noexcept>> d_bound;
  // The snapshot every bound content borrows a `const DocRoot&` into. Held so it
  // outlives the bindings (Constraint 4); dropped by `release()` after the detaches.
  DocStatePtr d_pin;
};

// Attach the driver's live services onto every registered-kind content reachable in
// `document`: each top-level content (`Document::for_each_content`) and,
// transitively, every `Content::inputs()` child (so an operator nested as an input is
// bound too), each bound at most once. A content is attached BEFORE the walk recurses
// into its `inputs()` -- load-bearing, not incidental: `NestedContent::inputs()` is
// empty until nested has its resolver and pin, so attach-then-recurse is what lets the
// walk see THROUGH a nested boundary into the child composition's own contents
// (Constraint 5).
//
// `pin` is the DRIVER'S snapshot -- the same one the frame renders against -- and is a
// required parameter rather than something this function pins for itself: re-pinning
// here could hand a nested content a NEWER revision than the frame is rendering, the
// snapshot inconsistency doc 05:71-75 forbids. The returned scope owns it. The
// id->`Content*` resolver is synthesized internally from `Document::resolve`, so no
// driver gains a parameter it does not already own.
//
// Dispatch is by the registered `OperatorBinder`s, so the driver names no concrete
// kind (Decision 2); a content of no registered kind is skipped. When the document
// carries a borrowed `Registry` (`Document::registry()`,
// `runtime.plugin_operator_registration` Decision 4), each content the global
// built-ins do not match is then offered to the registry-carried plugin
// `KindBinder`s -- the only binders that can match a kind whose concrete type
// lives in a plugin image -- with the contract-expressible `{pull, backend}`
// services only. Returns the RAII scope that tears every binding down on
// destruction (Constraint 3). Call `register_builtin_operator_binders()` first.
ARBC_API OperatorBindingScope bind_operators(const Document& document, PullService& pull,
                                             Backend& backend, DocStatePtr pin);

} // namespace arbc
