#pragma once

#include <arbc/contract/registry.hpp>

// The one symbol a plugin must expose across the dlopen boundary. The global
// build sets `-fvisibility=hidden` (CMAKE_CXX_VISIBILITY_PRESET hidden), so the
// entry point is annotated with default visibility explicitly.
#if defined(_WIN32)
#define ARBC_PLUGIN_EXPORT __declspec(dllexport)
#else
#define ARBC_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

// The Stage-1 (v1) plugin entry point (doc 03:164-171). A plugin -- a shared
// library loaded link-time or via `dlopen` -- exposes exactly this `extern "C"`
// symbol; the host (or, until `runtime.plugin_loading` lands in M8, imageseq's
// own end-to-end test) resolves it and calls it with the `Registry` the plugin
// registers its kinds into. No exceptions cross this boundary (doc 03:177-180);
// registration failures are values on the `Registry` result.
extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry& registry);
