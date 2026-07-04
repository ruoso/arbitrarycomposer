arbc_add_component(
  NAME base
  SOURCES transform.cpp
  PUBLIC_HEADERS arbc/base/geometry.hpp arbc/base/ids.hpp arbc/base/time.hpp
                 arbc/base/transform.hpp)

arbc_component_test(COMPONENT base SOURCES t/transform.t.cpp)
