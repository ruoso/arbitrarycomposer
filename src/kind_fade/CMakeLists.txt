arbc_add_component(
  NAME kind_fade
  SOURCES fade_content.cpp
  PUBLIC_HEADERS arbc/kind_fade/fade_content.hpp
  DEPENDS contract)

arbc_component_test(COMPONENT kind_fade SOURCES t/fade_envelope.t.cpp)
