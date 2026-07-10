#include <arbc/runtime/plugin_host.hpp>

// Platform dynamic-loader backing. POSIX `dlfcn`/`dirent` was the v1 implementation;
// the Windows `LoadLibrary`/`GetProcAddress`/`FreeLibrary` + `FindFirstFile` backing
// (`runtime.plugin_loading_win32`, M9) lives behind this single `_WIN32` seam --
// mirroring the guard in plugin.hpp:8-12. Only the platform leaf calls differ; the
// orchestration (load_plugin, scan, error mapping, the shared std::sort) is common,
// so "mirrors POSIX dlfcn behavior" is a structural guarantee (Decision 1).
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace arbc {

namespace detail {

// Unmap the handle when the host tears down -- AFTER the registry and every
// factory it minted are gone (PluginHost member order), so the code backing them
// is never unmapped while live (Constraint 4/6). The wrapper stores the HMODULE as
// void*, so the lifetime contract is identical on both platforms.
PluginHandle::~PluginHandle() {
  if (d_handle != nullptr) {
#if defined(_WIN32)
    ::FreeLibrary(static_cast<HMODULE>(d_handle));
#else
    ::dlclose(d_handle);
#endif
  }
}

} // namespace detail

namespace {

// The `extern "C"` plugin entry point (plugin.hpp:20).
constexpr const char* k_entry_point = "arbc_plugin_register";

// Platform facts (Constraint 7): the shared-library filename suffix a scan matches,
// and the `ARBC_PLUGIN_PATH` directory-list separator.
#if defined(_WIN32)
constexpr std::string_view k_shared_lib_suffix = ".dll";
constexpr char k_path_separator = ';';
#else
constexpr std::string_view k_shared_lib_suffix = ".so";
constexpr char k_path_separator = ':';
#endif

using RegisterFn = void (*)(Registry&);

#if defined(_WIN32)
// The FormatMessage analog of dlerror(): render the last Win32 error as a captured
// diagnostic string (Decision 3). Errors are values on Windows too -- never thrown
// (Constraint 2, doc 03:176-180).
std::string last_error_message() {
  const DWORD code = ::GetLastError();
  if (code == 0) {
    return "";
  }
  LPSTR buffer = nullptr;
  const DWORD len = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     // NOLINTNEXTLINE(*-reinterpret-cast): FormatMessage's
                                     // FORMAT_MESSAGE_ALLOCATE_BUFFER out-pointer ABI.
                                     reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
  std::string message(buffer != nullptr ? buffer : "", buffer != nullptr ? len : 0);
  if (buffer != nullptr) {
    ::LocalFree(buffer);
  }
  return message;
}
#endif

bool has_suffix(std::string_view name, std::string_view suffix) {
  return name.size() >= suffix.size() &&
         name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The shared open+resolve step. Returns the resolved entry point (and, on the
// caller's side, ownership of the handle in `*out_handle`) or leaves `*out_fn`
// null with `*out_handle` cleaned up. `symbol_missing` distinguishes "opened but
// no entry point" (a scan skips it, an explicit load faults it) from "could not
// open at all". `diagnostic` captures the dlerror()-style message.
struct OpenResult {
  void* handle = nullptr;  // owned by the caller iff fn != nullptr
  RegisterFn fn = nullptr; // the resolved entry point, or nullptr
  bool opened = false;     // dlopen succeeded (even if the symbol was absent)
  std::string diagnostic;  // captured dlerror() on failure
};

OpenResult open_and_resolve(const std::string& path) {
  OpenResult result;
#if defined(_WIN32)
  // Default LoadLibraryA is the faithful analog of dlopen(RTLD_NOW | RTLD_LOCAL):
  // Windows resolves imports at load and has no global symbol namespace, and the
  // loader always hands a resolved path (Decision 3).
  ::SetLastError(0); // clear any stale error state
  HMODULE handle = ::LoadLibraryA(path.c_str());
  if (handle == nullptr) {
    result.diagnostic = last_error_message();
    return result; // opened == false, CannotOpen
  }
  result.opened = true;
  ::SetLastError(0);
  FARPROC symbol = ::GetProcAddress(handle, k_entry_point);
  if (symbol == nullptr) {
    result.diagnostic = last_error_message();
    ::FreeLibrary(handle); // nothing registered from this image; unmap it now
    return result;         // opened == true, fn == nullptr -> MissingEntryPoint / skip
  }
  result.handle = handle;
  // NOLINTNEXTLINE(*-reinterpret-cast): the extern "C" entry-point ABI.
  result.fn = reinterpret_cast<RegisterFn>(symbol);
  return result;
#else
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
#endif
}

} // namespace

expected<std::monostate, PluginLoadError> PluginHost::load_plugin(std::string_view path) {
  const std::string path_str(path); // dlopen needs a NUL-terminated C string
  OpenResult opened = open_and_resolve(path_str);

  if (!opened.opened) {
    return unexpected(
        PluginLoadError{PluginLoadError::Code::CannotOpen, path_str, std::move(opened.diagnostic)});
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

  // MSVC deprecates getenv in favour of _dupenv_s; suppress the warning since
  // getenv is both correct and portable for a read-only environment lookup.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* env = std::getenv("ARBC_PLUGIN_PATH");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
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
    // directories are silently ignored -- a stale `ARBC_PLUGIN_PATH` entry is not a
    // loader error. The candidate set is filtered by the shared `has_suffix`, so the
    // library suffix is single-sourced across platforms.
    std::vector<std::string> candidates;
#if defined(_WIN32)
    // FindFirstFile over `dir\*`; INVALID_HANDLE_VALUE is the `opendir == nullptr`
    // silent skip. Its enumeration order is unspecified -- the shared std::sort below
    // is what makes the scan deterministic (Constraint 4/5).
    std::string pattern = dir;
    if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/') {
      pattern.push_back('\\');
    }
    pattern.push_back('*');
    WIN32_FIND_DATAA find_data;
    HANDLE handle = ::FindFirstFileA(pattern.c_str(), &find_data);
    if (handle == INVALID_HANDLE_VALUE) {
      continue;
    }
    do {
      const std::string_view name(find_data.cFileName);
      if (has_suffix(name, k_shared_lib_suffix)) {
        std::string full = dir;
        if (!full.empty() && full.back() != '\\' && full.back() != '/') {
          full.push_back('\\');
        }
        full.append(name);
        candidates.push_back(std::move(full));
      }
    } while (::FindNextFileA(handle, &find_data) != 0);
    ::FindClose(handle);
#else
    DIR* handle = ::opendir(dir.c_str());
    if (handle == nullptr) {
      continue;
    }
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
#endif

    // Deterministic load order (Constraint 5): sort lexicographically so the
    // registration sequence -- and thus any DuplicateId outcome -- is reproducible.
    std::sort(candidates.begin(), candidates.end());

    for (const std::string& candidate : candidates) {
      OpenResult opened = open_and_resolve(candidate);
      if (!opened.opened) {
        report.entries.push_back(
            {candidate, PluginScanEntry::Outcome::CannotOpen, std::move(opened.diagnostic)});
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
