#include <arbc/contract/content.hpp> // Content, ContentRef, PullService
#include <arbc/runtime/document.hpp> // Document
#include <arbc/runtime/operator_binding.hpp>

#include <functional>
#include <string_view>
#include <unordered_set>

namespace arbc {

// Defined by the operator codec TUs (`codec_fade.cpp` / `codec_crossfade.cpp`, the
// only runtime TUs that legally name the concrete `FadeContent` / `CrossfadeContent`
// types, doc 17:60). Forward-declared here to avoid pulling the JSON-bearing
// `builtin_codecs.hpp` into this generic TU; a sibling runtime-binding task adds its
// own `register_*_binder()` beside these calls.
void register_fade_binder();
void register_crossfade_binder();

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
    return true;
  }();
  (void)once;
}

OperatorBindingScope::OperatorBindingScope(OperatorBindingScope&& other) noexcept
    : d_bound(std::move(other.d_bound)) {
  other.d_bound.clear();
}

OperatorBindingScope& OperatorBindingScope::operator=(OperatorBindingScope&& other) noexcept {
  if (this != &other) {
    release();
    d_bound = std::move(other.d_bound);
    other.d_bound.clear();
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
}

void OperatorBindingScope::record(Content* content, void (*detach)(Content&) noexcept) {
  d_bound.emplace_back(content, detach);
}

OperatorBindingScope bind_operators(const Document& document, PullService& pull, Backend& backend) {
  OperatorBindingScope scope;
  std::unordered_set<Content*> visited;
  const auto& r = registry();

  // Depth-first over the content graph: each top-level content and, transitively,
  // every `inputs()` child. Dedup so a content reachable by two paths binds once.
  std::function<void(Content*)> walk = [&](Content* c) {
    if (c == nullptr || !visited.insert(c).second) {
      return;
    }
    for (const auto& entry : r) {
      if (entry.second.try_attach(*c, pull, backend)) {
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
