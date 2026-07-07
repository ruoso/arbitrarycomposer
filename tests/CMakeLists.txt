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

# org.arbc.raster contract conformance (pulls kind_raster + arbc-testing). Like
# the solid driver above, arbc-testing precedes `arbc` on the link line so its
# unresolved contract symbols resolve from the umbrella.
add_executable(arbc_raster_conformance_t raster_conformance.t.cpp)
target_link_libraries(
  arbc_raster_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_raster_conformance_t)

# org.arbc.nested contract conformance (pulls kind_nested + kind_solid +
# arbc-testing). arbc-testing precedes `arbc` on the link line (static-archive
# link order), as with the solid / raster drivers above.
add_executable(arbc_nested_conformance_t nested_conformance.t.cpp)
target_link_libraries(
  arbc_nested_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_conformance_t)

# org.arbc.nested byte-exact goldens: nested's output vs the compositor's own
# per-layer loop over the same layers (the rendering-is-recursion identity), plus
# snapshot consistency, budget flow-through, and Droste-cycle termination.
add_executable(arbc_nested_goldens_t nested_goldens.t.cpp)
target_link_libraries(arbc_nested_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_goldens_t)

# org.arbc.nested two-level caching behavioral counters: a static subtree is
# served from the parent cache; only the spine re-renders (requests_issued).
add_executable(arbc_nested_cache_t nested_cache.t.cpp)
target_link_libraries(arbc_nested_cache_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_cache_t)

# org.arbc.nested concurrency stress (tier-6, tsan lane): many concurrent renders
# of one nested content on a real multi-worker WorkerPool are race-free and
# deterministic (render_thread_safe over a thread-confined target).
add_executable(arbc_nested_concurrency_t nested_concurrency.t.cpp)
target_link_libraries(arbc_nested_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_concurrency_t)

# Raster render-vs-edit concurrency stress (tier-6, tsan lane): pinned-snapshot
# readers race an editor painting new versions.
add_executable(arbc_raster_concurrency_stress_t raster_concurrency_stress.t.cpp)
target_link_libraries(arbc_raster_concurrency_stress_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_raster_concurrency_stress_t)

add_executable(arbc_walking_skeleton_t walking_skeleton.t.cpp)
target_link_libraries(arbc_walking_skeleton_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_walking_skeleton_t)

add_executable(arbc_surface_pool_integration_t surface_pool_integration.t.cpp)
target_link_libraries(arbc_surface_pool_integration_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_surface_pool_integration_t)

add_executable(arbc_provided_surfaces_t provided_surfaces.t.cpp)
target_link_libraries(arbc_provided_surfaces_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_provided_surfaces_t)

add_executable(arbc_offline_working_space_t offline_working_space.t.cpp)
target_link_libraries(arbc_offline_working_space_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_offline_working_space_t)

add_executable(arbc_scale_ladder_golden_t scale_ladder_golden.t.cpp)
target_link_libraries(arbc_scale_ladder_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_scale_ladder_golden_t)

add_executable(arbc_tile_planning_golden_t tile_planning_golden.t.cpp)
target_link_libraries(arbc_tile_planning_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_tile_planning_golden_t)

add_executable(arbc_refinement_golden_t refinement_golden.t.cpp)
target_link_libraries(arbc_refinement_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_refinement_golden_t)

add_executable(arbc_anchored_viewports_golden_t anchored_viewports_golden.t.cpp)
target_link_libraries(arbc_anchored_viewports_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_anchored_viewports_golden_t)

add_executable(arbc_damage_planning_golden_t damage_planning_golden.t.cpp)
target_link_libraries(arbc_damage_planning_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_damage_planning_golden_t)

add_executable(arbc_temporal_coalescing_golden_t temporal_coalescing_golden.t.cpp)
target_link_libraries(arbc_temporal_coalescing_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_temporal_coalescing_golden_t)

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

# compositor.pull_service async concurrency stress (tier-6, tsan lane): many
# concurrent worker-backed pulls settle race-free into the single-writer cache.
add_executable(arbc_pull_service_async_t pull_service_async.t.cpp)
target_link_libraries(arbc_pull_service_async_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_pull_service_async_t)
