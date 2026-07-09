#include <arbc/runtime/plugin_host.hpp>

#if defined(_WIN32)
// POSIX `dlfcn` is the v1 backing (Decision 4, Constraint 7). The Windows
// `LoadLibrary`/`GetProcAddress`/`FreeLibrary` backing -- and the `;`
// `ARBC_PLUGIN_PATH` separator -- are the deferred `runtime.plugin_loading_win32`
// leaf (M9). This single `_WIN32` seam mirrors the guard in plugin.hpp:8-12.
#error "runtime.plugin_loading is POSIX/dlfcn only for v1; the Windows backing is runtime.plugin_loading_win32 (M9). See tasks/refinements/runtime/plugin_loading.md Decision 4."
#endif

#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <dirent.h>

namespace arbc {

namespace detail {

// dlclose the handle when the host tears down -- AFTER the registry and every
// factory it minted are gone (PluginHost member order), so the code backing them
// is never unmapped while live (Constraint 4).
PluginHandle::~PluginHandle() {
  if (d_handle != nullptr) {
    ::dlclose(d_handle);
  }
}

} // namespace detail

namespace {

// The `extern "C"` plugin entry point (plugin.hpp:20).
constexpr const char* k_entry_point = "arbc_plugin_register";

// POSIX facts (Constraint 7): the shared-library filename suffix a scan matches,
// and the `ARBC_PLUGIN_PATH` directory-list separator. The Windows values live
// behind the `_WIN32` seam above (deferred).
constexpr std::string_view k_shared_lib_suffix = ".so";
constexpr char k_path_separator = ':';

using RegisterFn = void (*)(Registry&);

bool has_suffix(std::string_view name, std::string_view suffix) {
  return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The shared open+resolve step. Returns the resolved entry point (and, on the
// caller's side, ownership of the handle in `*out_handle`) or leaves `*out_fn`
// null with `*out_handle` cleaned up. `symbol_missing` distinguishes "opened but
// no entry point" (a scan skips it, an explicit load faults it) from "could not
// open at all". `diagnostic` captures the dlerror()-style message.
struct OpenResult {
  void* handle = nullptr;    // owned by the caller iff fn != nullptr
  RegisterFn fn = nullptr;   // the resolved entry point, or nullptr
  bool opened = false;       // dlopen succeeded (even if the symbol was absent)
  std::string diagnostic;    // captured dlerror() on failure
};

OpenResult open_and_resolve(const std::string& path) {
  OpenResult result;
  ::dlerror(); // clear any stale error state
  void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    const char* err = ::dlerror();
    result.diagnostic = err != nullptr ? err : "";
    return result; // opened == false, CannotOpen
  }
  result.opened = true;
  ::dlerror();
  void* symbol = ::dlsym(handle, k_entry_point);
  if (symbol == nullptr) {
    const char* err = ::dlerror();
    result.diagnostic = err != nullptr ? err : "";
    ::dlclose(handle); // nothing registered from this image; unmap it now
    return result;     // opened == true, fn == nullptr -> MissingEntryPoint / skip
  }
  result.handle = handle;
  // NOLINTNEXTLINE(*-reinterpret-cast): the extern "C" entry-point ABI.
  result.fn = reinterpret_cast<RegisterFn>(symbol);
  return result;
}

} // namespace

expected<std::monostate, PluginLoadError> PluginHost::load_plugin(std::string_view path) {
  const std::string path_str(path); // dlopen needs a NUL-terminated C string
  OpenResult opened = open_and_resolve(path_str);

  if (!opened.opened) {
    return unexpected(PluginLoadError{PluginLoadError::Code::CannotOpen, path_str,
                                      std::move(opened.diagnostic)});
  }
  if (opened.fn == nullptr) {
    // An explicit by-path load asserts "this IS a plugin", so a missing symbol is
    // a real error (Decision 3).
    return unexpected(PluginLoadError{PluginLoadError::Code::MissingEntryPoint, path_str,
                                      std::move(opened.diagnostic)});
  }

  // Own the handle for the session BEFORE running the entry point, so any factory
  // it mints (and any partial registration) is backed by a mapped image for the
  // host's lifetime (Constraint 4).
  d_handles.emplace_back(opened.handle);
  const std::size_t before = d_registry.size();
  opened.fn(d_registry);

  if (d_registry.size() == before) {
    // The entry point ran but the registry gained no kind: every kind it offers is
    // already registered. The `void` boundary cannot bubble the `Registry`'s own
    // `DuplicateId`, so the loader reports it from the observed registry state --
    // explicit registration wins (doc 10:49-52).
    return unexpected(PluginLoadError{PluginLoadError::Code::DuplicateId, path_str, ""});
  }
  return std::monostate{};
}

PluginScanReport PluginHost::scan_plugin_path() {
  PluginScanReport report;

  const char* env = std::getenv("ARBC_PLUGIN_PATH");
  if (env == nullptr || env[0] == '\0') {
    return report; // genuinely opt-in: unset/empty -> zero loads, zero fs access
  }

  // Split the directory list on the platform separator.
  std::vector<std::string> directories;
  std::string_view spec(env);
  std::size_t start = 0;
  while (start <= spec.size()) {
    const std::size_t sep = spec.find(k_path_separator, start);
    const std::size_t end = sep == std::string_view::npos ? spec.size() : sep;
    if (end > start) {
      directories.emplace_back(spec.substr(start, end - start));
    }
    if (sep == std::string_view::npos) {
      break;
    }
    start = sep + 1;
  }

  for (const std::string& dir : directories) {
    // Enumerate the directory's shared-library entries. Missing/unreadable
    // directories are silently ignored (opendir returns null) -- a stale
    // `ARBC_PLUGIN_PATH` entry is not a loader error.
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
      continue;
    }
    std::vector<std::string> candidates;
    for (const dirent* entry = ::readdir(handle); entry != nullptr; entry = ::readdir(handle)) {
      const std::string_view name(entry->d_name);
      if (has_suffix(name, k_shared_lib_suffix)) {
        std::string full = dir;
        if (!full.empty() && full.back() != '/') {
          full.push_back('/');
        }
        full.append(name);
        candidates.push_back(std::move(full));
      }
    }
    ::closedir(handle);

    // Deterministic load order (Constraint 5): sort lexicographically so the
    // registration sequence -- and thus any DuplicateId outcome -- is reproducible.
    std::sort(candidates.begin(), candidates.end());

    for (const std::string& candidate : candidates) {
      OpenResult opened = open_and_resolve(candidate);
      if (!opened.opened) {
        report.entries.push_back({candidate, PluginScanEntry::Outcome::CannotOpen,
                                  std::move(opened.diagnostic)});
        continue;
      }
      if (opened.fn == nullptr) {
        // A support library in the plugin directory: skip, do not fail the scan
        // (Decision 3).
        report.entries.push_back({candidate, PluginScanEntry::Outcome::SkippedNoEntry, ""});
        continue;
      }

      d_handles.emplace_back(opened.handle);
      const std::size_t before = d_registry.size();
      opened.fn(d_registry);
      if (d_registry.size() == before) {
        // Registered nothing new -> a DuplicateId collision; the earlier
        // registration is left intact (Decision 2, explicit registration wins).
        report.entries.push_back({candidate, PluginScanEntry::Outcome::DuplicateId, ""});
      } else {
        report.entries.push_back({candidate, PluginScanEntry::Outcome::Loaded, ""});
        ++report.loaded;
      }
    }
  }

  return report;
}

} // namespace arbc
