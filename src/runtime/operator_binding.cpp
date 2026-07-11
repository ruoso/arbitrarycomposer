#include <arbc/contract/content.hpp> // Content, ContentRef, PullService
#include <arbc/runtime/document.hpp> // Document
#include <arbc/runtime/operator_binding.hpp>

#include <cassert>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace arbc {

// Defined by the TUs that legally name a concrete kind type (doc 17:60): the operator
// codec TUs (`codec_fade.cpp` / `codec_crossfade.cpp`) and, for `org.arbc.nested`
// -- which has no codec TU until `runtime.nested_codec` lands -- its own
// `binder_nested.cpp`. Forward-declared here to avoid pulling the JSON-bearing
// `builtin_codecs.hpp` into this generic TU; a sibling runtime-binding task adds its
// own `register_*_binder()` beside these calls.
void register_fade_binder();
void register_crossfade_binder();
void register_nested_binder();

namespace {

// Process-global operator-binder registry. Keyed by the durable `kind_id` string
// (the interned `uint64` is per-load, so unusable as a stable key). WRITTEN only
// during the one-time `register_builtin_operator_binders` init (guarded below) and
// READ-ONLY thereafter, so `bind_operators` needs no lock: the binding is written on
// the driver thread before any worker dispatch and read-only on workers
// (Constraint 8).
std::vector<std::pair<std::string_view, OperatorBinder>>& registry() {
  static std::vector<std::pair<std::string_view, OperatorBinder>> r;
  return r;
}

} // namespace

void register_operator_binder(const char* kind_id, OperatorBinder binder) {
  auto& r = registry();
  const std::string_view key{kind_id};
  for (const auto& entry : r) {
    if (entry.first == key) {
      return; // idempotent: first registration wins
    }
  }
  r.emplace_back(key, binder);
}

void register_builtin_operator_binders() {
  // Once, thread-safe: function-local-static initialization runs the registration
  // exactly once across all threads, so the registry is fully written before any
  // concurrent read and needs no further synchronization.
  static const bool once = [] {
    register_fade_binder();
    register_crossfade_binder();
    register_nested_binder();
    return true;
  }();
  (void)once;
}

OperatorBindingScope::OperatorBindingScope(OperatorBindingScope&& other) noexcept
    : d_bound(std::move(other.d_bound)), d_pin(std::move(other.d_pin)) {
  other.d_bound.clear();
  other.d_pin.reset();
}

OperatorBindingScope& OperatorBindingScope::operator=(OperatorBindingScope&& other) noexcept {
  if (this != &other) {
    release();
    d_bound = std::move(other.d_bound);
    d_pin = std::move(other.d_pin);
    other.d_bound.clear();
    other.d_pin.reset();
  }
  return *this;
}

OperatorBindingScope::~OperatorBindingScope() { release(); }

void OperatorBindingScope::release() noexcept {
  // Detach in reverse attach order (symmetry; each detach only clears its own
  // content's borrowed pointers, so order is not load-bearing).
  for (auto it = d_bound.rbegin(); it != d_bound.rend(); ++it) {
    it->second(*it->first);
  }
  d_bound.clear();
  // Only now, with every borrowed `const DocRoot*` cleared, may the snapshot go
  // (Constraint 4): the pin outlives every content it was injected into.
  d_pin.reset();
}

void OperatorBindingScope::record(Content* content, void (*detach)(Content&) noexcept) {
  d_bound.emplace_back(content, detach);
}

OperatorBindingScope bind_operators(const Document& document, PullService& pull, Backend& backend,
                                    DocStatePtr pin) {
  assert(pin != nullptr && "bind_operators requires the driver's pin (doc 05:71-75)");
  OperatorBindingScope scope;
  // Synthesize the id->Content resolver from the document's side-map -- the binding
  // the model deliberately does not hold (doc 17:66-72) -- so no driver has to pass
  // one it already owns. The `BindContext` borrows it; a thunk that keeps it (nested)
  // COPIES the `std::function` into its own `NestedResolver`, so nothing dangles when
  // this frame's `resolve` dies.
  const ContentResolver resolve = [&document](ObjectId id) { return document.resolve(id); };
  const BindContext ctx{pull, backend, resolve, *pin};
  // The scope owns the snapshot for as long as any content borrows a `const DocRoot&`
  // into it.
  scope.adopt_pin(std::move(pin));
  std::unordered_set<Content*> visited;
  const auto& r = registry();

  // Depth-first over the content graph: each top-level content and, transitively,
  // every `inputs()` child. Dedup so a content reachable by two paths binds once.
  // ATTACH BEFORE RECURSING (Constraint 5): a nested content's `inputs()` is empty
  // until its resolver and pin are injected, so this order -- not the reverse -- is
  // what lets the walk reach a content living inside a nested child composition.
  std::function<void(Content*)> walk = [&](Content* c) {
    if (c == nullptr || !visited.insert(c).second) {
      return;
    }
    for (const auto& entry : r) {
      if (entry.second.try_attach(*c, ctx)) {
        scope.record(c, entry.second.detach);
        break; // a content matches at most one kind
      }
    }
    for (const ContentRef in : c->inputs()) {
      walk(in);
    }
  };

  document.for_each_content([&](Content* c) { walk(c); });
  return scope;
}

} // namespace arbc
