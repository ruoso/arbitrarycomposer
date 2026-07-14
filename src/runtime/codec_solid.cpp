// The org.arbc.solid built-in codec (runtime.document_serialize). A runtime TU that
// alone sees both the concrete kind type (L4 kind_solid) and nlohmann::json (via L4
// serialize, PRIVATE link) -- the sanctioned home for a built-in codec (doc 08
// Principle 1, doc 17:60, Decision 3). Encodes/decodes only the kind's `params`; the
// core routing (`content_body_to_json`/`content_body_from_json`) owns the
// `{kind, kind_version}` frame. No nlohmann exception escapes (Constraint 1).

#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>

#include <nlohmann/json.hpp>

#include <memory>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// Serialize a `SolidContent`'s premultiplied `Rgba` as `{"color": [r, g, b, a]}`.
// A non-solid content is a codec/kind-routing mismatch -> CodecFailed as a value.
expected<json, SerializeError> serialize_solid(const Content& content, SaveContext& /*ctx*/) {
  const auto* solid = dynamic_cast<const SolidContent*>(&content);
  if (solid == nullptr) {
    return unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
  }
  const Rgba c = solid->color();
  json params = json::object();
  params["color"] = json::array({c.r, c.g, c.b, c.a});
  return params;
}

// Reconstruct a `SolidContent` from a `{"color": [r, g, b, a]}` params object.
// `params` is guaranteed an object by the core routing; a missing/mistyped `color`
// (not a 4-element numeric array) is a MalformedField value -- no exception, no
// partial construction. The leaf kind ignores `inputs`/`ctx`.
expected<std::unique_ptr<Content>, ReaderError>
deserialize_solid(const json& params, std::span<const ContentRef> /*inputs*/,
                  ObjectId /*composition*/, LoadContext& /*ctx*/) {
  const auto cit = params.find("color");
  if (cit == params.end() || !cit->is_array() || cit->size() != 4) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/color");
  }
  const json& arr = *cit;
  for (const json& e : arr) {
    if (!e.is_number()) {
      return read_fail(ReaderError::Kind::MalformedField, "/params/color");
    }
  }
  const Rgba color{
      static_cast<float>(arr[0].get<double>()), static_cast<float>(arr[1].get<double>()),
      static_cast<float>(arr[2].get<double>()), static_cast<float>(arr[3].get<double>())};
  return std::unique_ptr<Content>(std::make_unique<SolidContent>(color));
}

} // namespace

Codec solid_codec() { return Codec{serialize_solid, deserialize_solid}; }

} // namespace arbc
