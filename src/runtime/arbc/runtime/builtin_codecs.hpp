#pragma once

// INTERNAL runtime-component header (NOT in PUBLIC_HEADERS): it names
// `nlohmann::json` (through `Codec`), kept off the runtime public interface and
// linked PRIVATE, exactly as `arbc/serialize/codec.hpp` is for `arbc_serialize`
// (kind_params Decision 3, runtime.document_serialize Decision 3).
//
// The built-in codecs live in their own TUs (`codec_solid.cpp`, `codec_tone.cpp`,
// the operator codecs `codec_fade.cpp`/`codec_crossfade.cpp`, and the nesting codec
// `codec_nested.cpp`) -- the only layer that legally sees both a concrete kind type
// (L4 `kind_solid`/`kind_tone`/`kind_fade`/`kind_crossfade`/`kind_nested`) and the JSON
// library (via L4 `serialize`) is `runtime` (L5). Each factory returns a
// `Codec{serialize, deserialize}` pair over
// its kind's `params`; the façade (`document_serialize.cpp`) assembles them into a
// `CodecTable` for the save path and wraps their `deserialize` with per-load kind
// recording for the read path (Decision 4). The operator codecs additionally adopt
// their already-built input edges (`std::span<const ContentRef> inputs`) at
// construction (runtime.operator_codecs Decision 2).

#include <arbc/serialize/codec.hpp>

namespace arbc {

class ExternalCompositionLoader; // runtime/external_composition_loader.hpp
class ExternalAssetLoader;       // runtime/external_asset_loader.hpp
class RasterTileStore;           // runtime/raster_tile_store.hpp
class TileEncodeDispatch;        // runtime/tile_encode_dispatch.hpp
class TileDecodeDispatch;        // runtime/tile_decode_dispatch.hpp

// Per-built-in producer `kind_version` (Constraint 3): a fixed constant chosen and
// pinned by this task, golden-pinned as the literal emitted beside `kind`. Advisory
// metadata -- the built-in kinds declare no version of their own today.
inline constexpr const char* k_solid_kind_version = "1";
inline constexpr const char* k_tone_kind_version = "1";
inline constexpr const char* k_fade_kind_version = "1";
inline constexpr const char* k_crossfade_kind_version = "1";
inline constexpr const char* k_nested_kind_version = "1";
inline constexpr const char* k_image_kind_version = "1";
inline constexpr const char* k_raster_kind_version = "1";

// `org.arbc.image` ships OUT-OF-LIB (`arbc-plugin-image`), so `runtime` cannot name its
// content type -- doing so would link the decoder into `libarbc`, which is the one thing
// doc 17's codec line forbids. Its reverse-DNS id is the persistent contract the codec
// routes on, so the id (and only the id) is spelled here.
inline constexpr const char* k_image_kind_id = "org.arbc.image";

// org.arbc.solid: encodes/decodes `SolidContent`'s premultiplied `Rgba` as a
// `{"color": [r, g, b, a]}` params object.
Codec solid_codec();

// org.arbc.tone: encodes/decodes `ToneContent`'s `{frequency_hz, amplitude}` as a
// `{"amplitude": <real>, "frequency_hz": <int>}` params object.
Codec tone_codec();

// org.arbc.fade: a single-input operator codec. Serializes `FadeContent`'s immutable
// `FadeParams` as `{"shape": "linear", "in"?: {start,end}, "out"?: {start,end}}` (the
// optional windows omitted when absent); deserializes it back and adopts the one
// already-built input edge -- `FadeContent(inputs[0], params)`. Wrong input arity is
// a `ReaderError` value (runtime.operator_codecs Constraint 2).
Codec fade_codec();

// Register the runtime binder for `org.arbc.fade` (a typed attach/detach thunk over
// the concrete `FadeContent`, `operators.fade_runtime_binding`). Defined in the same
// TU as `fade_codec()` -- the only runtime TU that legally names `FadeContent`
// (doc 17:60). Reached once via `register_builtin_operator_binders()`
// (`operator_binding.hpp`); a sibling operator kind adds its own `register_*_binder`.
void register_fade_binder();

// org.arbc.crossfade: a two-input operator codec. Serializes `CrossfadeContent`'s
// immutable `CrossfadeParams` as `{"shape": "linear", "start": <int>, "duration":
// <int>}`; deserializes it back and adopts the two already-built input edges (from =
// slot 0, to = slot 1) -- `CrossfadeContent(inputs[0], inputs[1], params)`. An input
// arity other than 2 is a `ReaderError` value.
Codec crossfade_codec();

// Register the runtime binder for `org.arbc.crossfade` (a typed attach/detach thunk
// over the concrete two-input `CrossfadeContent`,
// `operators.crossfade_runtime_binding`). Defined in the same TU as
// `crossfade_codec()` -- the only runtime TU that legally names `CrossfadeContent`
// (doc 17:60). Reached once via `register_builtin_operator_binders()`
// (`operator_binding.hpp`), beside `register_fade_binder()`.
void register_crossfade_binder();

// org.arbc.nested: the nesting codec. A nested content's child arrives one of two ways
// (doc 08 Principle 7 vs Principle 3):
//
//  - IN-DOCUMENT: core-owned. The writer re-derives the `"composition"` id from graph
//    structure after `serialize` returns, and the reader pre-allocates the child id and
//    hands it to `deserialize` as its `composition` argument -- so serialize emits an
//    EMPTY `params` object and deserialize is `NestedContent(composition)`. An absent
//    child is a `MissingRequiredField` value at `/composition`.
//  - EXTERNAL: kind-owned. Serialize emits `{"ref": "<authored URI>"}` verbatim;
//    deserialize consumes it, resolves it through the `LoadContext`, and drives `loader`
//    to install the referenced `.arbc`'s composition graph into this document's model
//    (runtime.nested_external_ref). A body carrying BOTH a `composition` and a
//    `params.ref` names one child two contradictory ways: `MalformedField` at
//    `/params/ref`.
//
// Input edges are NOT consumed either way: nested's `inputs()` are a projection of the
// child composition, never authored data.
//
// `loader` is bound by CLOSURE rather than widening `DeserializeFn` -- a structural seam
// one kind needs does not belong in the signature every kind implements (Constraint 3).
// The no-argument overload binds no loader, which is what the SAVE path wants (it never
// deserializes) and what makes every external reference degrade to the doc-05 placeholder
// -- the same benign outcome as an absent `AssetSource`.
Codec nested_codec();
Codec nested_codec(ExternalCompositionLoader* loader);

// Register the runtime binder for `org.arbc.nested` (a typed attach/detach thunk over
// the concrete `NestedContent`, `kinds.nested_runtime_binding`). Defined in the same TU as
// `nested_codec()` -- the only runtime TU that names `NestedContent` (doc 17:60) -- exactly
// as fade's and crossfade's binders ride theirs; it lived in a standalone `binder_nested.cpp`
// only until this codec TU existed (that task's Decision 5). Reached once via
// `register_builtin_operator_binders()` (`operator_binding.hpp`), beside the two operator
// binders. Nested is the kind that consumes the `BindContext`'s resolver and pinned
// `DocRoot` as well as the services.
void register_nested_binder();

// org.arbc.image: the codec for an OUT-OF-LIB kind, and the worked example of doc 17's
// "the codec line is a DECODER line" (kinds.image Decision 2). It parses only our own JSON
// and a URI string -- never an encoded image byte -- so it does not cross the line and it
// lives here; the decoder that parses a third-party format over untrusted input stays in
// `arbc-plugin-image`. It is also forced: the plugin ABI is `arbc_plugin_register(Registry&)`
// and a `Registry` traffics in content factories, not codecs, so a plugin CANNOT register a
// codec without putting `nlohmann::json` in every plugin's link surface.
//
// Serialize emits `{"source": "<authored URI>"}` verbatim as the document authored it (read
// back off `Content::external_asset_ref()`) -- never absolutised, so the project directory
// stays relocatable and the bytes stay stable. Deserialize resolves that URI through the
// `LoadContext`, fetches the encoded bytes through the `AssetSource` hook -- the FIRST
// production caller of `LoadContext::load_asset` -- and hands the authored URI, the resolved
// URI, and the bytes to the plugin's `ContentFactory` through the opaque `ContentConfig`.
// The kind performs no file I/O of its own (doc 08 Principle 3 as amended).
//
// `registry` and `loader` are bound by CLOSURE, mirroring
// `nested_codec(ExternalCompositionLoader*)`: a structural seam that one kind needs does not
// belong in the signature every kind implements. The registry is where the codec finds the
// factory it cannot name; the `ExternalAssetLoader` is where the fetch acquires its THREE
// outcomes instead of two (kinds.image_async_pending Decision 2).
//
// A NULL loader means "no deferral machinery": the codec falls back to a direct
// `ctx.load_asset`, so a source that has not answered inside `request()` reads as
// unavailable, exactly as it did before that task. That keeps `image_codec(registry)`
// working standalone -- the save-path table (`builtin_codecs(const Registry&)`) never
// deserializes, and the codec stays independently testable -- and it is the same benign
// degradation `nested_codec()`'s no-argument overload already provides.
//
// Registration is GATED on the plugin being loaded (see `builtin_codecs(const Registry&)`):
// no factory for the kind, no codec, and the layer round-trips verbatim as a
// `PlaceholderContent` -- a user without the plugin opens the document, saves, and loses
// nothing. That is the existing `CodecTable::find`-miss path; it needs no new machinery.
Codec image_codec(const Registry& registry);
Codec image_codec(const Registry& registry, ExternalAssetLoader* loader);

// org.arbc.raster: the content-addressed tile store (serialize.raster_tile_store). Unlike
// every sibling above, this codec has BYTES to persist -- a painted layer's pixels are
// document state with no source file (doc 08 Principle 8) -- so it is the first consumer
// of the write-side `SaveContext`/`AssetSink` seam. It persists THE TILE TABLE, NOT THE
// IMAGE: level-0 tiles only, each hashed and written once under its content hash, mips
// rebuilt on load.
//
// `tiles` is the incremental-save hash memo, bound by CLOSURE -- the same pattern as
// `nested_codec(loader)` / `image_codec(registry, loader)`, and for the same reason: a
// structural seam one kind needs does not belong in the signature every kind implements.
// The host owns one `RasterTileStore` per `Document`.
//
// The null overload is CORRECT BUT NON-MEMOIZING: it hashes every tile on every save.
// That degradation is deliberate, and it is what lets every existing zero-argument
// `builtin_codecs()` call site keep working and still save correct pixels -- it just does
// not get the incremental CPU win.
Codec raster_codec();
Codec raster_codec(RasterTileStore* tiles);

// The PARALLEL-SAVE overload (serialize.tile_store_parallel_save): `dispatch` fans the
// per-tile encode across pool workers (or, null / default-constructed, runs it inline).
// Byte-identical to the serial save under any executor (Constraint 1); the fan-out lives
// wholly in `runtime` (L5) -- `arbc::serialize` gains no pool edge. `dispatch` must
// outlive the codec.
Codec raster_codec(RasterTileStore* tiles, TileEncodeDispatch* dispatch);

// The PARALLEL-LOAD overload (serialize.tile_store_parallel_load): `dispatch` fans the
// per-tile decode (decompress -> unshuffle -> verify-hash) across pool workers, while the
// fetch and every pool write stay on the loading thread (or, null / default-constructed, runs
// the decode inline). Byte-identical to the serial load under any executor (Constraint 1); the
// fan-out lives wholly in `runtime` (L5) -- `arbc::serialize` gains no pool edge. `dispatch`
// must outlive the codec. The mirror of the encode overload above, on the decode side.
Codec raster_codec(RasterTileStore* tiles, TileDecodeDispatch* dispatch);

} // namespace arbc
