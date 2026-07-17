// The zero-configuration half of plugin discovery (runtime.plugin_default_search_paths):
// an installed package discovers its own plugins with NO environment variables.
//
// A foreign application-style host: it links arbc::arbc ALONE, UNSETS ARBC_PLUGIN_PATH,
// pins the per-user and system data dirs at nonexistent paths, and calls the combined
// PluginHost::scan_standard_paths(). The ONLY remaining route to the staged install's
// plugins is the image-relative default: the libarbc image this binary loaded sits at
// <prefix>/<libdir>, so the resolver's image-relative entry is <prefix>/<libdir>/arbc/plugins
// -- exactly where the install layout (packaging/install.md D6) staged them. The
// compiled-in configure-time libdir dangles here BY DESIGN (the stage is a relocated
// `cmake --install --prefix` tree), which is why the image-relative form is load-bearing
// (refinement Decision 3).
//
// The behavioral counter: exactly TWO Loaded entries under the staged plugin dir (the
// CMake-computed dir arrives as a compile definition) -- org.arbc.image and
// org.arbc.imageseq; arbc-plugin-miniaudio has no arbc_plugin_register and is skipped.
// Both kinds must then resolve from the registry.
//
// No Catch2: this is the embedder's path, mirroring plugin_scan.cpp.

#include <arbc/runtime/plugin_host.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Portable env mutation, mirroring the in-tree loader tests: `_putenv_s(name, "")`
// removes the variable, matching `unsetenv` semantics.
void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
  ::_putenv_s(name, value);
#else
  ::setenv(name, value, 1);
#endif
}
void unset_env_var(const char* name) {
#if defined(_WIN32)
  ::_putenv_s(name, "");
#else
  ::unsetenv(name);
#endif
}

// Separator-normalized (\ -> /) for the prefix comparison below: the resolver
// composes paths with the platform separator, CMake hands the staged dir with
// forward slashes.
std::string normalized(std::string path) {
  for (char& c : path) {
    if (c == '\\') {
      c = '/';
    }
  }
  return path;
}

} // namespace

int main() {
  // Prove the env-var-FREE story: no ARBC_PLUGIN_PATH, and the user/system data
  // dirs pinned at nonexistent paths so a real ~/.local/share/arbc/plugins on the
  // machine cannot fake the discovery this test exists to prove.
  unset_env_var("ARBC_PLUGIN_PATH");
#if defined(_WIN32)
  set_env_var("LOCALAPPDATA", "C:\\arbc-no-such-local-app-data");
#else
  set_env_var("XDG_DATA_HOME", "/arbc-no-such-data-home");
  set_env_var("XDG_DATA_DIRS", "/arbc-no-such-data-dir");
#endif

  arbc::PluginHost host;
  const arbc::PluginScanReport report = host.scan_standard_paths();

  const std::string staged_dir = normalized(ARBC_STAGED_PLUGIN_DIR);
  std::size_t loaded_from_staged = 0;
  for (const arbc::PluginScanEntry& entry : report.entries) {
    if (entry.outcome == arbc::PluginScanEntry::Outcome::Loaded &&
        normalized(entry.path).starts_with(staged_dir)) {
      ++loaded_from_staged;
    }
  }

  // Exactly the two kind-registering plugins of the three shipped MODULEs, found
  // through the image-relative default alone.
  constexpr std::size_t k_expected_loaded = 2;
  if (loaded_from_staged != k_expected_loaded) {
    std::printf("plugin_default_scan: %zu plugins loaded from the staged dir %s, expected %zu "
                "(%zu report entries total)\n",
                loaded_from_staged, staged_dir.c_str(), k_expected_loaded, report.entries.size());
    for (const arbc::PluginScanEntry& entry : report.entries) {
      std::printf("plugin_default_scan:   entry %s (outcome %d) %s\n", entry.path.c_str(),
                  static_cast<int>(entry.outcome), entry.diagnostic.c_str());
    }
    return 1;
  }

  if (host.registry().factory("org.arbc.image") == nullptr ||
      host.registry().factory("org.arbc.imageseq") == nullptr) {
    std::printf("plugin_default_scan: shipped kinds did not resolve after the default scan\n");
    return 1;
  }

  std::printf("plugin_default_scan: installed plugins discovered with zero configuration -- "
              "%zu loaded from %s\n",
              loaded_from_staged, staged_dir.c_str());
  return 0;
}
