arbc_add_component(
  NAME cache
  SOURCES arbc/cache/keyed_store.cpp
  PUBLIC_HEADERS arbc/cache/keyed_store.hpp
  DEPENDS base)

arbc_component_test(COMPONENT cache SOURCES t/keyed_store.t.cpp)
