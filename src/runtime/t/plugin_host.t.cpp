#include <arbc/base/expected.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/runtime/plugin_host.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <variant>

// Value-level unit coverage for the runtime plugin loader, exercised WITHOUT a real
// plugin (the end-to-end dlopen path over org.arbc.imageseq lives in the integration
// test tests/plugin_loading.t.cpp). These assert the boundary discipline of doc
// 03:176-180 / doc 10:16-18 -- every failure is a value, never a throw -- and the
// genuinely-opt-in scan of Constraint 2 (unset/empty ARBC_PLUGIN_PATH is a no-op).

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
