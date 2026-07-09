arbc_add_component(
  NAME kind_crossfade
  SOURCES crossfade_content.cpp
  PUBLIC_HEADERS arbc/kind_crossfade/crossfade_content.hpp
  DEPENDS contract)

arbc_component_test(COMPONENT kind_crossfade SOURCES t/crossfade_position.t.cpp)
