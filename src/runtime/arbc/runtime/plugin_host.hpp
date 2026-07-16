#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp>     // expected, unexpected
#include <arbc/contract/registry.hpp> // Registry, RegistryError

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The production host-side plugin loader (doc 03:164-171, 188-207; doc 10:49-52;
// doc 17:60). It generalizes the hand-rolled `dlopen -> dlsym -> arbc_plugin_register
// -> factory -> render -> dlclose` path of tests/imageseq_plugin_path.t.cpp into a
// reusable, value-erroring API an embedder or an application host drives to bring
// out-of-lib content kinds (starting with `org.arbc.imageseq`) into a `Registry` the
// document-load path resolves against.
//
// Two audiences, one object (doc 10:49-52): an embedder drives `registry()` +
// `load_plugin(path)` explicitly ("embedders usually want control"); an
// application host additionally calls `scan_plugin_path()` for the opt-in
// `ARBC_PLUGIN_PATH` directory sweep. The ordering contract -- "explicit host
// registration first" -- is realized on the `Registry`'s existing `DuplicateId`
// return: the host registers explicitly BEFORE scanning, and a scanned kind that
// collides with an already-registered id is a per-entry value, not a silent
// override (explicit registration wins).
//
// Errors are values across the boundary (doc 03:176-180, doc 10:16-18): every
// entry point returns through `arbc::expected` or a report value; a missing file,
// an un-`dlopen`-able library, a library without the entry point, and a
// `DuplicateId` are all values. `dlerror()` is captured into a diagnostic string,
// never propagated as state. The loader never throws and never aborts.
//
// Levelization (doc 17:60): the loader is `arbc::runtime` (L5) and depends only on
// the L3 `contract::Registry` it already lists. `dlopen`/`libdl` is a platform
// facility, invisible to `check_levels.py`; this public header names no
// `nlohmann::json` type. The built-in-kind -> `Registry` bootstrap is the L6
// umbrella `arbc` target's concern (doc 17:61), NOT the loader's: the loader
// operates on whatever `Registry` the host hands it, agnostic to its contents.

namespace arbc {

// Why an explicit `load_plugin(path)` failed, or why a scanned entry could not
// register. A value type carrying the `dlerror()`-style diagnostic so a host can
// surface WHY a load failed without parsing. `DuplicateId` is the loader's view of
// the `Registry`'s own `RegistryError::DuplicateId`, reconstructed at the boundary:
// the `extern "C"` entry point returns `void`, so the loader observes a collision
// by the registry gaining no new kind rather than by a bubbled return value.
struct PluginLoadError {
  enum class Code {
    CannotOpen,        // dlopen failed: missing file, not a shared object, unresolved symbol
    MissingEntryPoint, // opened, but exposes no `arbc_plugin_register` symbol
    DuplicateId,       // the entry point registered no new kind (its kinds are already present)
  };

  Code code;
  std::string path;       // the path the loader tried to open
  std::string diagnostic; // the captured dlerror()-style message (may be empty)
};

// The outcome of one directory entry visited during a scan. A scan is lenient
// where an explicit load is strict (Decision 3): a shared object that opens but
// lacks the entry point is a `SkippedNoEntry` (a plugin directory may hold support
// libraries), not an error.
struct PluginScanEntry {
  enum class Outcome {
    Loaded,         // registered at least one new kind
    SkippedNoEntry, // opened, but no `arbc_plugin_register` -- a support library
    CannotOpen,     // dlopen failed
    DuplicateId,    // opened and ran, but registered no new kind (already present)
  };

  std::string path;
  Outcome outcome;
  std::string diagnostic; // captured dlerror() for CannotOpen, else empty
};

// What a `scan_plugin_path()` did, so an application host can log it: the count of
// entries that registered a new kind, plus a per-entry outcome list in deterministic
// (lexicographic) load order.
struct PluginScanReport {
  std::size_t loaded = 0;
  std::vector<PluginScanEntry> entries;
};

namespace detail {

// RAII wrapper over a `dlopen` handle: `dlclose` on destruction. Move-only so it
// can live in the host's handle vector; the destructor is out-of-line so this
// public header pulls in no `<dlfcn.h>`.
class ARBC_API PluginHandle {
public:
  explicit PluginHandle(void* handle) noexcept : d_handle(handle) {}
  ~PluginHandle();

  PluginHandle(PluginHandle&& other) noexcept : d_handle(other.d_handle) {
    other.d_handle = nullptr;
  }
  PluginHandle& operator=(PluginHandle&&) = delete;
  PluginHandle(const PluginHandle&) = delete;
  PluginHandle& operator=(const PluginHandle&) = delete;

  void* get() const noexcept { return d_handle; }

private:
  void* d_handle;
};

} // namespace detail

// Owns a `Registry` plus the `dlopen` handles every plugin-derived factory was
// minted from. Member declaration order is the destroy-order contract (Constraint
// 4): `d_handles` is declared BEFORE `d_registry`, so the registry (and all
// factories, and any `Content` still holding a factory-captured pointer into a
// plugin image) is destroyed FIRST, and the handles are `dlclose`d AFTER -- never
// unmapping code that is still live. This is the exact ordering
// tests/imageseq_plugin_path.t.cpp:79-81 documents by hand, made a property of the
// type rather than a burden on every embedder.
class ARBC_API PluginHost {
public:
  PluginHost() = default;

  PluginHost(const PluginHost&) = delete;
  PluginHost& operator=(const PluginHost&) = delete;
  PluginHost(PluginHost&&) = delete;
  PluginHost& operator=(PluginHost&&) = delete;

  Registry& registry() noexcept { return d_registry; }
  const Registry& registry() const noexcept { return d_registry; }

  // Explicit, by-path load. `dlopen(path, RTLD_NOW | RTLD_LOCAL)`, resolve
  // `arbc_plugin_register`, invoke it against `registry()`. The handle is owned for
  // the host's session before the entry point runs, so any factory it mints
  // outlives no `dlclose`. Returns success, or a value:
  //   - `CannotOpen`        -- the path could not be `dlopen`ed;
  //   - `MissingEntryPoint` -- opened, but no entry-point symbol (an explicit load
  //                            asserts "this IS a plugin", so a missing symbol is a
  //                            real error here, unlike during a scan);
  //   - `DuplicateId`       -- the entry point registered no new kind because its
  //                            kinds are already present (explicit registration
  //                            wins). Never throws, never aborts.
  expected<std::monostate, PluginLoadError> load_plugin(std::string_view path);

  // The opt-in `ARBC_PLUGIN_PATH` directory scan. Reads the environment variable
  // (platform-path-separator-delimited directory list). If it is unset or empty the
  // scan does NOTHING and touches the filesystem ZERO times (Constraint 2). If set,
  // each directory's shared-library entries are loaded in lexicographic order
  // (Constraint 5, deterministic). A library that opens but lacks the entry point is
  // SKIPPED, not an error (Decision 3); a `DuplicateId` collision is a per-entry
  // value that leaves the earlier registration intact (Decision 2). Never throws.
  PluginScanReport scan_plugin_path();

private:
  std::vector<detail::PluginHandle> d_handles; // destroyed AFTER d_registry (dlclose last)
  Registry d_registry;                         // destroyed FIRST (factories released first)
};

} // namespace arbc
