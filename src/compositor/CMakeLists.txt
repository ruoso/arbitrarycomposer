arbc_add_component(
  NAME compositor
  SOURCES compositor.cpp tile_planning.cpp
  PUBLIC_HEADERS arbc/compositor/compositor.hpp
                 arbc/compositor/scale_ladder.hpp
                 arbc/compositor/tile_planning.hpp
  DEPENDS contract cache)

arbc_component_test(COMPONENT compositor SOURCES t/scale_ladder.t.cpp
                    t/tile_planning.t.cpp)
