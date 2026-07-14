arbc_add_component(
  NAME model
  SOURCES model.cpp journal.cpp
  PUBLIC_HEADERS arbc/model/model.hpp arbc/model/records.hpp arbc/model/hamt.hpp
                 arbc/model/damage.hpp arbc/model/journal_entry.hpp arbc/model/journal.hpp
  DEPENDS base pool media)

arbc_component_test(COMPONENT model
                    SOURCES t/model.t.cpp t/records.t.cpp t/persistent_state.t.cpp
                            t/transactions.t.cpp t/journal.t.cpp t/editable_facet.t.cpp
                            t/composition_membership.t.cpp t/damage.t.cpp
                            t/temporal_placement.t.cpp t/workspace_backing.t.cpp
                            t/per_object_revision.t.cpp)
