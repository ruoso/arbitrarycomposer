arbc_add_component(
  NAME runtime
  SOURCES document.cpp offline.cpp housekeeping.cpp housekeeping_thread.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/offline.hpp arbc/runtime/housekeeping.hpp
                 arbc/runtime/housekeeping_thread.hpp
  DEPENDS base model contract compositor pool)

arbc_component_test(COMPONENT runtime SOURCES t/housekeeping.t.cpp t/housekeeping_thread.t.cpp)
