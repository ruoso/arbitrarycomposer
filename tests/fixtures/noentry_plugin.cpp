// A deliberately entry-point-less shared object: it opens cleanly under dlopen but
// exposes NO `arbc_plugin_register` symbol, so the production loader must report
// MissingEntryPoint on an explicit `load_plugin` (and SkippedNoEntry during a scan).
// Fixture for tests/plugin_loading.t.cpp. It carries no arbc dependency, so it
// resolves under RTLD_NOW with zero external symbols and cannot fail to open for an
// unrelated reason.
extern "C" int arbc_test_noentry_marker(void) { return 42; }
