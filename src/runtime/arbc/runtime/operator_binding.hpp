#pragma once

#include <cstddef>
#include <utility>
#include <vector>

// The runtime-side operator binding seam (`operators.fade_runtime_binding`, doc
// 13:69-71, doc 17:60/66-72). An operator content (`org.arbc.fade`, ...) borrows a
// live `PullService` + `Backend` at attach and pulls its input only through them
// (doc 13:69-83); no production code attached them before this seam -- every attach
// lived in a test double. This header lets the one runtime TU that legally sees a
// concrete operator kind (its codec TU, doc 17:60) register a typed attach/detach
// thunk, and lets the render drivers bind every operator content in a document to
// their live services before render and tear the binding down on scope exit
// (Constraint 3), all without the driver naming any concrete kind (Decision 2).
//
// Why the dispatch is a registered typed thunk and not a driver-side cast chain or
// a model-kind lookup: a document's interned `uint64` kind is per-load and
// non-durable (`document_serialize` `KindBridge`), so it is no use as a render-time
// key; the only render-time kind knowledge is the concrete C++ type, which only the
// codec TU may name. Registering the typed match there keeps the driver kind-
// agnostic -- a sibling runtime-binding task (`operators.crossfade_runtime_binding`,
// `kinds.*_runtime_binding`) registers its kind's thunk without touching any driver
// (Constraint 7).

namespace arbc {

class Content;
class PullService;
class Backend;
class Document;

// A per-operator-kind attach/detach thunk pair, registered by the codec TU that
// sees the concrete kind type. `try_attach` attaches `pull`/`backend` onto
// `content` IFF `content` is this binder's kind (a typed match, mirroring the
// codec's own `dynamic_cast`, e.g. `codec_fade.cpp` `serialize_fade`), returning
// true; a non-matching content is left untouched and returns false. `detach` clears
// the borrowed pointers a prior matched `try_attach` set (its precondition is that
// `content` is this binder's kind, so it may `static_cast`).
struct OperatorBinder {
  bool (*try_attach)(Content& content, PullService& pull, Backend& backend);
  void (*detach)(Content& content) noexcept;
};

// Register `binder` for `kind_id` (the durable reverse-DNS string, e.g.
// `FadeContent::kind_id`). Idempotent per `kind_id` (first registration wins). Not
// itself thread-safe: reach it only through `register_builtin_operator_binders`,
// which runs the registration exactly once under a thread-safe guard.
void register_operator_binder(const char* kind_id, OperatorBinder binder);

// Register every built-in operator kind's binder exactly once, thread-safely (a
// function-local-static guard). The render drivers call this before `bind_operators`
// so the registry is fully populated (and read-only) before any bind or worker
// dispatch.
void register_builtin_operator_binders();

// The RAII binding scope (Decision 1): owns the set of operator contents attached
// by `bind_operators` and detaches every one on destruction, so no borrowed service
// is dereferenced after the scope (the frame or the driver) ends (Constraint 3).
// The live services MUST outlive this scope (Constraint 4): declare the
// `PullServiceImpl` and `Backend` BEFORE the scope so they destruct AFTER it.
class OperatorBindingScope {
public:
  OperatorBindingScope() = default;
  OperatorBindingScope(OperatorBindingScope&& other) noexcept;
  OperatorBindingScope& operator=(OperatorBindingScope&& other) noexcept;
  OperatorBindingScope(const OperatorBindingScope&) = delete;
  OperatorBindingScope& operator=(const OperatorBindingScope&) = delete;
  ~OperatorBindingScope();

  // The number of operator contents currently bound (observability for the teardown
  // test: nonzero after a bind, and every one cleared once the scope is released).
  std::size_t size() const noexcept { return d_bound.size(); }

  // Detach every bound content now and empty the scope (idempotent). The destructor
  // calls this; a caller may end the binding early.
  void release() noexcept;

  // Record that `content` was attached and must be cleared with `detach` (used by
  // `bind_operators`).
  void record(Content* content, void (*detach)(Content&) noexcept);

private:
  std::vector<std::pair<Content*, void (*)(Content&) noexcept>> d_bound;
};

// Attach the driver's live `pull`/`backend` onto every registered-kind operator
// content reachable in `document`: each top-level content
// (`Document::for_each_content`) and, transitively, every `Content::inputs()` child
// (so an operator nested as an input is bound too), each bound at most once.
// Dispatch is by the registered `OperatorBinder`s, so the driver names no concrete
// kind (Decision 2); a content of no registered kind is skipped. Returns the RAII
// scope that tears every binding down on destruction (Constraint 3). Call
// `register_builtin_operator_binders()` first.
OperatorBindingScope bind_operators(const Document& document, PullService& pull, Backend& backend);

} // namespace arbc
