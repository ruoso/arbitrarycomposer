# Cross-component tests: integration, claims-register golden tests, and the
# harness smoke test. Component unit tests live in src/<component>/t/.

add_executable(arbc_smoke_t smoke.t.cpp)
target_link_libraries(arbc_smoke_t PRIVATE Catch2::Catch2WithMain
                                           arbc_build_flags)
catch_discover_tests(arbc_smoke_t)

add_executable(arbc_walking_skeleton_t walking_skeleton.t.cpp)
target_link_libraries(arbc_walking_skeleton_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_walking_skeleton_t)

add_executable(arbc_surface_pool_integration_t surface_pool_integration.t.cpp)
target_link_libraries(arbc_surface_pool_integration_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_surface_pool_integration_t)

add_executable(arbc_scale_ladder_golden_t scale_ladder_golden.t.cpp)
target_link_libraries(arbc_scale_ladder_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_scale_ladder_golden_t)
