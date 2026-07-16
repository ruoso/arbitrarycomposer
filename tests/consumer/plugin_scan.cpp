// The plugin-install-layout half of the out-of-tree consumer (packaging.install).
//
// A foreign application-style host: it links arbc::arbc ALONE, sets ARBC_PLUGIN_PATH to
// the INSTALLED plugin directory (<prefix>/<libdir>/arbc/plugins, set as this test's
// ENVIRONMENT by the consumer CMakeLists), and runs the very PluginHost::scan_plugin_path()
// the runtime already ships. This is the end-to-end proof doc 17:28 asks for -- the three
// shipped MODULE plugins install "alongside as loadable modules, not as link targets" and
// are discoverable exactly where an ARBC_PLUGIN_PATH host would look.
//
// The behavioral-counter assertion: of the three installed plugins, exactly TWO register a
// kind through `arbc_plugin_register` -- org.arbc.image and org.arbc.imageseq. The third,
// arbc-plugin-miniaudio, is a DeviceSink backend whose entry point is arbc_device_sink_create,
// NOT a kind registration, so the lenient scan (Decision 3) records it as SkippedNoEntry and
// does not count it in `loaded`. So the honest count is 2 loaded, 0 CannotOpen -- a plugin
// that failed to OPEN is the real failure this asserts against.
//
// No Catch2: this is the embedder's path, driven in the core-only configuration.

#include <arbc/runtime/plugin_host.hpp>

#include <cstdio>

int main() {
  arbc::PluginHost host;
  const arbc::PluginScanReport report = host.scan_plugin_path();

  int cannot_open = 0;
  for (const arbc::PluginScanEntry& entry : report.entries) {
    if (entry.outcome == arbc::PluginScanEntry::Outcome::CannotOpen) {
      ++cannot_open;
      std::printf("plugin_scan: a shipped plugin failed to open: %s (%s)\n", entry.path.c_str(),
                  entry.diagnostic.c_str());
    }
  }
  if (cannot_open != 0) {
    std::printf("plugin_scan: %d installed plugin(s) failed to open\n", cannot_open);
    return 1;
  }

  // Exactly the two kind-registering plugins of the three shipped MODULEs.
  constexpr std::size_t k_expected_loaded = 2;
  if (report.loaded != k_expected_loaded) {
    std::printf("plugin_scan: ARBC_PLUGIN_PATH scan loaded %zu kinds, expected %zu\n",
                report.loaded, k_expected_loaded);
    return 1;
  }

  std::printf("plugin_scan: installed plugins discovered on ARBC_PLUGIN_PATH -- %zu loaded, "
              "%zu entries, no open failures\n",
              report.loaded, report.entries.size());
  return 0;
}
