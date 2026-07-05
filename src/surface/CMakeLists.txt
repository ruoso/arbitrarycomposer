arbc_add_component(
  NAME surface
  SOURCES surface.cpp
  PUBLIC_HEADERS arbc/surface/backend.hpp arbc/surface/surface.hpp
                 arbc/surface/capabilities.hpp arbc/surface/surface_error.hpp
  DEPENDS base media)

arbc_component_test(COMPONENT surface SOURCES t/capabilities.t.cpp)
