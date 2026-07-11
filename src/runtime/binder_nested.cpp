// The runtime binder for org.arbc.nested (kinds.nested_runtime_binding). The one
// runtime TU that legally names the concrete `NestedContent` type (doc 17:60), so the
// render drivers stay kind-agnostic and dispatch only through the binder registry
// (Constraint 2).
//
// Why its OWN TU rather than a codec TU (Decision 5): fade's and crossfade's binders
// ride their codec TUs because those already name their kind. Nested has no codec TU
// -- `runtime.nested_codec` is unlanded M8 work -- and putting an M8 task in front of
// this M9 one for a file-placement reason is not worth it. `runtime.nested_codec` can
// trivially absorb this file into `codec_nested.cpp` when it lands.
//
// Nested is the kind doc 17:143-148 has in mind when it says a kind needing
// render-time services is "constructed unattached across the boundary and the host
// injects its services afterwards": it takes all four of the `BindContext`'s fields
// -- the live `PullService` and `Backend`, the id->`Content*` resolver, and the
// DRIVER'S PIN (never a fresh one: nested reads the child's membership from the very
// snapshot the frame renders against, doc 05:71-75).

#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/operator_binding.hpp>

namespace arbc {

void register_nested_binder() {
  register_operator_binder(
      NestedContent::kind_id,
      OperatorBinder{[](Content& c, const BindContext& ctx) -> bool {
                       if (auto* nested = dynamic_cast<NestedContent*>(&c)) {
                         // `ContentResolver` and `NestedResolver` are the same
                         // `std::function<Content*(ObjectId)>`, so the copy needs no
                         // adapter -- and copying is what keeps the resolver alive
                         // past the caller's frame-local one.
                         nested->attach(ctx.pull, ctx.backend, ctx.resolve, ctx.doc);
                         return true;
                       }
                       return false;
                     },
                     [](Content& c) noexcept { static_cast<NestedContent&>(c).detach(); }});
}

} // namespace arbc
