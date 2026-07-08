# arbc-plugin-miniaudio -- the out-of-lib reference `DeviceSink` backend (doc 12)
# and the device-side end-to-end test of the runtime plugin path (doc 17 "the
# codec line", generalized to device backends; device_monitor.md D1/D4).
#
# It carries its OWN private miniaudio-class backend dependency
# (third_party/maudio.h), which must never enter `libarbc` / `arbc-testing`
# (device_monitor.md D4). This is a hand-rolled MODULE target, NOT an
# `arbc_add_component` (those fold into `libarbc`, dragging the OS-audio backend
# into core); `packaging.plugin_helper` (M9) migrates it onto the shipped
# `arbc_add_plugin()` helper later (mirrors imageseq, Decision 6).

# The sink implementation as a STATIC library so BOTH the loadable MODULE and the
# in-repo test binaries construct the sink directly, while the backend dependency
# stays private to this library (and the MODULE), never in `libarbc`.
add_library(
  arbc-plugin-miniaudio-impl STATIC
  miniaudio_sink.cpp
  maudio.cpp)
target_include_directories(
  arbc-plugin-miniaudio-impl
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"                # arbc/device_miniaudio/*.hpp
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")  # maudio.h -- private backend dep
target_compile_features(arbc-plugin-miniaudio-impl PUBLIC cxx_std_20)
# Links the umbrella `arbc` (PUBLIC: DeviceSink/runtime symbols + headers) plus its
# own PRIVATE backend dep (compiled into maudio.cpp). The backend dep is NOT added
# to any `arbc_*` component, to `arbc`, or to `arbc-testing` (device_monitor.md D4).
target_link_libraries(arbc-plugin-miniaudio-impl PUBLIC arbc PRIVATE arbc_build_flags)

# The shipped loadable artifact: a MODULE carrying only the extern "C" factory
# entry point, linking the impl (+ its private backend dep) and the umbrella `arbc`.
add_library(arbc-plugin-miniaudio MODULE miniaudio_plugin.cpp)
target_link_libraries(arbc-plugin-miniaudio PRIVATE arbc-plugin-miniaudio-impl arbc arbc_build_flags)
