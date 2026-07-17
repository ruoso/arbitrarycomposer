#include <arbc/base/expected.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/runtime/plugin_host.hpp>

#include <catch2/catch_test_macros.hpp>

// The platform loader-table lookup backing expected_image_plugin_dir() below --
// the test-side mirror of the resolver's image-relative leaf.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdlib>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// Value-level unit coverage for the runtime plugin loader, exercised WITHOUT a real
// plugin (the end-to-end dlopen path over org.arbc.imageseq lives in the integration
// test tests/plugin_loading.t.cpp). These assert the boundary discipline of doc
// 03:176-180 / doc 10:16-18 -- every failure is a value, never a throw -- the
// genuinely-opt-in scan of Constraint 2 (unset/empty ARBC_PLUGIN_PATH is a no-op),
// and the platform-conventional default-directory resolver of
// runtime.plugin_default_search_paths under controlled env.

using namespace arbc;

namespace {

// A do-nothing factory: registered but never invoked here (we only exercise the
// registry's value-level accept/reject, not content construction).
ContentFactory stub_factory() {
  return [](ContentConfig) {
    return expected<std::unique_ptr<Content>, std::string>(unexpected<std::string>("stub"));
  };
}

// Portable ARBC_PLUGIN_PATH mutation: `_putenv_s` on Windows, `setenv`/`unsetenv` on
// POSIX. Both loader suites share this shape so they compile and run on the msvc-debug
// lane. `_putenv_s(name, "")` removes the variable, matching `unsetenv` semantics.
void set_plugin_path(const char* value) {
#if defined(_WIN32)
  ::_putenv_s("ARBC_PLUGIN_PATH", value);
#else
  ::setenv("ARBC_PLUGIN_PATH", value, 1);
#endif
}
void unset_plugin_path() {
#if defined(_WIN32)
  ::_putenv_s("ARBC_PLUGIN_PATH", "");
#else
  ::unsetenv("ARBC_PLUGIN_PATH");
#endif
}

// RAII save/restore of ARBC_PLUGIN_PATH so a test that mutates it does not leak the
// value into sibling test cases in the same binary.
class ScopedPluginPath {
public:
  ScopedPluginPath() {
    const char* prior = std::getenv("ARBC_PLUGIN_PATH");
    if (prior != nullptr) {
      d_had = true;
      d_prior = prior;
    }
  }
  ~ScopedPluginPath() {
    if (d_had) {
      set_plugin_path(d_prior.c_str());
    } else {
      unset_plugin_path();
    }
  }

private:
  bool d_had = false;
  std::string d_prior;
};

// Portable mutation of an arbitrary environment variable -- the ARBC_PLUGIN_PATH
// helpers above generalized to the default-directory env inputs (XDG_DATA_HOME,
// XDG_DATA_DIRS, HOME, LOCALAPPDATA). `_putenv_s(name, "")` removes the variable,
// matching `unsetenv` semantics.
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

// RAII save/restore of one named environment variable, so a test pinning the
// default-directory inputs does not leak them into sibling test cases.
class ScopedEnvVar {
public:
  explicit ScopedEnvVar(const char* name) : d_name(name) {
    const char* prior = std::getenv(name);
    if (prior != nullptr) {
      d_had = true;
      d_prior = prior;
    }
  }
  ~ScopedEnvVar() {
    if (d_had) {
      set_env_var(d_name, d_prior.c_str());
    } else {
      unset_env_var(d_name);
    }
  }
  ScopedEnvVar(const ScopedEnvVar&) = delete;
  ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
  const char* d_name;
  bool d_had = false;
  std::string d_prior;
};

// The directory the resolver's image-relative default entry should name, computed
// independently against the resolver's own public symbol: whichever image defines
// arbc::default_plugin_directories (libarbc in a shared build, this test
// executable in a static one) is the image whose sibling `arbc/plugins` the
// resolver must return. Empty when the platform cannot answer -- then the
// resolver must have skipped the entry too.
std::string expected_image_plugin_dir() {
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (::GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          // NOLINTNEXTLINE(*-reinterpret-cast): the FROM_ADDRESS lookup ABI.
          reinterpret_cast<LPCSTR>(&arbc::default_plugin_directories), &module) == 0) {
    return "";
  }
  char buffer[MAX_PATH] = {};
  const DWORD len = ::GetModuleFileNameA(module, buffer, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    return "";
  }
  const std::string path(buffer, len);
  const std::size_t last = path.find_last_of("\\/");
  if (last == std::string::npos || last == 0) {
    return "";
  }
  return path.substr(0, last) + "\\arbc\\plugins";
#else
  Dl_info info{};
  // NOLINTNEXTLINE(*-reinterpret-cast): dladdr takes the anchor as an object pointer.
  if (::dladdr(reinterpret_cast<void*>(&arbc::default_plugin_directories), &info) == 0 ||
      info.dli_fname == nullptr) {
    return "";
  }
  const std::string path(info.dli_fname);
  const std::size_t last = path.find_last_of('/');
  if (last == std::string::npos || last == 0) {
    return "";
  }
  return path.substr(0, last) + "/arbc/plugins";
#endif
}

// The resolver's final, compiled-in entry: the configure-time install libdir
// (mirrored onto this test target by src/runtime/CMakeLists.txt) joined exactly
// as the resolver joins it -- trailing separators trimmed, platform separator.
std::string expected_install_plugin_dir() {
  std::string dir = ARBC_INSTALL_LIBDIR;
#if defined(_WIN32)
  while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) {
    dir.pop_back();
  }
  return dir + "\\arbc\\plugins";
#else
  while (!dir.empty() && dir.back() == '/') {
    dir.pop_back();
  }
  return dir + "/arbc/plugins";
#endif
}

} // namespace

TEST_CASE("load_plugin on a nonexistent path is a CannotOpen value, never a throw") {
  PluginHost host;
  expected<std::monostate, PluginLoadError> result(std::monostate{});
  REQUIRE_NOTHROW(result = host.load_plugin("/no/such/plugin/zzz.so"));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == PluginLoadError::Code::CannotOpen);
  REQUIRE(result.error().path == "/no/such/plugin/zzz.so");
  // dlopen supplies a diagnostic string; the loader captures rather than drops it.
  REQUIRE_FALSE(result.error().diagnostic.empty());
  // A failed load registers nothing.
  REQUIRE(host.registry().size() == 0);
}

TEST_CASE("scan_plugin_path is a no-op with ARBC_PLUGIN_PATH unset or empty") {
  ScopedPluginPath guard;
  PluginHost host;
  host.registry().add("org.arbc.stub", stub_factory());
  const std::size_t before = host.registry().size();

  SECTION("unset") {
    unset_plugin_path();
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_plugin_path());
    REQUIRE(report.loaded == 0);
    REQUIRE(report.entries.empty());
    REQUIRE(host.registry().size() == before);
  }

  SECTION("empty") {
    set_plugin_path("");
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_plugin_path());
    REQUIRE(report.loaded == 0);
    REQUIRE(report.entries.empty());
    REQUIRE(host.registry().size() == before);
  }
}

TEST_CASE("scan_plugin_path over a nonexistent directory loads nothing, never throws") {
  ScopedPluginPath guard;
  set_plugin_path("/no/such/plugin/dir/zzz");
  PluginHost host;
  PluginScanReport report;
  REQUIRE_NOTHROW(report = host.scan_plugin_path());
  REQUIRE(report.loaded == 0);
  REQUIRE(report.entries.empty());
  REQUIRE(host.registry().size() == 0);
}

TEST_CASE("a duplicate registration on the host registry is a value, not a throw") {
  PluginHost host;
  REQUIRE(host.registry().add("org.arbc.stub", stub_factory()).has_value());
  expected<std::monostate, RegistryError> dup(std::monostate{});
  REQUIRE_NOTHROW(dup = host.registry().add("org.arbc.stub", stub_factory()));
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error() == RegistryError::DuplicateId);
  REQUIRE(host.registry().size() == 1);
}

// Every env-derived path below is nonexistent ON PURPOSE: the resolver returning
// them verbatim, in the pinned order, is the structural proof it consults only the
// environment and the loader table -- no stat, no dlopen (the purity half of the
// claim, pinned the same way registry.tsv's opt-in scan claim is).
#if defined(_WIN32)

// enforces: 03-layer-plugin-interface#default-plugin-dirs-follow-platform-convention
TEST_CASE("default_plugin_directories follows the Windows convention under env control") {
  ScopedEnvVar guard_local_app_data("LOCALAPPDATA");
  const std::string image_dir = expected_image_plugin_dir();
  const std::string install_dir = expected_install_plugin_dir();

  SECTION("LOCALAPPDATA leads, trailing separator trimmed") {
    set_env_var("LOCALAPPDATA", "C:\\no\\such\\local-app-data\\");
    std::vector<std::string> expected = {"C:\\no\\such\\local-app-data\\arbc\\plugins"};
    if (!image_dir.empty()) {
      expected.push_back(image_dir);
    }
    expected.push_back(install_dir);
    REQUIRE(default_plugin_directories() == expected);
  }

  SECTION("unset LOCALAPPDATA skips the per-user entry") {
    unset_env_var("LOCALAPPDATA");
    std::vector<std::string> expected;
    if (!image_dir.empty()) {
      expected.push_back(image_dir);
    }
    expected.push_back(install_dir);
    REQUIRE(default_plugin_directories() == expected);
  }
}

#else

// enforces: 03-layer-plugin-interface#default-plugin-dirs-follow-platform-convention
TEST_CASE("default_plugin_directories follows the XDG convention under env control") {
  ScopedEnvVar guard_data_home("XDG_DATA_HOME");
  ScopedEnvVar guard_data_dirs("XDG_DATA_DIRS");
  ScopedEnvVar guard_home("HOME");
  const std::string image_dir = expected_image_plugin_dir();
  const std::string install_dir = expected_install_plugin_dir();

  // The image-relative and compiled-in entries close every list, in that order.
  const auto with_tail = [&](std::vector<std::string> expected) {
    if (!image_dir.empty()) {
      expected.push_back(image_dir);
    }
    expected.push_back(install_dir);
    return expected;
  };

  SECTION("XDG_DATA_HOME beats HOME; XDG_DATA_DIRS entries in listed order") {
    set_env_var("XDG_DATA_HOME", "/no/such/data-home/"); // trailing separator trims
    set_env_var("HOME", "/no/such/home");
    set_env_var("XDG_DATA_DIRS", "/no/such/aa:/no/such/bb");
    REQUIRE(default_plugin_directories() ==
            with_tail({"/no/such/data-home/arbc/plugins", "/no/such/aa/arbc/plugins",
                       "/no/such/bb/arbc/plugins"}));
  }

  SECTION("unset XDG vars fall back to $HOME/.local/share and the XDG spec dirs") {
    unset_env_var("XDG_DATA_HOME");
    set_env_var("HOME", "/no/such/home");
    unset_env_var("XDG_DATA_DIRS");
    REQUIRE(default_plugin_directories() ==
            with_tail({"/no/such/home/.local/share/arbc/plugins", "/usr/local/share/arbc/plugins",
                       "/usr/share/arbc/plugins"}));
  }

  SECTION("empty XDG_DATA_HOME behaves as unset") {
    set_env_var("XDG_DATA_HOME", "");
    set_env_var("HOME", "/no/such/home");
    set_env_var("XDG_DATA_DIRS", "/no/such/aa");
    REQUIRE(default_plugin_directories() ==
            with_tail({"/no/such/home/.local/share/arbc/plugins", "/no/such/aa/arbc/plugins"}));
  }

  SECTION("no XDG_DATA_HOME and no HOME skips the user entry; empty segments drop") {
    unset_env_var("XDG_DATA_HOME");
    unset_env_var("HOME");
    set_env_var("XDG_DATA_DIRS", "/no/such/cc/::/no/such/dd");
    REQUIRE(default_plugin_directories() ==
            with_tail({"/no/such/cc/arbc/plugins", "/no/such/dd/arbc/plugins"}));
  }
}

#endif

TEST_CASE("scan_standard_paths over nonexistent env and default dirs is a silent no-op") {
  ScopedPluginPath path_guard;
#if defined(_WIN32)
  ScopedEnvVar user_data_guard("LOCALAPPDATA");
  constexpr const char* k_fake_prefix = "C:\\no\\such";
  // The env-listed dir deliberately EQUALS the pinned per-user default, so the
  // combined scan's dedup branch executes (each directory visited at most once).
  set_env_var("LOCALAPPDATA", "C:\\no\\such\\data-home");
  set_plugin_path("C:\\no\\such\\data-home\\arbc\\plugins");
#else
  ScopedEnvVar user_data_guard("XDG_DATA_HOME");
  ScopedEnvVar data_dirs_guard("XDG_DATA_DIRS");
  constexpr const char* k_fake_prefix = "/no/such";
  // The env-listed dir deliberately EQUALS the pinned per-user default, so the
  // combined scan's dedup branch executes (each directory visited at most once).
  set_env_var("XDG_DATA_HOME", "/no/such/data-home");
  set_env_var("XDG_DATA_DIRS", "/no/such/data-dirs");
  set_plugin_path("/no/such/data-home/arbc/plugins");
#endif

  PluginHost host;
  PluginScanReport report;
  REQUIRE_NOTHROW(report = host.scan_standard_paths());

  // Per-directory containment, not machine totals (hermeticity, Constraint 9): a
  // developer machine's REAL install libdir may legitimately contribute entries,
  // but none of the pinned nonexistent directories may.
  for (const PluginScanEntry& entry : report.entries) {
    REQUIRE(entry.path.find(k_fake_prefix) == std::string::npos);
  }
}
