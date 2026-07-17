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
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>

using namespace arbc;

namespace {

constexpr const char* k_kind_id = "org.arbc.imageseq";

// The platform `ARBC_PLUGIN_PATH` directory-list separator (`;` on Windows, `:` on
// POSIX) -- the test-side mirror of the loader's `k_path_separator`.
#if defined(_WIN32)
constexpr char k_path_list_separator = ';';
#else
constexpr char k_path_list_separator = ':';
#endif

// The env var naming the per-user data dir the default-directory resolver reads
// first (runtime.plugin_default_search_paths Decision 2) -- the lane this suite
// stages fixture plugins into.
#if defined(_WIN32)
constexpr const char* k_user_data_env = "LOCALAPPDATA";
#else
constexpr const char* k_user_data_env = "XDG_DATA_HOME";
#endif

// Portable ARBC_PLUGIN_PATH mutation: `_putenv_s` on Windows, `setenv`/`unsetenv` on
// POSIX, so this suite compiles and runs on the msvc-debug lane. `_putenv_s(name, "")`
// removes the variable, matching `unsetenv` semantics.
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
// helpers above generalized to the default-directory env inputs.
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

// A per-case staging root under the system temp dir: recreated on entry, removed
// on exit, so a crashed prior run cannot leak staged plugins into this one. Tags
// are unique per TEST_CASE, and catch_discover_tests runs each case as its own
// ctest entry, so parallel cases never share a root.
class ScopedTempDir {
public:
  explicit ScopedTempDir(const char* tag)
      : d_path(std::filesystem::temp_directory_path() /
               (std::string("arbc_plugin_default_scan_") + tag)) {
    std::filesystem::remove_all(d_path);
    std::filesystem::create_directories(d_path);
  }
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(d_path, ec); // best-effort cleanup
  }
  ScopedTempDir(const ScopedTempDir&) = delete;
  ScopedTempDir& operator=(const ScopedTempDir&) = delete;

  const std::filesystem::path& path() const { return d_path; }

private:
  std::filesystem::path d_path;
};

// Copy the built imageseq plugin into `dir` (created as needed), returning the
// staged module path. The copy keeps the original's absolute build-tree RPATH, so
// it loads from any staged location.
std::filesystem::path stage_imageseq_copy(const std::filesystem::path& dir) {
  const std::filesystem::path source(ARBC_IMAGESEQ_PLUGIN_FILE);
  std::filesystem::create_directories(dir);
  const std::filesystem::path staged = dir / source.filename();
  std::filesystem::copy_file(source, staged, std::filesystem::copy_options::overwrite_existing);
  return staged;
}

// The report outcome for one exact staged path, or nullopt when the path produced
// no entry -- so assertions stay per-directory-contained (Constraint 9: a
// developer machine's REAL default dirs may add entries of their own; those never
// carry our staged paths).
std::optional<PluginScanEntry::Outcome> outcome_of(const PluginScanReport& report,
                                                   const std::filesystem::path& path) {
  for (const PluginScanEntry& entry : report.entries) {
    if (entry.path == path.string()) {
      return entry.outcome;
    }
  }
  return std::nullopt;
}

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
  const RenderRequest request{
      Rect{0.0, 0.0, 2.0, 2.0}, 1.0, Time{0}, StateHandle{}, **target, Exactness::Exact,
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
    unset_plugin_path();
    PluginHost host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_plugin_path());
    REQUIRE(report.loaded == 0);
    REQUIRE(report.entries.empty());
    REQUIRE(host.registry().size() == 0);
    REQUIRE(host.registry().factory(k_kind_id) == nullptr);
  }

  SECTION("set: the scan loads the plugin and the kind becomes resolvable") {
    set_plugin_path(ARBC_IMAGESEQ_PLUGIN_DIR);
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

// enforces: 03-layer-plugin-interface#plugin-path-scan-is-opt-in
TEST_CASE("scan_plugin_path splits ARBC_PLUGIN_PATH on the platform separator") {
  ScopedPluginPath guard;

  // A multi-directory value: a bogus leading entry, then the real plugin directory,
  // joined by the platform separator (';' on Windows, ':' on POSIX). The split must
  // recover the real directory for the kind to load -- the identical outcome on both
  // platforms proves the shared split honors k_path_separator (Constraint 5). The
  // bogus entry also exercises the missing-directory silent skip within a scan.
  const std::string multi =
      std::string("/no/such/plugin/dir/zzz") + k_path_list_separator + ARBC_IMAGESEQ_PLUGIN_DIR;
  set_plugin_path(multi.c_str());

  PluginHost host;
  PluginScanReport report;
  REQUIRE_NOTHROW(report = host.scan_plugin_path());
  REQUIRE(report.loaded >= 1);

  const ContentFactory* factory = host.registry().factory(k_kind_id);
  REQUIRE(factory != nullptr);
  require_constructs_and_renders(*factory);
}

// enforces: 03-layer-plugin-interface#standard-scan-orders-env-before-defaults-and-dedups
TEST_CASE("scan_standard_paths walks env dirs before defaults and dedups directories") {
  ScopedPluginPath path_guard;
  ScopedEnvVar user_data_guard(k_user_data_env);
#if !defined(_WIN32)
  ScopedEnvVar data_dirs_guard("XDG_DATA_DIRS");
#endif
  ScopedTempDir stage_root("combined");
  const std::filesystem::path user_base = stage_root.path() / "user-data";
  const std::filesystem::path user_plugins = user_base / "arbc" / "plugins";
#if !defined(_WIN32)
  // Neutralize the system data dirs: a machine's real /usr/local/share/arbc/plugins
  // must not leak entries into this report (hermeticity, Constraint 9).
  set_env_var("XDG_DATA_DIRS", (stage_root.path() / "no-such-data-dir").string().c_str());
#endif
  set_env_var(k_user_data_env, user_base.string().c_str());

  SECTION("env unset: a fixture plugin in the per-user default dir is discovered") {
    unset_plugin_path();
    const std::filesystem::path staged = stage_imageseq_copy(user_plugins);

    PluginHost host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_standard_paths());

    REQUIRE(outcome_of(report, staged) == PluginScanEntry::Outcome::Loaded);
    const ContentFactory* factory = host.registry().factory(k_kind_id);
    REQUIRE(factory != nullptr);
    require_constructs_and_renders(*factory);
  }

  SECTION("a dir listed in both ARBC_PLUGIN_PATH and the defaults is scanned once") {
    const std::filesystem::path staged = stage_imageseq_copy(user_plugins);
    // The env-listed dir IS the per-user default dir: the combined scan must
    // visit it exactly once -- one report entry for the staged module, Loaded,
    // with no self-inflicted DuplicateId from a second visit.
    set_plugin_path(user_plugins.string().c_str());

    PluginHost host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_standard_paths());

    std::size_t entries_for_staged = 0;
    for (const PluginScanEntry& entry : report.entries) {
      if (entry.path == staged.string()) {
        ++entries_for_staged;
        REQUIRE(entry.outcome == PluginScanEntry::Outcome::Loaded);
      }
    }
    REQUIRE(entries_for_staged == 1);
    REQUIRE(host.registry().factory(k_kind_id) != nullptr);
  }

  SECTION("an env-dir kind survives the same kind in a default dir as DuplicateId") {
    const std::filesystem::path env_dir = stage_root.path() / "env-plugins";
    const std::filesystem::path env_copy = stage_imageseq_copy(env_dir);
    const std::filesystem::path default_copy = stage_imageseq_copy(user_plugins);
    set_plugin_path(env_dir.string().c_str());

    PluginHost host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = host.scan_standard_paths());

    // Env dirs walk FIRST (Constraint 2), so the env copy registers the kind and
    // the default-dir copy is the additive per-entry DuplicateId (Constraint 4).
    REQUIRE(outcome_of(report, env_copy) == PluginScanEntry::Outcome::Loaded);
    REQUIRE(outcome_of(report, default_copy) == PluginScanEntry::Outcome::DuplicateId);

    // The env-dir registration is intact and functional.
    const ContentFactory* factory = host.registry().factory(k_kind_id);
    REQUIRE(factory != nullptr);
    require_constructs_and_renders(*factory);
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
  set_plugin_path(ARBC_IMAGESEQ_PLUGIN_DIR);
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

  // The same precedence holds against a DEFAULT-dir plugin through the combined
  // scan (runtime.plugin_default_search_paths): stage a copy into a temp per-user
  // data dir -- the explicitly-registered kind stays, the staged copy is a
  // per-entry DuplicateId. No registry-size assertion after this scan: a machine's
  // real install libdir may legitimately add kinds of its own (Constraint 9).
  ScopedEnvVar user_data_guard(k_user_data_env);
#if !defined(_WIN32)
  ScopedEnvVar data_dirs_guard("XDG_DATA_DIRS");
#endif
  ScopedTempDir stage_root("precedence");
#if !defined(_WIN32)
  set_env_var("XDG_DATA_DIRS", (stage_root.path() / "no-such-data-dir").string().c_str());
#endif
  const std::filesystem::path user_base = stage_root.path() / "user-data";
  const std::filesystem::path staged = stage_imageseq_copy(user_base / "arbc" / "plugins");
  set_env_var(k_user_data_env, user_base.string().c_str());
  unset_plugin_path();

  PluginScanReport combined;
  REQUIRE_NOTHROW(combined = host.scan_standard_paths());
  REQUIRE(outcome_of(combined, staged) == PluginScanEntry::Outcome::DuplicateId);
  require_constructs_and_renders(*host.registry().factory(k_kind_id));
}
