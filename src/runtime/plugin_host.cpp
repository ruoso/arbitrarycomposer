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

// The path-join separator for composing default-directory paths -- a platform
// leaf under the single `_WIN32` seam (plugin_loading_win32 Decision 1).
#if defined(_WIN32)
constexpr char k_dir_separator = '\\';
#else
constexpr char k_dir_separator = '/';
#endif

bool is_dir_separator(char c) {
#if defined(_WIN32)
  return c == '\\' || c == '/';
#else
  return c == '/';
#endif
}

// Read-only environment lookup (Decision 5: no shell APIs, no platform-paths
// library). MSVC deprecates getenv in favour of _dupenv_s; suppress the warning
// since getenv is both correct and portable here.
const char* read_env(const char* name) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  return std::getenv(name);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
}

// Split a separator-delimited directory list, dropping empty segments -- shared
// by the ARBC_PLUGIN_PATH read and the $XDG_DATA_DIRS read.
std::vector<std::string> split_directory_list(std::string_view spec, char separator) {
  std::vector<std::string> directories;
  std::size_t start = 0;
  while (start <= spec.size()) {
    const std::size_t sep = spec.find(separator, start);
    const std::size_t end = sep == std::string_view::npos ? spec.size() : sep;
    if (end > start) {
      directories.emplace_back(spec.substr(start, end - start));
    }
    if (sep == std::string_view::npos) {
      break;
    }
    start = sep + 1;
  }
  return directories;
}

// Trailing-separator trim: the combined scan's directory dedup key (Decision 4 --
// string equality, no canonicalization; aliased paths fall through to the
// registry's DuplicateId guard) and the normalization the default-dir join
// applies before appending the plugin subdir.
std::string trim_trailing_separators(std::string_view dir) {
  std::size_t end = dir.size();
  while (end > 0 && is_dir_separator(dir[end - 1])) {
    --end;
  }
  return std::string(dir.substr(0, end));
}

// `<base>/arbc/plugins` with the platform join separator: the shipped install
// layout's plugin subdir (packaging/install.md D6) under a conventional data dir.
std::string plugins_subdir_under(std::string_view base) {
  std::string dir = trim_trailing_separators(base);
  dir += k_dir_separator;
  dir += "arbc";
  dir += k_dir_separator;
  dir += "plugins";
  return dir;
}

// The directory holding the binary image this resolver is compiled into --
// libarbc in a shared build, the host executable in a static one (Decision 3).
// `dladdr` on an in-image anchor / `GetModuleHandleExA(FROM_ADDRESS)` +
// `GetModuleFileNameA`; both read the loader's in-memory module table, no
// filesystem access. Returns empty when unresolvable (the caller skips the
// image-relative entry; the configure-time libdir backstops it).
std::string image_directory() {
  std::string path;
#if defined(_WIN32)
  HMODULE module = nullptr;
  if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           // NOLINTNEXTLINE(*-reinterpret-cast): the FROM_ADDRESS lookup ABI.
                           reinterpret_cast<LPCSTR>(&image_directory), &module) == 0) {
    return "";
  }
  path.resize(MAX_PATH, '\0');
  for (;;) {
    const DWORD len = ::GetModuleFileNameA(module, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0) {
      return "";
    }
    if (len < path.size()) {
      path.resize(len);
      break;
    }
    path.resize(path.size() * 2, '\0'); // truncated: grow and retry
  }
#else
  Dl_info info{};
  // NOLINTNEXTLINE(*-reinterpret-cast): dladdr takes the anchor as an object pointer.
  if (::dladdr(reinterpret_cast<void*>(&image_directory), &info) == 0 ||
      info.dli_fname == nullptr) {
    return "";
  }
  path = info.dli_fname;
#endif
  std::size_t last = path.size();
  while (last > 0 && !is_dir_separator(path[last - 1])) {
    --last;
  }
  if (last == 0) {
    return ""; // no directory component to anchor on
  }
  return trim_trailing_separators(std::string_view(path).substr(0, last));
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

std::vector<std::string> default_plugin_directories() {
  std::vector<std::string> directories;

#if defined(_WIN32)
  // Per-user data dir: %LOCALAPPDATA% -- machine-specific binaries must not roam,
  // so not %APPDATA% (Decision 2). Unset/empty -> skip (Constraint 5).
  const char* local_app_data = read_env("LOCALAPPDATA");
  if (local_app_data != nullptr && local_app_data[0] != '\0') {
    directories.push_back(plugins_subdir_under(local_app_data));
  }
#else
  // Per-user data dir: $XDG_DATA_HOME, XDG Base Directory fallback
  // $HOME/.local/share; skipped when neither resolves (Constraint 5). First in
  // the list, so a per-user plugin shadows a system/shipped one with the same
  // kind id under first-registration-wins -- the conventional XDG semantics.
  const char* xdg_data_home = read_env("XDG_DATA_HOME");
  if (xdg_data_home != nullptr && xdg_data_home[0] != '\0') {
    directories.push_back(plugins_subdir_under(xdg_data_home));
  } else {
    const char* home = read_env("HOME");
    if (home != nullptr && home[0] != '\0') {
      directories.push_back(plugins_subdir_under(trim_trailing_separators(home) + "/.local/share"));
    }
  }

  // System data dirs: each $XDG_DATA_DIRS entry in listed order, spec fallback
  // /usr/local/share:/usr/share. The XDG list separator is ':' by spec.
  const char* xdg_data_dirs = read_env("XDG_DATA_DIRS");
  const std::string_view data_dirs = xdg_data_dirs != nullptr && xdg_data_dirs[0] != '\0'
                                         ? std::string_view(xdg_data_dirs)
                                         : std::string_view("/usr/local/share:/usr/share");
  for (const std::string& dir : split_directory_list(data_dirs, ':')) {
    directories.push_back(plugins_subdir_under(dir));
  }
#endif

  // Install-relative libdir, image-relative first: in a shared install the arbc
  // image sits AT `<prefix>/<libdir>`, sibling to the `arbc/plugins` subdir, so
  // this entry survives relocation (staged `cmake --install --prefix` trees,
  // relocatable packages). The configure-time libdir follows as the static-build
  // backstop, where the "image" is the host executable and the image-relative dir
  // is usually meaningless (Decision 3); in a non-relocated install the two
  // coincide and the combined scan's dedup collapses them (Constraint 3).
  const std::string image_dir = image_directory();
  if (!image_dir.empty()) {
    directories.push_back(plugins_subdir_under(image_dir));
  }
  directories.push_back(plugins_subdir_under(ARBC_INSTALL_LIBDIR));

  return directories;
}

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

// The per-directory enumeration + load step both scans drive (the factored-out
// helper of runtime.plugin_default_search_paths Decision 1 -- scan_plugin_path's
// original per-directory body, semantics unchanged).
void PluginHost::scan_directory(const std::string& directory, PluginScanReport& report) {
  // Enumerate the directory's shared-library entries. Missing/unreadable
  // directories are silently ignored -- a stale `ARBC_PLUGIN_PATH` entry or an
  // absent default directory is not a loader error. The candidate set is filtered
  // by the shared `has_suffix`, so the library suffix is single-sourced across
  // platforms.
  std::vector<std::string> candidates;
#if defined(_WIN32)
  // FindFirstFile over `dir\*`; INVALID_HANDLE_VALUE is the `opendir == nullptr`
  // silent skip. Its enumeration order is unspecified -- the shared std::sort below
  // is what makes the scan deterministic (Constraint 4/5).
  std::string pattern = directory;
  if (!pattern.empty() && pattern.back() != '\\' && pattern.back() != '/') {
    pattern.push_back('\\');
  }
  pattern.push_back('*');
  WIN32_FIND_DATAA find_data;
  HANDLE handle = ::FindFirstFileA(pattern.c_str(), &find_data);
  if (handle == INVALID_HANDLE_VALUE) {
    return;
  }
  do {
    const std::string_view name(find_data.cFileName);
    if (has_suffix(name, k_shared_lib_suffix)) {
      std::string full = directory;
      if (!full.empty() && full.back() != '\\' && full.back() != '/') {
        full.push_back('\\');
      }
      full.append(name);
      candidates.push_back(std::move(full));
    }
  } while (::FindNextFileA(handle, &find_data) != 0);
  ::FindClose(handle);
#else
  DIR* handle = ::opendir(directory.c_str());
  if (handle == nullptr) {
    return;
  }
  for (const dirent* entry = ::readdir(handle); entry != nullptr; entry = ::readdir(handle)) {
    const std::string_view name(entry->d_name);
    if (has_suffix(name, k_shared_lib_suffix)) {
      std::string full = directory;
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

PluginScanReport PluginHost::scan_plugin_path() {
  PluginScanReport report;

  const char* env = read_env("ARBC_PLUGIN_PATH");
  if (env == nullptr || env[0] == '\0') {
    return report; // genuinely opt-in: unset/empty -> zero loads, zero fs access
  }

  // Split the directory list on the platform separator and scan each in listed
  // order.
  for (const std::string& dir : split_directory_list(env, k_path_separator)) {
    scan_directory(dir, report);
  }

  return report;
}

PluginScanReport PluginHost::scan_standard_paths() {
  PluginScanReport report;

  // Env-listed dirs first, exactly as scan_plugin_path orders them, then the
  // platform-conventional defaults (Constraint 2): the full walk order is a pure
  // function of environment + build constants.
  std::vector<std::string> directories;
  const char* env = read_env("ARBC_PLUGIN_PATH");
  if (env != nullptr && env[0] != '\0') {
    directories = split_directory_list(env, k_path_separator);
  }
  for (std::string& dir : default_plugin_directories()) {
    directories.push_back(std::move(dir));
  }

  // Each directory is scanned at most once per combined scan (Constraint 3):
  // dedup by string equality after trailing-separator trim (Decision 4). Aliased
  // paths this cannot see (symlinks, case) stay covered by the registry's
  // DuplicateId guard -- the semantic safety net.
  std::vector<std::string> visited;
  for (const std::string& dir : directories) {
    std::string key = trim_trailing_separators(dir);
    if (std::find(visited.begin(), visited.end(), key) != visited.end()) {
      continue;
    }
    visited.push_back(std::move(key));
    scan_directory(dir, report);
  }

  return report;
}

} // namespace arbc
