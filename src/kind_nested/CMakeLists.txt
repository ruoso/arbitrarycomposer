arbc_add_component(
  NAME kind_nested
  SOURCES nested_content.cpp
  PUBLIC_HEADERS arbc/kind_nested/nested_content.hpp
  DEPENDS contract)

arbc_component_test(COMPONENT kind_nested SOURCES t/nested_meta.t.cpp t/nested_audio_meta.t.cpp)
