arbc_add_component(
  NAME kind_tone
  SOURCES tone_content.cpp
  PUBLIC_HEADERS arbc/kind_tone/tone_content.hpp
  DEPENDS contract)

arbc_component_test(COMPONENT kind_tone SOURCES t/tone_goldens.t.cpp t/tone_rates.t.cpp)
