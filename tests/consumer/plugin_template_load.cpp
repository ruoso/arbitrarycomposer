// The packaging.plugin_helper half of the out-of-tree consumer: doc 10:47-49's
// promise that third-party plugin builds are ONE LINE, pinned end-to-end.
//
// run_staged_install.cmake compiled examples/plugin-template -- a standalone foreign
// project whose only target-defining lines are find_package(arbc CONFIG REQUIRED)
// and one arbc_add_plugin() call -- against the SAME staged install prefix this
// consumer uses, and handed the produced module's path in as the
// ARBC_TEMPLATE_MODULE compile definition. This host (an application-style embedder
// linking arbc::arbc alone) loads it through the production
// PluginHost::load_plugin() and asserts the template's kind actually registered:
// the helper produced a REAL plugin the shipped loader runs, not merely a file that
// links. Explicit by-path load, not the ARBC_PLUGIN_PATH scan -- the template
// module lives in its own build directory, not the installed plugin dir, and
// plugin_scan already owns the scan path.
//
// No Catch2: like plugin_scan, this is the embedder's path, driven in the
// core-only configuration too.

#include <arbc/runtime/plugin_host.hpp>

#include <cstdio>

// enforces: 10-tooling-and-packaging#third-party-plugin-builds-are-one-line
int main() {
  arbc::PluginHost host;

  const auto loaded = host.load_plugin(ARBC_TEMPLATE_MODULE);
  if (!loaded.has_value()) {
    std::printf("plugin_template_load: the arbc_add_plugin-built module failed to load: "
                "%s (code %d, %s)\n",
                loaded.error().path.c_str(), static_cast<int>(loaded.error().code),
                loaded.error().diagnostic.c_str());
    return 1;
  }

  // Loaded is not enough: the entry point must have REGISTERED the template's kind.
  if (host.registry().factory("org.example.template") == nullptr) {
    std::printf("plugin_template_load: module loaded but org.example.template is not "
                "in the Registry\n");
    return 1;
  }

  std::printf("plugin_template_load: template module loaded through PluginHost, "
              "org.example.template registered\n");
  return 0;
}
