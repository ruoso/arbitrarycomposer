arbc_add_component(
  NAME runtime
  SOURCES document.cpp editable_binding.cpp document_serialize.cpp codec_solid.cpp codec_tone.cpp
          codec_fade.cpp codec_crossfade.cpp codec_nested.cpp
          external_composition_loader.cpp filesystem_asset_source.cpp
          pending_external_loads.cpp
          operator_binding.cpp pull_identity.cpp
          offline.cpp
          offline_sequence.cpp housekeeping.cpp housekeeping_thread.cpp
          worker_pool.cpp worker_dispatch.cpp
          interactive.cpp transport.cpp host_viewport.cpp damage_router.cpp
          playback_hints.cpp
          audio_worker_pool.cpp lookahead_pump.cpp device_monitor.cpp export_monitor.cpp
          plugin_host.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/editable_binding.hpp
                 arbc/runtime/document_serialize.hpp
                 arbc/runtime/external_composition_loader.hpp
                 arbc/runtime/pending_external_loads.hpp
                 arbc/runtime/filesystem_asset_source.hpp
                 arbc/runtime/operator_binding.hpp
                 arbc/runtime/pull_identity.hpp
                 arbc/runtime/offline.hpp
                 arbc/runtime/offline_sequence.hpp arbc/runtime/housekeeping.hpp
                 arbc/runtime/housekeeping_targets.hpp
                 arbc/runtime/housekeeping_thread.hpp arbc/runtime/worker_pool.hpp
                 arbc/runtime/worker_dispatch.hpp
                 arbc/runtime/interactive.hpp arbc/runtime/transport.hpp
                 arbc/runtime/host_viewport.hpp arbc/runtime/damage_router.hpp
                 arbc/runtime/playback_hints.hpp arbc/runtime/audio_worker_pool.hpp
                 arbc/runtime/lookahead_pump.hpp arbc/runtime/device_sink.hpp
                 arbc/runtime/device_monitor.hpp arbc/runtime/export_monitor.hpp
                 arbc/runtime/plugin_host.hpp
  DEPENDS base model contract compositor pool cache audio_engine serialize kind_solid kind_tone
          kind_fade kind_crossfade kind_nested)

# nlohmann/json is header-only; linking it PRIVATE keeps it off libarbc's public
# interface, so the built-in-codec TUs (codec_solid.cpp, codec_tone.cpp) and the
# serialize façade (document_serialize.cpp) can see the concrete kind types + the
# JSON library at once (Decision 3), while the runtime PUBLIC headers name no JSON
# type (Constraint 7). Mirrors src/serialize/CMakeLists.txt:12.
target_link_libraries(arbc_runtime PRIVATE nlohmann_json::nlohmann_json)

# The platform dynamic loader (POSIX libdl / empty where dlopen is in libc): the
# first in-lib use of `dlopen`, backing the plugin_host loader. Linked PRIVATE, the
# same idiom as nlohmann_json above; `dlopen`/`libdl` is a platform facility, not an
# `arbc_*` component, so it is invisible to check_levels.py (no new edge).
target_link_libraries(arbc_runtime PRIVATE ${CMAKE_DL_LIBS})

arbc_component_test(COMPONENT runtime
                    SOURCES t/housekeeping.t.cpp t/housekeeping_thread.t.cpp
                            t/housekeeping_document.t.cpp t/worker_pool.t.cpp
                            t/interactive.t.cpp t/transport.t.cpp t/host_viewport.t.cpp
                            t/damage_router.t.cpp
                            t/playback_hints.t.cpp
                            t/offline_sequence.t.cpp t/audio_worker_pool.t.cpp
                            t/lookahead_pump.t.cpp t/device_monitor.t.cpp
                            t/export_monitor.t.cpp t/content_binding.t.cpp
                            t/editable_binding.t.cpp t/editable_sink_multiplex.t.cpp
                            t/document_serialize.t.cpp t/operator_codecs.t.cpp
                            t/nested_codec.t.cpp
                            t/pull_identity.t.cpp
                            t/bench_smoke.t.cpp
                            t/plugin_host.t.cpp)

# The interactive worker-count sweep (`runtime.interactive_worker_count_default`, doc 16
# tier 9): three scenes x the worker counts 0/1/2/4/hw-1, reporting wall-clock AND the
# frame loop's behavioral counters. Built only under ARBC_BENCHMARKS (the `bench` preset),
# so dev/asan/tsan/coverage neither fetch Google Benchmark nor compile it -- benchmarks
# trend, they do not gate (doc 16:225-226). Its workload bodies carry diff coverage via
# t/bench_smoke.t.cpp above, which drives the shared header in the normal test build.
arbc_component_bench(COMPONENT runtime SOURCES bench/interactive_worker_bench.cpp)

# t/nested_codec.t.cpp drives `nested_codec()`'s two halves directly, so it names
# `Codec` -- and through it `nlohmann::json`, which is PRIVATE to arbc_runtime. Link it
# into the component-test binary the same way, the idiom tests/CMakeLists.txt already uses
# for every target holding a CodecTable. nlohmann is header-only and invisible to
# check_levels.py (it scans `#include <arbc/...>` only), so this adds no levelization edge
# and no new dependency (doc 10).
if(TARGET arbc_runtime_t)
  target_link_libraries(arbc_runtime_t PRIVATE nlohmann_json::nlohmann_json)
endif()
