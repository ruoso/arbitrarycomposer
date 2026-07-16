#pragma once

// INTERNAL runtime-component header (NOT in PUBLIC_HEADERS), split out of
// builtin_codecs.hpp by runtime.registry_bootstrap: the per-built-in persisted
// `kind_version` constants (and the one out-of-lib kind id `runtime` may
// spell), WITHOUT the `Codec` declarations that drag `nlohmann::json` into an
// includer's build. The L6 umbrella bootstrap TU (src/builtin_kinds.cpp) reuses
// these constants for its `KindMetadata` so registry metadata always equals the
// serialized `kind_version` (that task's Constraint 6) -- and the JSON library
// must not enter the umbrella TU for six string constants' sake.

namespace arbc {

// Per-built-in producer `kind_version` (runtime.document_serialize Constraint 3):
// a fixed constant chosen and pinned by that task, golden-pinned as the literal
// emitted beside `kind`. Advisory metadata -- the built-in kinds declare no
// version of their own today.
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

} // namespace arbc
