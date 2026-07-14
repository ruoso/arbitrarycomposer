// The org.arbc.image built-in codec (kinds.image Decision 2). A runtime TU, even though the
// KIND itself ships out-of-lib in `arbc-plugin-image`.
//
// THE CODEC LINE IS A DECODER LINE (doc 17 "The codec line", doc 00 decision record). Doc
// 17:246 supplies the test -- "the distinction is WHAT IS BEING PARSED, not whether bytes
// get smaller" -- and it resolves cleanly here: this codec parses our own JSON and a URI
// string, and never an encoded image byte. So it does not cross the line, and it lives in
// `runtime` beside the built-in codecs. The DECODER -- the thing that parses a third-party
// format over untrusted input -- stays in the plugin.
//
// It is also FORCED. A plugin cannot register a codec today: the whole plugin ABI is
// `arbc_plugin_register(Registry&)` (`plugin.hpp:20`), and a `Registry` traffics in content
// factories, not codecs. Putting codec registration across that boundary would put
// `nlohmann::json` in every plugin's link surface -- exactly the dependency
// `serialize.kind_params` removed from `contract`. `runtime.plugin_operator_registration`
// may one day supply a json-free codec ABI; when it does, this TU migrates, and nothing
// here forecloses that.
//
// Consequently this TU NAMES NO PLUGIN TYPE. It reaches the kind solely through
// `Registry::factory("org.arbc.image")` -- which is also what makes the gating fall out for
// free: no plugin, no factory, no codec registered, and the layer round-trips verbatim as a
// `PlaceholderContent` (the existing `CodecTable::find`-miss path, doc 08 Principles 2/4).
// A user without the image plugin opens the document, sees a placeholder, saves, and loses
// nothing. Zero new machinery.
//
// THE SPLIT OF LABOUR (Decision 5). The CORE owns URI resolution and the byte fetch -- one
// `LoadContext` seam serving assets and external compositions alike (doc 08:33-35) -- and the
// PLUGIN owns bytes -> pixels. This codec is the first production caller of
// `LoadContext::load_asset`, the hook `serialize.reader` shipped as an interface and left
// deliberately unused ("a filled-in loader lands with the kinds that first need it").
//
// UNAVAILABLE IS NEVER A READ ERROR (doc 08:126-134, doc 05:77-83). A missing, unreadable,
// or undecodable asset -- and a `LoadContext` with no `AssetSource` installed at all --
// yields a content with no pixels; the authored URI is preserved verbatim, the layer
// re-saves byte-identically, and the parent document LOADS SUCCESSFULLY. A missing external
// file is a condition of the environment that may resolve later, not data loss.

#include <arbc/runtime/builtin_codecs.hpp>

#include <nlohmann/json.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// The document body carries the authored URI AND NOTHING ELSE (Constraint 4, doc 08
// Principle 3: "an imported image has a file it came from, so it serializes as a URI and
// nothing more"). No pixels, no tiles, no intrinsic size, no decoded cache -- which is the
// whole point of the kind, and the reason a 30-layer 24 MP composition is ~32 MB rather
// than ~490 MB.
//
// The URI is the one the document AUTHORED, read back off `Content::external_asset_ref()`
// (Decision 3) -- never absolutised and never rewritten to the resolved URI, so a project
// directory stays relocatable and `save(load(bytes)) == bytes` (Constraint 5). Whether the
// asset actually loaded is invisible here: a document saved with the photograph missing is
// byte-identical to the same document saved with it present.
//
// Deliberately type-blind. Every sibling codec `dynamic_cast`s to its concrete kind; this
// one cannot, because naming `ImageContent` would link the plugin -- and with it the decoder
// -- into `libarbc`, which is the exact leak the codec line forbids. It does not need to:
// `content_body_to_json` routes by kind id, so the content it is handed IS an image, and the
// accessor it reads is a contract-level virtual with a null default. An empty ref emits an
// empty `params` object, which is also the right answer for the lenient mistyped-`source`
// case below (the residual diff restores the authored key verbatim).
expected<json, SerializeError> serialize_image(const Content& content) {
  json params = json::object();
  const std::string_view source = content.external_asset_ref();
  if (!source.empty()) {
    params["source"] = std::string(source);
  }
  return params;
}

// Read `params.source`, resolve it, fetch its bytes, and hand both to the plugin's factory.
//
// A present-but-non-string `source` is treated LENIENTLY as absent (the `codec_nested.cpp`
// idiom): it is then a param the codec did not consume, so it rides the core's residual diff
// and round-trips verbatim, and no hostile input can turn a mistyped key into a load failure.
//
// V1 IS SYNCHRONOUS, and correct in production because of it: `FilesystemAssetSource` fires
// `on_ready` INLINE, before `request` returns (`filesystem_asset_source.hpp:27`), so the
// bytes are in hand here and no pending state arises. A DEFERRING source (a future network /
// content-store source) simply has not fired: the buffer is empty, and v1 reads that as
// UNAVAILABLE -- honest and lossless (the ref is preserved, the layer re-saves
// byte-identically). The true pending state -- mint now, install pixels later on the writer
// thread with a revision bump and a damage route -- is `kinds.image_async_pending`; it needs
// a pixels-arrive-later install channel that `Content` does not have today.
//
// The sink is a `shared_ptr` rather than a stack local precisely because a deferring source
// may retain the continuation and fire it after this frame is gone: the buffer then outlives
// us and the late write is harmless, instead of scribbling on a dead stack.
expected<std::unique_ptr<Content>, ReaderError> deserialize_image(const json& params,
                                                                  LoadContext& ctx,
                                                                  const Registry& registry) {
  const ContentFactory* const factory = registry.factory(k_image_kind_id);
  if (factory == nullptr) {
    // Unreachable through the assembled table (the codec is registered only when the
    // factory exists), but `image_codec` is a public factory function and a caller may hand
    // it a registry without the plugin. An honest value beats undefined behaviour.
    return read_fail(ReaderError::Kind::MalformedField, "/kind");
  }

  std::string source;
  if (const auto it = params.find("source"); it != params.end() && it->is_string()) {
    source = it->get<std::string>();
  }

  // Resolve through the ONE seam (doc 08:33-35), which is also what buys resolved-identity
  // dedup: `bg.png` and `./bg.png` are two authored refs and one `ResolvedRef` -- and, on the
  // plugin side, one decode. An empty `source` resolves to nothing and fetches nothing; the
  // content is minted unavailable, keeping whatever the residual diff preserved.
  std::string resolved;
  auto bytes = std::make_shared<std::string>();
  if (!source.empty()) {
    const ResolvedRef ref = ctx.resolve(source);
    resolved = ctx.resolved_uri(ref);
    ctx.load_asset(ref, [bytes](std::string_view fetched) { bytes->assign(fetched); });
  }

  // The `ContentConfig` frame (Decision 5): "<authored>\n<resolved>\n<encoded-bytes>", split
  // at the first two newlines. `ContentConfig` is explicitly an opaque, kind-defined
  // `string_view` (`registry.hpp:35`) and the CI dual-build plugins already carry ad-hoc
  // framings over it, so this needs NO ABI change. A URI cannot contain a raw newline and
  // `normalize_uri` is purely lexical, so the framing is unambiguous. Both URIs ride it
  // because they answer different questions: the AUTHORED one is what the kind reads back for
  // `params.source`, the RESOLVED one is the identity its pyramid cache dedups the decode on.
  std::string config;
  config.reserve(source.size() + resolved.size() + bytes->size() + 2);
  config.append(source);
  config.push_back('\n');
  config.append(resolved);
  config.push_back('\n');
  config.append(*bytes);

  expected<std::unique_ptr<Content>, std::string> built = (*factory)(config);
  if (!built) {
    // The kind refused to construct AT ALL -- distinct from an unavailable asset, which the
    // kind accepts and reports as empty bounds. Only a malformed frame (a core bug) or a
    // hostile third-party factory reaches here; report it as a value, never a throw.
    return read_fail(ReaderError::Kind::MalformedField, "/params/source");
  }
  return std::move(*built);
}

} // namespace

Codec image_codec(const Registry& registry) {
  return Codec{serialize_image,
               [&registry](const json& params, std::span<const ContentRef> /*inputs*/,
                           ObjectId /*composition*/,
                           LoadContext& ctx) -> expected<std::unique_ptr<Content>, ReaderError> {
                 // Neither `inputs` nor `composition` is consumed: `org.arbc.image` is a
                 // leaf kind with an empty `inputs()` and no child composition (Constraint 3).
                 return deserialize_image(params, ctx, registry);
               }};
}

} // namespace arbc
