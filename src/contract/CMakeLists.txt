arbc_add_component(
  NAME contract
  SOURCES content.cpp
  PUBLIC_HEADERS arbc/contract/content.hpp
  DEPENDS base media surface model)

arbc_component_test(COMPONENT contract SOURCES t/snapshot_pins.t.cpp)
