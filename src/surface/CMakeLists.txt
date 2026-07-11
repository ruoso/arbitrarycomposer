arbc_add_component(
  NAME surface
  SOURCES surface.cpp surface_pool.cpp
  PUBLIC_HEADERS arbc/surface/backend.hpp arbc/surface/surface.hpp
                 arbc/surface/typed_span.hpp arbc/surface/capabilities.hpp
                 arbc/surface/surface_error.hpp arbc/surface/surface_pool.hpp
                 arbc/surface/surface_ref.hpp
                 arbc/surface/testing/stub_backend.hpp
                 arbc/surface/testing/forwarding_backend.hpp
                 arbc/surface/testing/counting_backend.hpp
                 arbc/surface/testing/stub_surface.hpp
  DEPENDS base media)

arbc_component_test(COMPONENT surface SOURCES t/backend_doubles.t.cpp
                                             t/capabilities.t.cpp
                                             t/surface_pool.t.cpp
                                             t/surface_ref.t.cpp)
