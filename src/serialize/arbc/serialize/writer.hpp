#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/model/model.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace arbc {

class Content;    // contract (L2); named only as a reference in the provider seam.
class CodecTable; // serialize-internal (codec.hpp); it names nlohmann::json, so it
                  // is only forward-declared here and stays off the public header.

// The canonical writer's error value (doc 08 Principle 5; doc 10 errors-as-values,
// 10:15-17). A malformed pinned document -- a non-finite placement scalar (NaN /
// +/-Inf) that cannot round-trip through JSON, or a content-body codec that fails
// or is absent (serialize.kind_params) -- surfaces here as a value rather than as
// a thrown nlohmann exception or a silent `null`.
struct SerializeError {
  enum class Kind {
    NonFiniteValue, // a transform/opacity/gain/canvas scalar was NaN or +/-Inf
    NoCodec,        // a non-placeholder content had no registered content-body codec
    CodecFailed,    // a registered codec could not serialize the content's params
  };
  Kind kind{Kind::NonFiniteValue};
  ObjectId object{}; // the offending object (layer / composition), when known

  friend bool operator==(const SerializeError&, const SerializeError&) = default;
};

// Serialize one pinned document version to its byte-canonical `.arbc` JSON
// (doc 08). Reads `doc` only through its lock-free `const` peek accessors, so a
// caller may pin `Model::current()`, hold the `DocStatePtr`, and serialize it on a
// background thread while editing proceeds -- doc 14:35-39,230-231's "autosave
// never pauses editing." Output is deterministic (Principle 5, as amended): object
// keys in ascending UTF-8 byte order, numbers in the JSON library's shortest
// round-trip form (integers for flick/canvas extents, reals for placement
// scalars), and omit-when-default placement, so a given pinned version serializes
// byte-identically across runs and re-serializations.
//
// Field scope: the document envelope (`{"arbc":{"format":1}}`), the `composition`
// object (`canvas`, `working_space`/`working_audio_format` omitted when default),
// and each layer bottom-to-top with its core-owned spatial + temporal + audio
// placement (`transform`, `opacity`, `visible`; the omit-when-default twins `gain`,
// `audible`, `span`, `time_map`). The content body (`kind`/`kind_version`/`params`),
// operator `inputs`, and the `contents`/`$ref` sharing table ride only the
// content-aware overload below (this no-provider form stays content-free,
// Constraint 6).
//
// Returns the byte buffer, or a `SerializeError` value when a placement scalar is
// non-finite; no nlohmann exception crosses this boundary.
expected<std::string, SerializeError> serialize_document(const DocRoot& doc);

// One layer's content body, resolved by the provider for the writer to emit
// (serialize.kind_params). `kind`/`kind_version` are the reverse-DNS id + producer
// version the core emits; `content` is the live content run through the codec for
// its `params`. A `PlaceholderContent` re-emits its own stored body verbatim, so
// its `kind`/`kind_version` here are advisory (the stored body wins). Names no JSON
// type, so it rides the public header.
struct ContentBody {
  std::string_view kind;
  std::string_view kind_version;
  const Content& content;
};

// The seam through which the content-aware writer resolves a LAYER's bound content
// (by its `ContentRecord` ObjectId) to a `ContentBody`, or `nullopt` for a layer
// with no content to emit. runtime.document_serialize supplies this from a pinned
// snapshot (Decision 5); the writer stays agnostic to the runtime side-map.
using ContentBodyProvider = std::function<std::optional<ContentBody>(ObjectId content_id)>;

// A content's core-owned `(kind, kind_version)` metadata (serialize.sharing
// Decision 3). Input children are reached via `Content::inputs()` as bare
// `Content*`s -- the model gives them no `ObjectId` (records.hpp:60-92) -- so the
// write recursion resolves each child's kind through this `const Content&`-keyed
// lookup rather than the ObjectId-keyed `ContentBodyProvider`. The two string_views
// must outlive the serialize call (L5 backs them with `Document`'s stable strings).
struct ContentMeta {
  std::string_view kind;
  std::string_view kind_version;
};

// The `const Content&`-keyed metadata lookup (serialize.sharing Decision 3): resolve
// any live content reached via the operator graph to its `(kind, kind_version)`, or
// `nullopt` when the runtime side-map does not know it (a placeholder still
// serializes from its stored body; a codec-backed content with no metadata is a
// `NoCodec` value). runtime.document_serialize supplies this from `Document`'s
// reverse map (Decision 5). Names no JSON type, so it rides the public header.
using ContentMetaProvider = std::function<std::optional<ContentMeta>(const Content& content)>;

// Content-aware serialization (serialize.kind_params + serialize.sharing): like
// `serialize_document(doc)` for the envelope, composition, and core-owned placement,
// but every layer whose bound content the `provider` resolves also emits the
// operator graph rooted at that content -- each node a `{kind, kind_version, params,
// inputs?}` body via `codecs` (an unknown-kind `PlaceholderContent` re-emits its
// stored body verbatim), the `inputs` array walked from `Content::inputs()` and
// omitted when empty (doc 08 Principle 6). Content reachable two or more times across
// the whole document is hoisted ONCE into a document-level `contents` table under a
// deterministic first-encounter ordinal id and referenced by `{"$ref": id}` at every
// use site (Constraint 2); `meta` resolves each input child's `(kind, kind_version)`.
// The no-provider overload above stays byte-identical to today's goldens
// (Constraint 6).
//
// Returns the byte buffer, or a `SerializeError` (a non-finite scalar, or a codec
// that failed / had no registered codec / no metadata for a graph node); no nlohmann
// exception crosses the API.
expected<std::string, SerializeError> serialize_document(const DocRoot& doc,
                                                         const ContentBodyProvider& provider,
                                                         const ContentMetaProvider& meta,
                                                         const CodecTable& codecs);

} // namespace arbc
