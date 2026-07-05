arbc_add_component(
  NAME contract
  SOURCES content.cpp render_completion.cpp
  PUBLIC_HEADERS arbc/contract/content.hpp
  DEPENDS base media surface model)

arbc_component_test(COMPONENT contract SOURCES t/snapshot_pins.t.cpp t/async_render.t.cpp
                                                t/operator_members.t.cpp t/temporal_fields.t.cpp)
