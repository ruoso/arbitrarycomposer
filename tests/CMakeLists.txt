# Cross-component tests: integration, claims-register golden tests, and the
# harness smoke test. Component unit tests live in src/<component>/t/.

add_executable(arbc_smoke_t smoke.t.cpp)
target_link_libraries(arbc_smoke_t PRIVATE Catch2::Catch2WithMain
                                           arbc_build_flags)
catch_discover_tests(arbc_smoke_t)

# Contract conformance driver (doc 16, contract.conformance_suite): runs the
# public arbc-testing suite over one purpose-built double per contract branch
# plus the org.arbc.solid reference kind. Cross-component (pulls kind_solid), so
# it lives here. arbc-testing precedes `arbc` on the link line so its unresolved
# contract symbols resolve from the umbrella (static-archive link order).
add_executable(arbc_contract_conformance_t contract_conformance.t.cpp)
target_link_libraries(
  arbc_contract_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_contract_conformance_t)

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

add_executable(arbc_tile_planning_golden_t tile_planning_golden.t.cpp)
target_link_libraries(arbc_tile_planning_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_tile_planning_golden_t)

# Cross-component concurrency stress + litmus (design doc 16 tier 6). These
# combine model publish/pin (L2), pool reclamation/arena (L1), and runtime
# housekeeping (L5), so per doc 17:153 they live here in top-level tests/ and
# link the umbrella `arbc`. The brief TEST_CASEs run per-push (also under the
# `tsan` preset short-form); the `[.nightly]` sweeps are hidden and run only when
# a nightly job invokes the binary with the `[.nightly]` tag.
add_executable(arbc_stress_publish_pin_t stress_publish_pin.t.cpp)
target_link_libraries(arbc_stress_publish_pin_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_stress_publish_pin_t)

add_executable(arbc_stress_reclamation_queue_t stress_reclamation_queue.t.cpp)
target_link_libraries(arbc_stress_reclamation_queue_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_stress_reclamation_queue_t)

add_executable(arbc_arena_growth_litmus_t arena_growth_litmus.t.cpp)
target_link_libraries(arbc_arena_growth_litmus_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_arena_growth_litmus_t)
