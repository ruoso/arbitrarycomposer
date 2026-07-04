arbc_add_component(
  NAME pool
  SOURCES slot_store.cpp
  PUBLIC_HEADERS arbc/pool/chunk_source.hpp arbc/pool/slab_directory.hpp
                 arbc/pool/slot_store.hpp arbc/pool/typed_store.hpp
  DEPENDS base)

arbc_component_test(COMPONENT pool SOURCES t/pool.t.cpp)
