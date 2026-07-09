arbc_add_component(
  NAME runtime
  SOURCES document.cpp document_serialize.cpp codec_solid.cpp codec_tone.cpp offline.cpp
          offline_sequence.cpp housekeeping.cpp housekeeping_thread.cpp
          worker_pool.cpp interactive.cpp transport.cpp playback_hints.cpp
          audio_worker_pool.cpp lookahead_pump.cpp device_monitor.cpp export_monitor.cpp
  PUBLIC_HEADERS arbc/runtime/document.hpp arbc/runtime/document_serialize.hpp
                 arbc/runtime/offline.hpp
                 arbc/runtime/offline_sequence.hpp arbc/runtime/housekeeping.hpp
                 arbc/runtime/housekeeping_thread.hpp arbc/runtime/worker_pool.hpp
                 arbc/runtime/interactive.hpp arbc/runtime/transport.hpp
                 arbc/runtime/playback_hints.hpp arbc/runtime/audio_worker_pool.hpp
                 arbc/runtime/lookahead_pump.hpp arbc/runtime/device_sink.hpp
                 arbc/runtime/device_monitor.hpp arbc/runtime/export_monitor.hpp
  DEPENDS base model contract compositor pool cache audio_engine serialize kind_solid kind_tone)

# nlohmann/json is header-only; linking it PRIVATE keeps it off libarbc's public
# interface, so the built-in-codec TUs (codec_solid.cpp, codec_tone.cpp) and the
# serialize façade (document_serialize.cpp) can see the concrete kind types + the
# JSON library at once (Decision 3), while the runtime PUBLIC headers name no JSON
# type (Constraint 7). Mirrors src/serialize/CMakeLists.txt:12.
target_link_libraries(arbc_runtime PRIVATE nlohmann_json::nlohmann_json)

arbc_component_test(COMPONENT runtime
                    SOURCES t/housekeeping.t.cpp t/housekeeping_thread.t.cpp t/worker_pool.t.cpp
                            t/interactive.t.cpp t/transport.t.cpp t/playback_hints.t.cpp
                            t/offline_sequence.t.cpp t/audio_worker_pool.t.cpp
                            t/lookahead_pump.t.cpp t/device_monitor.t.cpp
                            t/export_monitor.t.cpp t/content_binding.t.cpp
                            t/document_serialize.t.cpp)
