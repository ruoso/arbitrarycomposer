arbc_add_component(
  NAME model
  SOURCES model.cpp
  PUBLIC_HEADERS arbc/model/model.hpp
  DEPENDS base)

arbc_component_test(COMPONENT model SOURCES t/model.t.cpp)
