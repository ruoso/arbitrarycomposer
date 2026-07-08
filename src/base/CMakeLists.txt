arbc_add_component(
  NAME base
  SOURCES rational_time.cpp transform.cpp rt_safety.cpp
  PUBLIC_HEADERS arbc/base/expected.hpp arbc/base/geometry.hpp arbc/base/ids.hpp
                 arbc/base/rational_time.hpp arbc/base/rt_safety.hpp arbc/base/time.hpp
                 arbc/base/transform.hpp)

arbc_component_test(COMPONENT base SOURCES t/transform.t.cpp t/expected.t.cpp
                                          t/rational_time.t.cpp t/rt_safety.t.cpp)
