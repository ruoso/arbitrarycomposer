arbc_add_component(
  NAME backend_cpu
  SOURCES cpu_backend.cpp
  PUBLIC_HEADERS arbc/backend_cpu/cpu_backend.hpp
  DEPENDS base media surface)

arbc_component_test(COMPONENT backend_cpu SOURCES t/cpu_backend.t.cpp)
