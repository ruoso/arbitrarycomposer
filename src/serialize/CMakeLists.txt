# `unknown_fields.hpp` joins PUBLIC_HEADERS precisely because it names no JSON type: the
# stash is canonical JSON object TEXT in a std::string, so `arbc::runtime` (L5) can own a
# store and copy it into a snapshot without ever seeing nlohmann (doc 08:61-63, doc 17:58).
# Its json-facing half (`unknown_json.hpp`) stays INTERNAL, exactly like codec.hpp.
arbc_add_component(
  NAME serialize
  SOURCES writer.cpp reader.cpp load_context.cpp codec.cpp placeholder_content.cpp
          unknown_fields.cpp
  PUBLIC_HEADERS arbc/serialize/writer.hpp arbc/serialize/reader.hpp
                 arbc/serialize/load_context.hpp arbc/serialize/unknown_fields.hpp
  DEPENDS contract model)

# nlohmann/json is header-only, so linking it PRIVATE keeps it off libarbc's
# public interface -- no transitive link/system requirement on an embedder
# (doc 10; serialize.writer Decision 3). arbc_add_component exposes no extra-link
# parameter, so the wiring is an explicit target_link_libraries here.
target_link_libraries(arbc_serialize PRIVATE nlohmann_json::nlohmann_json)

arbc_component_test(COMPONENT serialize SOURCES t/writer.t.cpp t/reader.t.cpp)

# t/reader.t.cpp drives `load_composition` (runtime.nested_external_ref), which takes a
# `CodecTable` -- and through it nlohmann::json, which is PRIVATE to arbc_serialize. Link it
# into the component-test binary the same way, the idiom src/runtime/CMakeLists.txt already
# uses. nlohmann is header-only and invisible to check_levels.py (it scans `#include
# <arbc/...>` only), so this adds no levelization edge and no new dependency (doc 10).
if(TARGET arbc_serialize_t)
  target_link_libraries(arbc_serialize_t PRIVATE nlohmann_json::nlohmann_json)
endif()
