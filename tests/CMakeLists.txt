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

# --- org.arbc.imageseq (kinds.imageseq_plugin) --------------------------------
# The out-of-lib reference Timed kind's enforcing tests. They live here (not in
# plugins/) because check_claims.py / check_levels.py scan src/tests/testing, not
# plugins/; the ones that construct the content link the plugin's impl archive
# (`arbc-plugin-imageseq-impl`), while the dlopen/containment drivers reach the
# built artifacts only through the paths passed as compile definitions. The
# checked-in fixture sequence lives under the plugin's test data (doc 17:171).
set(ARBC_IMAGESEQ_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/plugins/imageseq/t/fixtures")

# Contract conformance over org.arbc.imageseq (arbc-testing before arbc; impl
# archive before both so ImageSeqContent resolves).
add_executable(arbc_imageseq_conformance_t imageseq_conformance.t.cpp)
target_link_libraries(
  arbc_imageseq_conformance_t
  PRIVATE arbc-plugin-imageseq-impl arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_conformance_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_conformance_t)

# Content-local achieved_time contract + byte-exact goldens + decode counter.
add_executable(arbc_imageseq_achieved_time_t imageseq_achieved_time.t.cpp)
target_link_libraries(arbc_imageseq_achieved_time_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_achieved_time_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_achieved_time_t)

# Through-compositor achieved-time coalescing (counters + cache hits/misses).
add_executable(arbc_imageseq_temporal_t imageseq_temporal.t.cpp)
target_link_libraries(arbc_imageseq_temporal_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_temporal_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_temporal_t)

# Temporal prefetch ring + decoder pre-roll (playback_hint) behavioral counters.
add_executable(arbc_imageseq_prefetch_t imageseq_prefetch.t.cpp)
target_link_libraries(arbc_imageseq_prefetch_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_prefetch_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_prefetch_t)

# Provided-surface honored/released + advisory-hint byte-identity (constraint 6).
add_executable(arbc_imageseq_provided_surface_t imageseq_provided_surface.t.cpp)
target_link_libraries(arbc_imageseq_provided_surface_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_provided_surface_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_provided_surface_t)

# Per-content serialization (render_thread_safe() == false) stress (tsan lane).
add_executable(arbc_imageseq_concurrency_stress_t imageseq_concurrency_stress.t.cpp)
target_link_libraries(arbc_imageseq_concurrency_stress_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_concurrency_stress_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_concurrency_stress_t)

# The permanent end-to-end dlopen plugin-path test: boundary-only (no impl link),
# reaching the kind solely through extern "C" arbc_plugin_register.
add_executable(arbc_imageseq_plugin_path_t imageseq_plugin_path.t.cpp)
target_link_libraries(arbc_imageseq_plugin_path_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags ${CMAKE_DL_LIBS})
target_compile_definitions(
  arbc_imageseq_plugin_path_t
  PRIVATE ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>"
          ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_plugin_path_t)

# The codec-line containment proof: no decode symbol in libarbc, present in the
# plugin's impl archive and loadable module (byte scan of the built artifacts).
add_executable(arbc_imageseq_containment_t imageseq_containment.t.cpp)
target_link_libraries(arbc_imageseq_containment_t
                      PRIVATE Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(
  arbc_imageseq_containment_t
  PRIVATE ARBC_LIBARBC_FILE="$<TARGET_FILE:arbc>"
          ARBC_IMAGESEQ_IMPL_FILE="$<TARGET_FILE:arbc-plugin-imageseq-impl>"
          ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>")
catch_discover_tests(arbc_imageseq_containment_t)
