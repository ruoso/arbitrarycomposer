arbc_add_component(
  NAME runtime
  SOURCES document.cpp offline.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/offline.hpp
  DEPENDS base model contract compositor)
