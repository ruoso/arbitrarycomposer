arbc_add_component(
  NAME runtime
  SOURCES document.cpp offline.cpp housekeeping.cpp housekeeping_thread.cpp worker_pool.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/offline.hpp arbc/runtime/housekeeping.hpp
                 arbc/runtime/housekeeping_thread.hpp arbc/runtime/worker_pool.hpp
  DEPENDS base model contract compositor pool)

arbc_component_test(COMPONENT runtime
                    SOURCES t/housekeeping.t.cpp t/housekeeping_thread.t.cpp t/worker_pool.t.cpp)
