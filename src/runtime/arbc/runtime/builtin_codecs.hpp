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

// Per-built-in producer `kind_version` (Constraint 3): a fixed constant chosen and
// pinned by this task, golden-pinned as the literal emitted beside `kind`. Advisory
// metadata -- the built-in kinds declare no version of their own today.
inline constexpr const char* k_solid_kind_version = "1";
inline constexpr const char* k_tone_kind_version = "1";
inline constexpr const char* k_fade_kind_version = "1";
inline constexpr const char* k_crossfade_kind_version = "1";
inline constexpr const char* k_nested_kind_version = "1";

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

} // namespace arbc
