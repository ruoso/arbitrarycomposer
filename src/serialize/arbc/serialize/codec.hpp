#pragma once

// INTERNAL serialize-component header (deliberately NOT in the component's
// PUBLIC_HEADERS FILE_SET): it names `nlohmann::json`, which is linked PRIVATE so
// it never rides libarbc's public interface (serialize.writer Decision 3). Only
// serialize-component translation units (codec.cpp, reader.cpp, writer.cpp) and
// codec-registering call sites (runtime / plugins / this component's tests, which
// name both a concrete kind type and the JSON library) include it.
//
// The content-body codec seam (doc 08 Principle 1, as amended by
// serialize.kind_params): the core owns a layer's placement, a kind owns its
// `params`. Because the `Content` interface lives in `contract` (L2) and must not
// name the JSON library, the two hooks are NOT JSON-typed methods on `Content`;
// they are serialize-owned codecs keyed by reverse-DNS kind id (Decision 1/2),
// registered from a layer that can see both the concrete kind and the JSON library
// (`runtime` L5 for built-ins, a plugin's own TU out-of-tree; never L4).

#include <arbc/base/expected.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/serialize/deserialize.hpp> // DeserializeFn (the read hook, serialize.reader)
#include <arbc/serialize/load_context.hpp>
#include <arbc/serialize/reader.hpp> // ReaderError
#include <arbc/serialize/writer.hpp> // SerializeError

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace arbc {

// The write hook, symmetric to `DeserializeFn` (doc 08 Principle 1 delta,
// Decision 1): a live `Content` -> its `params` JSON, or a `SerializeError` value.
// The core wraps the returned `params` with the core-owned `kind`/`kind_version`.
using SerializeFn = std::function<expected<nlohmann::json, SerializeError>(const Content&)>;

// A per-kind codec pair. A kind registers both halves so its content round-trips;
// a kind with no registered codec round-trips as a `PlaceholderContent`
// (Principle 2).
struct Codec {
  SerializeFn serialize;
  DeserializeFn deserialize;
};

// The serialize-owned codec table keyed by reverse-DNS kind id (Decision 2): the
// dispatcher the read/write routing consults, distinct from `contract::Registry`
// (L2, which cannot hold `std::function`s naming `nlohmann::json`). Concrete
// codecs register from `runtime` / plugins (Decision 3); this component ships the
// table + the routing, and its tests register test codecs to prove the seam.
class CodecTable {
public:
  // Register `codec` under `kind_id`; a duplicate id overwrites the prior codec
  // (last registration wins -- a plugin may supersede a built-in). No throw.
  void add(std::string kind_id, Codec codec);

  // The codec for `kind_id`, or nullptr when none is registered (the placeholder
  // fall-through).
  const Codec* find(std::string_view kind_id) const;

  bool empty() const noexcept { return d_codecs.empty(); }
  std::size_t size() const noexcept { return d_codecs.size(); }

private:
  std::unordered_map<std::string, Codec> d_codecs;
};

// Read routing (serialize.kind_params, +inputs serialize.sharing): turn ONE node's
// content body `{kind, kind_version, params, inputs?, ...}` into a live `Content`,
// given its already-reconstructed input edges. The document-level read recursion
// (reader.cpp) parses `body["inputs"]`/`$ref` first, builds each child, and passes
// the resulting live `Content*`s here as `inputs`; this function only routes the
// `{kind, params}` node itself. A kind with a registered codec runs
// `codec.deserialize(params, inputs, ctx)` (the codec owns `params` and adopts
// `inputs` at construction, Decision 4); an unknown kind becomes a
// `PlaceholderContent` preserving `kind`/`kind_version`/`params` and any unknown
// fields verbatim (doc 08 Principles 2/4) while binding `inputs` as its live
// pass-through edges (Principle 6) -- the `inputs` array itself is NOT stored in the
// opaque body (it is graph-structural, re-derived on save from `inputs()`, Decision
// 2/7). `registry` witnesses whether the kind's plugin is present at all, which the
// placeholder records. Errors are values (Constraint 5): a missing/empty `kind` is
// `MissingRequiredField`, a mistyped `kind`/`params` is `MalformedField`, and a
// codec's own parse failure propagates; no nlohmann exception escapes.
expected<std::unique_ptr<Content>, ReaderError>
content_body_from_json(const nlohmann::json& body, std::span<const ContentRef> inputs,
                       const CodecTable& codecs, const Registry& registry, LoadContext& ctx);

// Write routing (serialize.kind_params): emit ONE node's LEAF content body -- the
// `{kind, kind_version, params}` frame only, NEVER the `inputs` limb (the
// document-level write recursion in writer.cpp appends `inputs`/`$ref` from a live
// `Content::inputs()` walk, Decision 1/2). A `PlaceholderContent` re-emits its stored
// (inputs-free) body verbatim (byte-equivalent under the writer's canonical dump --
// Principles 2/5), ignoring `kind_id`/`kind_version`. Any other content runs the
// registered `SerializeFn` for `kind_id`, wrapping the returned `params` as
// `{kind, kind_version, params}`. A non-placeholder content with no registered codec
// is `SerializeError::Kind::NoCodec`; a codec failure propagates.
expected<nlohmann::json, SerializeError> content_body_to_json(std::string_view kind_id,
                                                              std::string_view kind_version,
                                                              const Content& content,
                                                              const CodecTable& codecs);

} // namespace arbc
