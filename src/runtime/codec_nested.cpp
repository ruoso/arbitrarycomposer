// The org.arbc.nested built-in codec (runtime.nested_codec) and, beside it, the runtime
// binder for the same kind. A runtime TU that alone sees both the concrete kind type (L4
// kind_nested) and nlohmann::json (via L4 serialize, PRIVATE link) -- the sanctioned home
// for a built-in codec (doc 08 Principle 1, doc 17:60). The binder lived in its own
// `binder_nested.cpp` only because this TU did not exist yet
// (`kinds.nested_runtime_binding` Decision 5 anticipated the absorption); fade and
// crossfade each keep their binder in their codec TU for the same reason, and nested now
// matches.
//
// The thinnest codec in the tree, because `NestedContent` has NO params at all: its child
// `ObjectId` is its entire state, and that reference is CORE-owned graph structure (doc 08
// Principle 7). The core reads it off `Content::composition_ref()` and re-derives the
// emitted `"composition"` id from the document's shape on every save, so `serialize_nested`
// neither writes it nor could fight it -- it returns an empty `params` object. On the read
// side the reader has already pre-allocated the child's `ObjectId` (its `CompResolver`
// runs ahead of the codec, which is why a Droste back-edge costs nothing) and hands it in,
// so `deserialize_nested` simply builds around it.
//
// Emitting `params` as an empty object rather than omitting it is what gives nested free
// unknown-`params` preservation through the core's load-time residual diff
// (`codec.cpp`): a hand-authored `params.ref` naming an EXTERNAL child -- loader territory
// until `runtime.nested_external_ref` lands (doc 05:54-61) -- round-trips verbatim rather
// than being silently dropped.

#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/operator_binding.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <string>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// Nested owns no `params`: the child reference is core-owned and the writer appends it
// AFTER this returns (Constraint 1). A non-nested content is a codec/kind-routing
// mismatch -> CodecFailed as a value, exactly as every sibling codec reports it.
expected<json, SerializeError> serialize_nested(const Content& content) {
  if (dynamic_cast<const NestedContent*>(&content) == nullptr) {
    return unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
  }
  return json::object();
}

// Rebuild a `NestedContent` around the child `ObjectId` the reader already resolved and
// pre-allocated. The codec interns nothing and resolves nothing: an id naming a
// composition absent from the table is already `UnresolvableReference` ahead of this call
// (doc 08:184-186), so the only failure left to report is an ABSENT child -- a nested
// content with nothing to nest, which would round-trip as the empty placeholder forever
// (Decision 3). `inputs` is never consumed: nested's inputs are derived from the child
// composition, and the reader rejects a body carrying both edge sets before we are called.
expected<std::unique_ptr<Content>, ReaderError> deserialize_nested(const json& /*params*/,
                                                                   std::span<const ContentRef>,
                                                                   ObjectId composition,
                                                                   LoadContext& /*ctx*/) {
  if (!composition.valid()) {
    return read_fail(ReaderError::Kind::MissingRequiredField, "/composition");
  }
  return std::unique_ptr<Content>(std::make_unique<NestedContent>(composition));
}

} // namespace

Codec nested_codec() { return Codec{serialize_nested, deserialize_nested}; }

void register_nested_binder() {
  // The typed match lives here, the one runtime TU that names `NestedContent` (doc
  // 17:60), so the render drivers stay kind-agnostic and dispatch only through the binder
  // registry. Nested is the kind doc 17:143-148 has in mind when it says a kind needing
  // render-time services is "constructed unattached across the boundary and the host
  // injects its services afterwards": it takes all four of the `BindContext`'s fields --
  // the live `PullService` and `Backend`, the id->`Content*` resolver, and the DRIVER'S
  // PIN (never a fresh one: nested reads the child's membership from the very snapshot
  // the frame renders against, doc 05:71-75).
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
