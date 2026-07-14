# `unknown_fields.hpp` joins PUBLIC_HEADERS precisely because it names no JSON type: the
# stash is canonical JSON object TEXT in a std::string, so `arbc::runtime` (L5) can own a
# store and copy it into a snapshot without ever seeing nlohmann (doc 08:61-63, doc 17:58).
# Its json-facing half (`unknown_json.hpp`) stays INTERNAL, exactly like codec.hpp.
#
# `blob_compress.hpp` joins them for the same reason and it is load-bearing: the tile-blob
# compress/decompress seam (serialize.zstd_dep, doc 08 Principle 8) names NO zstd type --
# it trades in std::span<const std::byte> / std::vector<std::byte> / arbc::expected -- so
# zstd stays off libarbc's public compile interface entirely.
arbc_add_component(
  NAME serialize
  SOURCES writer.cpp reader.cpp load_context.cpp save_context.cpp codec.cpp
          placeholder_content.cpp unknown_fields.cpp blob_compress.cpp tile_blob.cpp
  PUBLIC_HEADERS arbc/serialize/writer.hpp arbc/serialize/reader.hpp
                 arbc/serialize/load_context.hpp arbc/serialize/save_context.hpp
                 arbc/serialize/unknown_fields.hpp arbc/serialize/blob_compress.hpp
                 arbc/serialize/tile_blob.hpp
  DEPENDS contract model)

# nlohmann/json is header-only, so linking it PRIVATE keeps it off libarbc's
# public interface -- no transitive link/system requirement on an embedder
# (doc 10; serialize.writer Decision 3). arbc_add_component exposes no extra-link
# parameter, so the wiring is an explicit target_link_libraries here.
target_link_libraries(arbc_serialize PRIVATE nlohmann_json::nlohmann_json)

# zstd, the core's second dependency (doc 10:27; serialize.zstd_dep) -- the tile-blob
# compressor. Linked PRIVATE for the same purpose as the nlohmann line above, but it
# does NOT inherit nlohmann's "header-only, therefore free" argument: zstd is a compiled
# C library. What makes PRIVATE hold here is blob_compress.hpp naming no zstd type, so no
# embedder ever compiles against zstd (zstd_dep Decision 2 / Constraint 3). `arbc_zstd` is
# the top-level shim normalizing the system and fetched target names into one (Decision 3).
# zstd adds no component-graph edge -- check_levels.py scans `#include <arbc/...>` only, so
# an external target is invisible to it and ALLOWED needs no entry (Constraint 5).
target_link_libraries(arbc_serialize PRIVATE arbc_zstd)

arbc_component_test(COMPONENT serialize SOURCES t/writer.t.cpp t/reader.t.cpp t/tile_blob.t.cpp)

# t/reader.t.cpp drives `load_composition` (runtime.nested_external_ref), which takes a
# `CodecTable` -- and through it nlohmann::json, which is PRIVATE to arbc_serialize. Link it
# into the component-test binary the same way, the idiom src/runtime/CMakeLists.txt already
# uses. nlohmann is header-only and invisible to check_levels.py (it scans `#include
# <arbc/...>` only), so this adds no levelization edge and no new dependency (doc 10).
if(TARGET arbc_serialize_t)
  target_link_libraries(arbc_serialize_t PRIVATE nlohmann_json::nlohmann_json)
endif()
