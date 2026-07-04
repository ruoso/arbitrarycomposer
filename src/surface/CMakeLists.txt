arbc_add_component(
  NAME surface
  SOURCES surface.cpp
  PUBLIC_HEADERS arbc/surface/backend.hpp arbc/surface/surface.hpp
  DEPENDS base media)
