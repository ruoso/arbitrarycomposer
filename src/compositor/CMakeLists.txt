arbc_add_component(
  NAME compositor
  SOURCES compositor.cpp tile_planning.cpp refinement.cpp anchored_viewports.cpp
          damage_planning.cpp operator_graph.cpp pull_service.cpp
  PUBLIC_HEADERS arbc/compositor/compositor.hpp
                 arbc/compositor/scale_ladder.hpp
                 arbc/compositor/tile_planning.hpp
                 arbc/compositor/refinement.hpp
                 arbc/compositor/counters.hpp
                 arbc/compositor/anchored_viewports.hpp
                 arbc/compositor/damage_planning.hpp
                 arbc/compositor/operator_graph.hpp
                 arbc/compositor/pull_service.hpp
                 arbc/compositor/provided_surface.hpp
  DEPENDS contract cache)

arbc_component_test(COMPONENT compositor SOURCES t/scale_ladder.t.cpp
                    t/tile_planning.t.cpp t/refinement.t.cpp
                    t/counters.t.cpp t/anchored_viewports.t.cpp
                    t/damage_planning.t.cpp t/temporal_coalescing.t.cpp
                    t/temporal_cache.t.cpp t/operator_graph.t.cpp
                    t/pull_service.t.cpp)
