#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/model/model.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/unknown_fields.hpp>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace arbc {

class CodecTable; // serialize-internal (codec.hpp): names nlohmann::json, so it is
                  // only forward-declared here and stays off the public header.

// The reader's error value (doc 08 Principle 5; doc 10 errors-as-values,
// 10:15-17), the read-side twin of `SerializeError`. Every failure -- malformed
// JSON, an unknown format major, a missing or mistyped required field, an
// unresolvable reference -- surfaces here as a value; no nlohmann exception ever
// crosses this boundary, on well-formed or hostile input alike (the precondition
// serialize.format_tests' loader fuzzing relies on).
struct ReaderError {
  enum class Kind {
    MalformedJson,         // the input was not parseable JSON
    UnknownFormatMajor,    // arbc.format is a major this reader does not know
    MissingRequiredField,  // a structurally required key was absent
    MalformedField,        // a present key had the wrong JSON type / shape
    UnresolvableReference, // a reference did not resolve (serialize.sharing / kinds.nested)
  };
  Kind kind{Kind::MalformedJson};
  std::string path;  // the JSON pointer to the offending node, for diagnostics
  ObjectId object{}; // the offending object, when known

  friend bool operator==(const ReaderError&, const ReaderError&) = default;
};

// Load one canonical `.arbc` JSON document into a fresh model as its version-0
// baseline -- the exact inverse of `serialize_document` for the envelope
// (`{"arbc":{"format":1}}`), the `composition` object
// (`working_space`/`canvas`/`working_audio_format`), and the bottom-to-top
// `layers` array with their core-owned placement, supplying each field's
// still/identity default where the writer omitted the key (doc 08:21-55). Parses
// with the non-throwing nlohmann overload, validates the `arbc.format` major
// (rejecting unknown majors with NO document mutation, Principle 4), and installs
// the reconstructed graph via `Model::load_baseline` at `revision 0` with an
// empty journal (doc 14:263-264).
//
// `registry` and `ctx` are the seams the content half of the read path
// (serialize.kind_params / .sharing) threads through -- the registry factory for
// `kind`/`params` -> Content, and the `LoadContext` for base-URI resolution +
// async asset loading + resolved-identity dedup. The canonical writer emits no
// content body, `inputs`, or `$ref` table today, so this v1 reader reconstructs
// only core placement and does not yet consult them (serialize.reader Decision 5).
//
// Returns success, or a `ReaderError` value; on any error the target `Model` is
// left unmutated (still `revision() == 0`, empty).
expected<std::monostate, ReaderError> load_document(std::string_view json, const Registry& registry,
                                                    LoadContext& ctx, Model& into);

// What the sink hands back for each reconstructed node (serialize.sharing
// Decision 3): the `ObjectId` to bind into a layer's `LayerRecord` (for a layer
// root) AND the live, non-owning `Content*` the read recursion wires as another
// node's input edge (`ContentRef`). Every node -- layer roots and input children
// alike -- flows through the sink, which owns it; only layer roots have their id
// bound into a `LayerRecord` (the model stores no input edges, records.hpp:60-92).
struct SunkContent {
  ObjectId id{};
  Content* live{nullptr};
};

// The seam through which the content-aware read path hands each reconstructed
// `Content` to the caller (serialize.kind_params; +live `Content*` serialize.sharing
// Decision 3): the caller takes ownership and returns the `ObjectId` to bind plus
// the live pointer the recursion threads into parents as an input edge.
// runtime.document_serialize implements this against `Document`'s content map --
// sunk nodes populate `d_contents`, the returned id is the `ContentRecord`'s, and
// `live` is the owned object's address (Decision 5). Names no JSON type, so it rides
// the public header.
using ContentSink = std::function<SunkContent(std::unique_ptr<Content>)>;

// Content-aware load (serialize.kind_params): as `load_document` above for the
// envelope, composition, and core-owned placement, plus each layer's content body.
// For every layer carrying a `kind`, the body is routed through `codecs` (known kind
// -> the codec's live `Content`; unknown kind -> a `PlaceholderContent` preserving
// the body verbatim), handed to `sink`, and the returned `ObjectId` is bound into
// the layer -- replacing the invalid `ObjectId{}` the content-free overload binds.
// `ctx` threads base-URI resolution + async asset loading into each codec; `registry`
// supplies the kind-plugin-present witness (Decision 2). A layer with no content body
// binds `ObjectId{}` as before.
//
// Routing runs entirely BEFORE the model is touched, so a malformed content body
// returns a `ReaderError` with the target `Model` left unmutated (revision 0, empty).
//
// `unknown` is the every-tier unknown-field stash (doc 08 Principle 4,
// serialize.unknown_field_preservation): when non-null, every key the core does not
// name -- at the document root, the `arbc` envelope, the composition (and its
// `working_space` / `working_audio_format` interiors), each layer (and its `time_map`
// interior), each standalone content body, and each known kind's `params` -- is
// captured verbatim into it, keyed by `(scope, ObjectId)`, and the content-aware
// `serialize_document` merges it back on save without ever shadowing a known key. It is
// REPLACED wholesale on a successful load and left untouched on any error, exactly like
// the target `Model`. `nullptr` is the historical lossy behavior.
//
// `root_composition` PRE-ALLOCATES the root composition's `ObjectId`
// (runtime.nested_external_ref Constraint 6). An external-reference loader needs that id
// in hand BEFORE the read begins, so it can seed its resolved-URI map with "this document
// -> this composition" and collapse a document that references ITSELF onto the
// in-document Droste case. The default -- an invalid id -- allocates one during the read,
// exactly as before, so the root stays the lowest-id composition either way.
expected<std::monostate, ReaderError> load_document(std::string_view json, const Registry& registry,
                                                    const CodecTable& codecs, LoadContext& ctx,
                                                    const ContentSink& sink, Model& into,
                                                    UnknownFieldStore* unknown = nullptr,
                                                    ObjectId root_composition = ObjectId{});

// Install ONE document's composition graph into an EXISTING model as a child subtree,
// under the caller-supplied root id `root_composition`, and return that id
// (runtime.nested_external_ref). The read is `load_document`'s, exactly: same envelope
// check, same codec routing, same `$ref` resolution, same cycle discipline. The three
// differences are the whole of "external":
//
//  - it installs through an ordinary transaction rather than `load_baseline`, which would
//    republish the HOST document at revision 0 and discard it;
//  - it leaves the model root alone -- `find_first_composition` names the lowest-id
//    composition, and the host's root was allocated before any child's;
//  - it drops the child's unknown-field residuals: they are the OTHER document's data,
//    which this one never re-emits (its child is named by URI, doc 08 Principle 3).
//
// `root_composition` must come from `Model::allocate_id()` and must ALREADY be recorded in
// the caller's resolved-URI map: that is the allocate-before-parse knot-cut that makes a
// cross-document cycle (A embeds B embeds A) terminate as a finite graph
// (compositions_table Decision 4, lifted across documents; doc 05, doc 08 Principle 7).
//
// Returns `ObjectId{}` when the bytes are a legal document that simply holds no root
// composition -- there is nothing to embed, and the caller reports the reference
// unavailable. Malformed bytes are a `ReaderError` value, which the caller likewise turns
// into unavailability rather than failing the parent load (doc 08 Principle 3, doc 05).
expected<ObjectId, ReaderError> load_composition(std::string_view json, const Registry& registry,
                                                 const CodecTable& codecs, LoadContext& ctx,
                                                 const ContentSink& sink, Model& into,
                                                 ObjectId root_composition);

} // namespace arbc
