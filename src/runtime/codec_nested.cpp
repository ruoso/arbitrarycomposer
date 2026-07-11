// The org.arbc.nested built-in codec (runtime.nested_codec) and, beside it, the runtime
// binder for the same kind. A runtime TU that alone sees both the concrete kind type (L4
// kind_nested) and nlohmann::json (via L4 serialize, PRIVATE link) -- the sanctioned home
// for a built-in codec (doc 08 Principle 1, doc 17:60). The binder lived in its own
// `binder_nested.cpp` only because this TU did not exist yet
// (`kinds.nested_runtime_binding` Decision 5 anticipated the absorption); fade and
// crossfade each keep their binder in their codec TU for the same reason, and nested now
// matches.
//
// Nested's child comes to it one of exactly TWO ways, and the split is the whole of the
// codec (doc 08 Principle 7 vs Principle 3):
//
//  - an IN-DOCUMENT child is CORE-owned graph structure. The core reads it off
//    `Content::composition_ref()` and re-derives the emitted `"composition"` id from the
//    document's shape on every save, so `serialize_nested` neither writes it nor could
//    fight it; on the read side the reader has already pre-allocated the child's
//    `ObjectId` (its `CompResolver` runs ahead of the codec, which is why a Droste
//    back-edge costs nothing) and hands it in as the `composition` argument.
//
//  - an EXTERNAL child is KIND-owned: a `params.ref` URI naming another `.arbc` (doc
//    05:47-61). `params` is the one thing the core does not own, so both halves of that
//    reference are this TU's: `serialize_nested` emits `{"ref": ...}` verbatim as the
//    document authored it -- never an absolutised path, so a project directory stays
//    relocatable and the output stays byte-stable -- and `deserialize_nested` consumes it,
//    driving the `ExternalCompositionLoader` through the `LoadContext` to install the
//    referenced document's composition graph into THIS document's model and taking its
//    root id as the child (runtime.nested_external_ref Decision 1).
//
// A body naming its child BOTH ways -- a core-owned `composition` AND a `params.ref` --
// asserts two contradictory things and is rejected as a value (Decision 7). The check
// lives here, not in the core reader, because `params` is kind territory and the core may
// not read its semantics.
//
// An UNRESOLVABLE `ref` (no asset source, a missing file, a depth-cap overrun, unparseable
// bytes) is NOT an error: the content is built with a null child, keeps its `ref`, renders
// the doc-05 placeholder, and the parent load succeeds (Decision 6). This is why a `ref`
// with no `composition` does not trip the missing-child rejection below -- nothing is
// lost, the file simply is not there right now.

#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/external_composition_loader.hpp>
#include <arbc/runtime/operator_binding.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <string>
#include <utility>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// An IN-DOCUMENT child rides no `params` at all: its reference is core-owned and the
// writer appends it AFTER this returns (Constraint 1), so the params object is empty. An
// EXTERNAL child rides `params.ref` and nothing else -- the AUTHORED string, byte-for-byte
// as the document said it, so `save(load(bytes)) == bytes` for a relative reference
// (Constraint 4). Emitting the RESOLVED URI instead would make the output depend on where
// the project directory happens to sit on disk.
//
// Whether the reference actually LOADED is invisible here (the content holds its `ref`
// either way), which is Constraint 9's second half: a document saved with the widget file
// missing is byte-identical to the same document saved with it present.
//
// A non-nested content is a codec/kind-routing mismatch -> CodecFailed as a value, exactly
// as every sibling codec reports it.
expected<json, SerializeError> serialize_nested(const Content& content) {
  const auto* const nested = dynamic_cast<const NestedContent*>(&content);
  if (nested == nullptr) {
    return unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
  }
  json params = json::object();
  if (!nested->ref().empty()) {
    params["ref"] = nested->ref();
  }
  return params;
}

// Rebuild a `NestedContent` around either the child `ObjectId` the reader already resolved
// and pre-allocated (in-document), or the composition the loader installs from the `.arbc`
// that `params.ref` names (external).
//
// `loader` is bound by CLOSURE at load-codec-table build time (`document_serialize.cpp`),
// not by a `DeserializeFn` parameter: a structural seam that ONE kind needs does not belong
// in the signature every kind implements (Constraint 3, following runtime.operator_codecs
// Decision 2). A null loader -- the save-path codec table, and any caller that assembled
// one without a load -- makes every external reference unavailable, which is the same
// benign degradation as an absent `AssetSource`.
//
// A present-but-non-string `ref` is treated LENIENTLY as absent: it is then a param the
// codec did not consume, so it rides the core's residual diff and round-trips verbatim,
// and no hostile input can turn a mistyped key into a load failure.
//
// `inputs` is never consumed: nested's inputs are derived from the child composition, and
// the reader rejects a body carrying both edge sets before we are called.
expected<std::unique_ptr<Content>, ReaderError>
deserialize_nested(const json& params, std::span<const ContentRef>, ObjectId composition,
                   LoadContext& ctx, ExternalCompositionLoader* loader) {
  std::string ref;
  if (const auto it = params.find("ref"); it != params.end() && it->is_string()) {
    ref = it->get<std::string>();
  }
  if (!ref.empty()) {
    // One child, named two contradictory ways (Decision 7). Rejecting beats silently
    // preferring one and dropping the other -- doc 08 states the preference twice, and
    // nested_codec Decision 2 already made the same call for `composition` + `inputs`.
    if (composition.valid()) {
      return read_fail(ReaderError::Kind::MalformedField, "/params/ref");
    }
    // An unavailable reference yields `ObjectId{}` -- a null child, the doc-05 placeholder,
    // and a parent load that still succeeds. That does NOT contradict the missing-child
    // rejection below: that one rejects a body naming NO child at all (silent data loss),
    // whereas here the `ref` is present, preserved, and re-saved byte-identically.
    const ObjectId child = (loader != nullptr) ? loader->load(ctx, ref) : ObjectId{};
    return std::unique_ptr<Content>(std::make_unique<NestedContent>(child, std::move(ref)));
  }
  // The codec interns nothing and resolves nothing for an in-document child: an id naming a
  // composition absent from the table is already `UnresolvableReference` ahead of this call
  // (doc 08:184-186), so the only failure left to report is an ABSENT child -- a nested
  // content with nothing to nest, which would round-trip as the empty placeholder forever
  // (Decision 3).
  if (!composition.valid()) {
    return read_fail(ReaderError::Kind::MissingRequiredField, "/composition");
  }
  return std::unique_ptr<Content>(std::make_unique<NestedContent>(composition));
}

} // namespace

Codec nested_codec() { return nested_codec(nullptr); }

Codec nested_codec(ExternalCompositionLoader* loader) {
  return Codec{serialize_nested,
               [loader](const json& params, std::span<const ContentRef> inputs,
                        ObjectId composition,
                        LoadContext& ctx) -> expected<std::unique_ptr<Content>, ReaderError> {
                 return deserialize_nested(params, inputs, composition, ctx, loader);
               }};
}

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
