// The production host-side plugin loader's end-to-end test (runtime.plugin_loading,
// M8; doc 03:164-171,188-207; doc 10:49-52; doc 17:60,150-159). It drives the
// `PluginHost` loader over the real `arbc-plugin-imageseq` MODULE across the
// `extern "C" arbc_plugin_register` boundary -- NO impl-archive link, the kind is
// reached solely through the loaded `.so`. This is the production generalization of
// the hand-rolled dlopen dance in tests/imageseq_plugin_path.t.cpp.
//
// Every failing call is wrapped in REQUIRE_NOTHROW: errors are values across the
// boundary, never exceptions (doc 03:176-180, doc 10:16-18). The plugin path, its
// directory, the fixture directory, and an entry-point-less fixture module arrive as
// compile definitions.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/plugin_host.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <memory>
#include <string>
#include <variant>

using namespace arbc;

namespace {

constexpr const char* k_kind_id = "org.arbc.imageseq";

// RAII save/restore of ARBC_PLUGIN_PATH so a scan test does not leak the value into
// sibling test cases in the same binary.
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
      ::setenv("ARBC_PLUGIN_PATH", d_prior.c_str(), 1);
    } else {
      ::unsetenv("ARBC_PLUGIN_PATH");
    }
  }

private:
  bool d_had = false;
  std::string d_prior;
};

// Construct the kind through a resolved factory and render one frame across the
// boundary, asserting the Timed facet and a settled frame -- the proof that the
// loaded-across-dlopen factory yields a working Content.
void require_constructs_and_renders(const ContentFactory& factory) {
  expected<std::unique_ptr<Content>, std::string> content = factory(ARBC_IMAGESEQ_FIXTURE_DIR);
  REQUIRE(content.has_value());
  REQUIRE((*content)->stability() == Stability::Timed);
  REQUIRE((*content)->time_extent().has_value());

  CpuBackend backend;
  auto target = backend.make_surface(2, 2, k_working_rgba32f);
  REQUIRE(target.has_value());
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{Rect{0.0, 0.0, 2.0, 2.0},
                              1.0,
                              Time{0},
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  const std::optional<RenderResult> result = (*content)->render(request, done);
  REQUIRE(result.has_value());
  REQUIRE(result->achieved_time.has_value());
  REQUIRE(result->provided.has_value());
}

} // namespace

// enforces: 03-layer-plugin-interface#plugin-registers-through-extern-c-entry
// enforces: 03-layer-plugin-interface#loader-errors-are-values
TEST_CASE("PluginHost loads org.arbc.imageseq end-to-end and reports failures as values") {
  PluginHost host;
  REQUIRE(host.registry().size() == 0);

  // Explicit by-path load brings the out-of-lib kind into the host's Registry.
  expected<std::monostate, PluginLoadError> loaded(std::monostate{});
  REQUIRE_NOTHROW(loaded = host.load_plugin(ARBC_IMAGESEQ_PLUGIN_FILE));
  REQUIRE(loaded.has_value());

  const ContentFactory* factory = host.registry().factory(k_kind_id);
  REQUIRE(factory != nullptr);
  const KindMetadata* meta = host.registry().metadata(k_kind_id);
  REQUIRE(meta != nullptr);
  REQUIRE_FALSE(meta->human_name.empty());
  require_constructs_and_renders(*factory);

  // Every loader failure is a VALUE, never a throw (doc 03:176-180):
  // a path that cannot be opened...
  expected<std::monostate, PluginLoadError> missing(std::monostate{});
  REQUIRE_NOTHROW(missing = host.load_plugin("/no/such/plugin/zzz.so"));
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error().code == PluginLoadError::Code::CannotOpen);
  REQUIRE_FALSE(missing.error().diagnostic.empty());

  // ...a real shared object that lacks the entry-point symbol (an explicit load
  // asserts "this IS a plugin", so a missing symbol is an error here)...
  expected<std::monostate, PluginLoadError> no_entry(std::monostate{});
  REQUIRE_NOTHROW(no_entry = host.load_plugin(ARBC_NOENTRY_PLUGIN_FILE));
  REQUIRE_FALSE(no_entry.has_value());
  REQUIRE(no_entry.error().code == PluginLoadError::Code::MissingEntryPoint);

  // ...a duplicate kind id, both as the bubbled RegistryError on a direct add...
  expected<std::monostate, RegistryError> dup(std::monostate{});
  REQUIRE_NOTHROW(dup = host.registry().add(k_kind_id, *factory));
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error() == RegistryError::DuplicateId);

  // ...and as the loader's DuplicateId when a re-load registers no new kind.
  expected<std::monostate, PluginLoadError> reload(std::monostate{});
  REQUIRE_NOTHROW(reload = host.load_plugin(ARBC_IMAGESEQ_PLUGIN_FILE));
  REQUIRE_FALSE(reload.has_value());
  REQUIRE(reload.error().code == PluginLoadError::Code::DuplicateId);

  // The original registration is untouched by every failed attempt.
  REQUIRE(host.registry().size() == 1);
  require_constructs_and_renders(*host.registry().factory(k_kind_id));
}

// enforces: 03-layer-plugin-interface#plugin-path-scan-is-opt-in
TEST_CASE("scan_plugin_path is opt-in and loads org.arbc.imageseq when ARBC_PLUGIN_PATH is set") {
  ScopedPluginPath guard;

  SECTION("unset: zero loads, zero registry growth, zero filesystem access") {
    ::unsetenv("ARBC_PLUGIN_PATH");
    PluginHost host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_plugin_path());
    REQUIRE(report.loaded == 0);
    REQUIRE(report.entries.empty());
    REQUIRE(host.registry().size() == 0);
    REQUIRE(host.registry().factory(k_kind_id) == nullptr);
  }

  SECTION("set: the scan loads the plugin and the kind becomes resolvable") {
    ::setenv("ARBC_PLUGIN_PATH", ARBC_IMAGESEQ_PLUGIN_DIR, 1);
    PluginHost host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_plugin_path());
    REQUIRE(report.loaded >= 1);

    const ContentFactory* factory = host.registry().factory(k_kind_id);
    REQUIRE(factory != nullptr);
    require_constructs_and_renders(*factory);

    bool loaded_imageseq = false;
    for (const PluginScanEntry& entry : report.entries) {
      if (entry.outcome == PluginScanEntry::Outcome::Loaded) {
        loaded_imageseq = true;
      }
    }
    REQUIRE(loaded_imageseq);
  }
}

// enforces: 03-layer-plugin-interface#explicit-host-registration-precedes-scan
TEST_CASE("explicit host registration precedes and beats the scan") {
  ScopedPluginPath guard;
  PluginHost host;

  // Register the kind explicitly FIRST.
  expected<std::monostate, PluginLoadError> loaded(std::monostate{});
  REQUIRE_NOTHROW(loaded = host.load_plugin(ARBC_IMAGESEQ_PLUGIN_FILE));
  REQUIRE(loaded.has_value());
  REQUIRE(host.registry().size() == 1);

  // Then scan the directory that also holds it: the collision is a per-entry
  // DuplicateId value; the earlier registration is left intact (explicit wins).
  ::setenv("ARBC_PLUGIN_PATH", ARBC_IMAGESEQ_PLUGIN_DIR, 1);
  PluginScanReport report;
  REQUIRE_NOTHROW(report = host.scan_plugin_path());

  bool saw_duplicate = false;
  bool reloaded_imageseq = false;
  for (const PluginScanEntry& entry : report.entries) {
    if (entry.outcome == PluginScanEntry::Outcome::DuplicateId) {
      saw_duplicate = true;
    }
    if (entry.outcome == PluginScanEntry::Outcome::Loaded) {
      reloaded_imageseq = true;
    }
  }
  REQUIRE(saw_duplicate);
  REQUIRE_FALSE(reloaded_imageseq);

  // The original factory is untouched and still constructs the kind.
  REQUIRE(host.registry().size() == 1);
  require_constructs_and_renders(*host.registry().factory(k_kind_id));
}
