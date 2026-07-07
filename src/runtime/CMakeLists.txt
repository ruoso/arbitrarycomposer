arbc_add_component(
  NAME runtime
  SOURCES document.cpp offline.cpp offline_sequence.cpp housekeeping.cpp housekeeping_thread.cpp
          worker_pool.cpp interactive.cpp transport.cpp playback_hints.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/offline.hpp
                 arbc/runtime/offline_sequence.hpp arbc/runtime/housekeeping.hpp
                 arbc/runtime/housekeeping_thread.hpp arbc/runtime/worker_pool.hpp
                 arbc/runtime/interactive.hpp arbc/runtime/transport.hpp
                 arbc/runtime/playback_hints.hpp
  DEPENDS base model contract compositor pool cache)

arbc_component_test(COMPONENT runtime
                    SOURCES t/housekeeping.t.cpp t/housekeeping_thread.t.cpp t/worker_pool.t.cpp
                            t/interactive.t.cpp t/transport.t.cpp t/playback_hints.t.cpp
                            t/offline_sequence.t.cpp)
