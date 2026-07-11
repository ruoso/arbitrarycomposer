#include <arbc/serialize/codec.hpp>
#include <arbc/serialize/placeholder_content.hpp>
#include <arbc/serialize/unknown_json.hpp> // the params residual (doc 08 Principle 4)

#include <span>
#include <string>
#include <utility>
#include <vector>

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
content_body_from_json(const json& body, std::span<const ContentRef> inputs, ObjectId composition,
                       const CodecTable& codecs, const Registry& registry, LoadContext& ctx,
                       json* params_residual) {
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
  // (Decision 2). A registered codec owns `params` and adopts the already-built input
  // edges at construction (Decision 4); the `LoadContext` threads its base-URI
  // resolution + async asset loading into the codec (Principle 1).
  if (const Codec* codec = codecs.find(kind); codec != nullptr && codec->deserialize) {
    const json params = (pit != body.end()) ? *pit : json::object();
    expected<std::unique_ptr<Content>, ReaderError> produced =
        codec->deserialize(params, inputs, composition, ctx);

    // Decision 4: only the codec can say which `params` keys it consumed, and its own
    // serializer already reveals that -- every built-in serializer emits every key its
    // deserializer reads (`fade`'s optional `in`/`out` round-trip absence as absence).
    // So run it back over the fresh content and keep `params_in - params_out`. Doing it
    // HERE, at load, is what makes the rule safe: a save-time diff against a stashed raw
    // `params` would see a param the user later CLEARED as "dropped by the codec" and
    // resurrect it. Errors stay values: a codec that cannot re-serialize simply
    // preserves nothing.
    if (produced && params_residual != nullptr && codec->serialize) {
      if (const expected<json, SerializeError> back = codec->serialize(**produced); back) {
        *params_residual = unknown_residual_diff(params, *back);
      }
    }
    return produced;
  }

  // No codec: round-trip as a placeholder preserving `kind`/`kind_version`/`params`
  // and any unknown fields verbatim (Principles 2/4), and binding the reconstructed
  // input edges as its live pass-through inputs (Principle 6, Decision 4). The
  // `inputs` array is stripped from the STORED body: it is graph-structural, not
  // opaque -- the write recursion re-derives it from `inputs()` with fresh, canonical
  // `$ref` ids (Decision 2/7), so re-emitting a stale array here would double it and
  // pin dead ids. `composition` is stripped for exactly the same reason and gets
  // exactly the same treatment (compositions_table Decision 6): the RESOLVED child id
  // rides the placeholder's `composition_ref()`, so the core still sees the edge --
  // keeping the child reachable from the writer's walk, which then re-derives its
  // ordinal. Leaving the load-time string in the body would instead pin a stale
  // ordinal that renumbering can silently repoint at a different composition. The
  // `Registry` is consulted for the plugin-present witness (Decision 2): a kind whose
  // factory is registered but whose serialize codec is absent (an old plugin, or a
  // version-skew placeholder choice) records `kind_registered() == true`; an
  // entirely-missing plugin records false.
  json stored = body;
  stored.erase("inputs");
  stored.erase("composition");
  const bool kind_registered = registry.factory(kind) != nullptr;
  return std::unique_ptr<Content>(std::make_unique<PlaceholderContent>(
      std::move(stored), kind_registered, std::vector<ContentRef>(inputs.begin(), inputs.end()),
      composition));
}

expected<json, SerializeError> content_body_to_json(std::string_view kind_id,
                                                    std::string_view kind_version,
                                                    const Content& content,
                                                    const CodecTable& codecs) {
  // A placeholder re-emits its stored (inputs-free) LEAF body verbatim --
  // byte-equivalent under the writer's canonical dump (Principles 2/5) -- so its
  // `kind`/`kind_version`/`params` and any unknown fields survive without a codec.
  // Its `inputs` limb is re-derived by the write recursion from `inputs()`, never
  // from this body (Decision 2). The passed `kind_id`/version are advisory here; the
  // stored body is authoritative.
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
