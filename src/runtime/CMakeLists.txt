arbc_add_component(
  NAME runtime
  SOURCES document.cpp offline.cpp housekeeping.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/offline.hpp arbc/runtime/housekeeping.hpp
  DEPENDS base model contract compositor pool)

arbc_component_test(COMPONENT runtime SOURCES t/housekeeping.t.cpp)
