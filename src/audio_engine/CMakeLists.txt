arbc_add_component(
  NAME audio_engine
  SOURCES mix.cpp lookahead.cpp
  PUBLIC_HEADERS arbc/audio_engine/mix.hpp arbc/audio_engine/lookahead.hpp
  DEPENDS contract cache)

arbc_component_test(COMPONENT audio_engine SOURCES t/mix.t.cpp t/lookahead.t.cpp)
