arbc_add_component(
  NAME cache
  SOURCES arbc/cache/keyed_store.cpp
  PUBLIC_HEADERS arbc/cache/keyed_store.hpp arbc/cache/key_shapes.hpp
  DEPENDS base surface)

arbc_component_test(COMPONENT cache SOURCES t/keyed_store.t.cpp
                                           t/key_shapes.t.cpp)
