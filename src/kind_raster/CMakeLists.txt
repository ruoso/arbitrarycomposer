arbc_add_component(
  NAME kind_raster
  SOURCES raster_content.cpp
  PUBLIC_HEADERS arbc/kind_raster/raster_content.hpp
  DEPENDS contract)

arbc_component_test(COMPONENT kind_raster SOURCES t/raster_goldens.t.cpp t/raster_paint.t.cpp)
