arbc_add_component(
  NAME model
  SOURCES model.cpp
  PUBLIC_HEADERS arbc/model/model.hpp arbc/model/records.hpp arbc/model/hamt.hpp
                 arbc/model/damage.hpp arbc/model/journal_entry.hpp
  DEPENDS base pool)

arbc_component_test(COMPONENT model
                    SOURCES t/model.t.cpp t/records.t.cpp t/persistent_state.t.cpp
                            t/transactions.t.cpp)
