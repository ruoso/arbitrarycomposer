arbc_add_component(
  NAME serialize
  SOURCES writer.cpp reader.cpp load_context.cpp
  PUBLIC_HEADERS arbc/serialize/writer.hpp arbc/serialize/reader.hpp
                 arbc/serialize/load_context.hpp
  DEPENDS contract model)

# nlohmann/json is header-only, so linking it PRIVATE keeps it off libarbc's
# public interface -- no transitive link/system requirement on an embedder
# (doc 10; serialize.writer Decision 3). arbc_add_component exposes no extra-link
# parameter, so the wiring is an explicit target_link_libraries here.
target_link_libraries(arbc_serialize PRIVATE nlohmann_json::nlohmann_json)

arbc_component_test(COMPONENT serialize SOURCES t/writer.t.cpp t/reader.t.cpp)
