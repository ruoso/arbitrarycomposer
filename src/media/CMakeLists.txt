arbc_add_component(
  NAME media
  SOURCES pixel_format.cpp
  PUBLIC_HEADERS arbc/media/pixel_format.hpp arbc/media/color_space.hpp
                 arbc/media/surface_format.hpp
  DEPENDS base)

arbc_component_test(COMPONENT media SOURCES t/pixel_format.t.cpp)
