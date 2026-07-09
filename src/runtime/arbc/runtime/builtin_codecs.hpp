#pragma once

// INTERNAL runtime-component header (NOT in PUBLIC_HEADERS): it names
// `nlohmann::json` (through `Codec`), kept off the runtime public interface and
// linked PRIVATE, exactly as `arbc/serialize/codec.hpp` is for `arbc_serialize`
// (kind_params Decision 3, runtime.document_serialize Decision 3).
//
// The two built-in leaf-kind codecs live in their own TUs (`codec_solid.cpp`,
// `codec_tone.cpp`) -- the only layer that legally sees both a concrete kind type
// (L4 `kind_solid`/`kind_tone`) and the JSON library (via L4 `serialize`) is
// `runtime` (L5). Each factory returns a `Codec{serialize, deserialize}` pair over
// its kind's `params`; the façade (`document_serialize.cpp`) assembles them into a
// `CodecTable` for the save path and wraps their `deserialize` with per-load kind
// recording for the read path (Decision 4).

#include <arbc/serialize/codec.hpp>

namespace arbc {

// Per-built-in producer `kind_version` (Constraint 3): a fixed constant chosen and
// pinned by this task, golden-pinned as the literal emitted beside `kind`. Advisory
// metadata -- the built-in kinds declare no version of their own today.
inline constexpr const char* k_solid_kind_version = "1";
inline constexpr const char* k_tone_kind_version = "1";

// org.arbc.solid: encodes/decodes `SolidContent`'s premultiplied `Rgba` as a
// `{"color": [r, g, b, a]}` params object.
Codec solid_codec();

// org.arbc.tone: encodes/decodes `ToneContent`'s `{frequency_hz, amplitude}` as a
// `{"amplitude": <real>, "frequency_hz": <int>}` params object.
Codec tone_codec();

} // namespace arbc
