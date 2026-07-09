#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/model/model.hpp>

#include <string>

namespace arbc {

// The canonical writer's error value (doc 08 Principle 5; doc 10 errors-as-values,
// 10:15-17). A malformed pinned document -- currently a non-finite placement
// scalar (NaN / +/-Inf) that cannot round-trip through JSON -- surfaces here as a
// value rather than as a thrown nlohmann exception or a silent `null`.
struct SerializeError {
  enum class Kind {
    NonFiniteValue, // a transform/opacity/gain/canvas scalar was NaN or +/-Inf
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
// operator `inputs`, and the `contents`/`$ref` sharing table are later serialize
// tasks (kind_params / sharing).
//
// Returns the byte buffer, or a `SerializeError` value when a placement scalar is
// non-finite; no nlohmann exception crosses this boundary.
expected<std::string, SerializeError> serialize_document(const DocRoot& doc);

} // namespace arbc
