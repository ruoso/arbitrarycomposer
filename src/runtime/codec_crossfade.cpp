// The org.arbc.crossfade built-in operator codec (runtime.operator_codecs). A runtime
// TU that alone sees both the concrete kind type (L4 kind_crossfade) and
// nlohmann::json (via L4 serialize, PRIVATE link) -- the sanctioned home for a
// built-in codec (doc 08 Principle 1, doc 17:60, Decision 2). Encodes/decodes only the
// kind's `params`; the core routing owns the `{kind, kind_version}` frame and the
// `inputs` limb (re-derived on save from `Content::inputs()`, never stored here --
// Constraint 1). No nlohmann exception escapes; malformed input is a `ReaderError`
// value.

#include <arbc/base/time.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// The stable string literal for the (v1: only) crossfade shape (Constraint 3). A new
// shape is an additive string; an unknown token fails closed on read.
const char* shape_token(CrossfadeShape shape) {
  switch (shape) {
  case CrossfadeShape::Linear:
    return "linear";
  }
  return "linear";
}

// Serialize a `CrossfadeContent`'s immutable `CrossfadeParams` as
// `{"duration": <int flicks>, "shape": "linear", "start": <int flicks>}` -- flick
// instants are integer core scalars (doc 08 Principle 5); canonical key sort orders
// the keys. A non-crossfade content is a codec/kind-routing mismatch -> CodecFailed.
expected<json, SerializeError> serialize_crossfade(const Content& content) {
  const auto* cf = dynamic_cast<const CrossfadeContent*>(&content);
  if (cf == nullptr) {
    return unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
  }
  const CrossfadeParams& p = cf->params();
  json params = json::object();
  params["shape"] = shape_token(p.shape);
  params["start"] = static_cast<std::int64_t>(p.start.flicks);
  params["duration"] = static_cast<std::int64_t>(p.duration.flicks);
  return params;
}

// Reconstruct a `CrossfadeContent` from a `params` object and the two already-built
// input edges (from = slot 0, to = slot 1), adopted at construction (Constraint 2).
// `params` is guaranteed an object by the core routing. A missing/unknown `shape`, a
// non-integer `start`/`duration`, or an input arity other than 2 is a `ReaderError`
// value -- no exception, no partial construction. The `params` are validated BEFORE
// the input-arity check so a malformed 0-input operator body faults before any child
// is bound, leaving the model unmutated (Constraint 2).
expected<std::unique_ptr<Content>, ReaderError>
deserialize_crossfade(const json& params, std::span<const ContentRef> inputs,
                      LoadContext& /*ctx*/) {
  const auto shit = params.find("shape");
  if (shit == params.end() || !shit->is_string() || shit->get<std::string>() != "linear") {
    return read_fail(ReaderError::Kind::MalformedField, "/params/shape");
  }
  const auto sit = params.find("start");
  if (sit == params.end() || !sit->is_number_integer()) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/start");
  }
  const auto dit = params.find("duration");
  if (dit == params.end() || !dit->is_number_integer()) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/duration");
  }
  if (inputs.size() != 2) {
    return read_fail(ReaderError::Kind::MalformedField, "/inputs");
  }
  CrossfadeParams p;
  p.shape = CrossfadeShape::Linear;
  p.start = Time{sit->get<std::int64_t>()};
  p.duration = Time{dit->get<std::int64_t>()};
  return std::unique_ptr<Content>(
      std::make_unique<CrossfadeContent>(inputs[0], inputs[1], p));
}

} // namespace

Codec crossfade_codec() { return Codec{serialize_crossfade, deserialize_crossfade}; }

} // namespace arbc
