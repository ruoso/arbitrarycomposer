#include <arbc/serialize/codec.hpp>

#include <arbc/serialize/placeholder_content.hpp>

#include <string>
#include <utility>

namespace arbc {

using json = nlohmann::json;

void CodecTable::add(std::string kind_id, Codec codec) {
  d_codecs[std::move(kind_id)] = std::move(codec);
}

const Codec* CodecTable::find(std::string_view kind_id) const {
  // Heterogeneous lookup would avoid the string build, but the table is populated
  // once at startup and consulted once per layer -- correctness over the micro-opt.
  const auto it = d_codecs.find(std::string(kind_id));
  return it != d_codecs.end() ? &it->second : nullptr;
}

namespace {

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

} // namespace

expected<std::unique_ptr<Content>, ReaderError>
content_body_from_json(const json& body, const CodecTable& codecs, const Registry& registry,
                       LoadContext& ctx) {
  // The content body must be an object carrying at least a string `kind` (doc 08:29).
  if (!body.is_object()) {
    return read_fail(ReaderError::Kind::MalformedField, "");
  }
  const auto kit = body.find("kind");
  if (kit == body.end()) {
    return read_fail(ReaderError::Kind::MissingRequiredField, "/kind");
  }
  if (!kit->is_string()) {
    return read_fail(ReaderError::Kind::MalformedField, "/kind");
  }
  const std::string kind = kit->get<std::string>();
  if (kind.empty()) {
    return read_fail(ReaderError::Kind::MissingRequiredField, "/kind");
  }

  // `params` is the kind-owned, opaque frame field: absent is an empty object; a
  // present-but-non-object `params` is malformed at the format frame regardless of
  // kind (the codec never sees a non-object). This validation runs BEFORE dispatch
  // so both the codec and the placeholder paths reject it identically (Constraint 5).
  const auto pit = body.find("params");
  if (pit != body.end() && !pit->is_object()) {
    return read_fail(ReaderError::Kind::MalformedField, "/params");
  }

  // Dispatch on the reverse-DNS kind id through the serialize-owned codec table
  // (Decision 2). A registered codec owns `params`; the `LoadContext` threads its
  // base-URI resolution + async asset loading into the codec (Principle 1).
  if (const Codec* codec = codecs.find(kind); codec != nullptr && codec->deserialize) {
    const json params = (pit != body.end()) ? *pit : json::object();
    return codec->deserialize(params, ctx);
  }

  // No codec: round-trip as a placeholder preserving the WHOLE body verbatim
  // (Principles 2/4). The `Registry` is consulted for the plugin-present witness
  // (Decision 2): a kind whose factory is registered but whose serialize codec is
  // absent (an old plugin, or a version-skew placeholder choice) records
  // `kind_registered() == true`; an entirely-missing plugin records false.
  const bool kind_registered = registry.factory(kind) != nullptr;
  return std::unique_ptr<Content>(std::make_unique<PlaceholderContent>(body, kind_registered));
}

expected<json, SerializeError>
content_body_to_json(std::string_view kind_id, std::string_view kind_version,
                     const Content& content, const CodecTable& codecs) {
  // A placeholder re-emits its stored body verbatim -- byte-equivalent under the
  // writer's canonical dump (Principles 2/5) -- so its `kind`/`kind_version`/`inputs`
  // and any unknown fields survive without a codec. The passed `kind_id`/version are
  // advisory here; the stored body is authoritative.
  if (const auto* placeholder = dynamic_cast<const PlaceholderContent*>(&content)) {
    return placeholder->body();
  }

  // A known kind: run its `SerializeFn` for the `params` JSON, then wrap it with the
  // core-owned `{kind, kind_version}` frame (Principle 1).
  const Codec* codec = codecs.find(kind_id);
  if (codec == nullptr || !codec->serialize) {
    return unexpected(SerializeError{SerializeError::Kind::NoCodec, ObjectId{}});
  }
  expected<json, SerializeError> params = codec->serialize(content);
  if (!params) {
    return unexpected(params.error());
  }

  json out = json::object();
  out["kind"] = std::string(kind_id);
  out["kind_version"] = std::string(kind_version);
  out["params"] = std::move(*params);
  return out;
}

} // namespace arbc
