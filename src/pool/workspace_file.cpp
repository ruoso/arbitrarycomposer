// fallocate(2) and FALLOC_FL_* require _GNU_SOURCE under glibc; define it
// before any system header is pulled in.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <arbc/pool/slot_store.hpp> // align_up
#include <arbc/pool/workspace_file.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if ARBC_HAS_WORKSPACE_FILES
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// FSCTL_SET_SPARSE / FSCTL_SET_ZERO_DATA / FILE_ZERO_DATA_INFORMATION live here and
// are NOT pulled in by <windows.h> under WIN32_LEAN_AND_MEAN.
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#if defined(__linux__)
#include <linux/falloc.h>
#endif
#endif
#endif

namespace arbc {

void debug_assert_position_independent([[maybe_unused]] const void* record,
                                       [[maybe_unused]] std::size_t size,
                                       [[maybe_unused]] const void* map_base,
                                       [[maybe_unused]] std::size_t map_bytes) noexcept {
#ifndef NDEBUG
  // A record holding a raw pointer into the mapping would not survive remap /
  // reopen at a different base address; only index-only SlotRefs are legal.
  const auto* words = static_cast<const std::uintptr_t*>(record);
  const std::size_t count = size / sizeof(std::uintptr_t);
  const auto lo = reinterpret_cast<std::uintptr_t>(map_base);
  const auto hi = lo + map_bytes;
  for (std::size_t i = 0; i < count; ++i) {
    const std::uintptr_t value = words[i];
    assert((value < lo || value >= hi) &&
           "record holds a raw pointer into the workspace mapping; use a SlotRef");
  }
#endif
}

WorkspaceHeader* WorkspaceFileChunkSource::header() noexcept {
  return static_cast<WorkspaceHeader*>(d_header_map);
}

WorkspaceChunkEntry* WorkspaceFileChunkSource::directory() noexcept {
  return reinterpret_cast<WorkspaceChunkEntry*>(static_cast<std::byte*>(d_header_map) +
                                                sizeof(WorkspaceHeader));
}

WorkspaceStoreEntry* WorkspaceFileChunkSource::store_table(int ab) noexcept {
  auto* base = reinterpret_cast<WorkspaceStoreEntry*>(static_cast<std::byte*>(d_header_map) +
                                                      d_store_table_offset);
  return base + static_cast<std::size_t>(ab) * d_max_stores;
}

void WorkspaceFileChunkSource::init_store_directory(std::uint32_t max_stores,
                                                    std::uint64_t table_offset) {
  d_max_stores = max_stores;
  d_store_table_offset = table_offset;
  d_views.resize(max_stores);
  d_store_reopened.resize(max_stores);
  d_store_cursor.assign(max_stores, 0);
  d_restored_stores.assign(max_stores, WorkspaceStoreEntry{0, 0, 0, 0});
  d_router.self = this;
}

std::uint32_t WorkspaceFileChunkSource::store_id(std::uint32_t slot_stride,
                                                 std::uint32_t slot_align) noexcept {
  // The identity columns are duplicated into both snapshots, so either answers. A
  // source only exists with its header mapped (create/open return a value or
  // nothing), and `d_max_stores` is zero only on a platform where neither can
  // succeed, so the loop simply finds no row there.
  const WorkspaceStoreEntry* table = d_max_stores != 0 ? store_table(0) : nullptr;
  for (std::uint32_t r = 0; r < d_max_stores; ++r) {
    if (table[r].slot_stride == slot_stride && table[r].slot_align == slot_align) {
      return r;
    }
  }
  return k_workspace_no_owner;
}

void WorkspaceFileChunkSource::publish_store_high_water(int ab, std::uint32_t id,
                                                        std::uint32_t high_water) noexcept {
  // `id` always came from `store_id`, and `register_store` drops the no-owner answer.
  assert(id < d_max_stores);
  store_table(ab)[id].high_water = high_water;
}

ChunkSource* WorkspaceFileChunkSource::Router::source_for(std::size_t slot_stride,
                                                          std::size_t slot_align,
                                                          std::size_t chunk_slots) {
  expected<ChunkSource*, WorkspaceFileError> view = self->store_view(
      static_cast<std::uint32_t>(slot_stride), static_cast<std::uint32_t>(slot_align),
      static_cast<std::uint32_t>(chunk_slots));
  // A refusal is reported to the Arena as "no source" (it binds the store to a
  // RefusingChunkSource); `bind_error()` carries the reason.
  return view ? *view : nullptr;
}

expected<ChunkSource*, WorkspaceFileError>
WorkspaceFileChunkSource::store_view(std::uint32_t slot_stride, std::uint32_t slot_align,
                                     std::uint32_t chunk_slots) {
  const auto refuse = [this](WorkspaceFileErrc code) {
    d_bind_error = WorkspaceFileError{code, 0};
    return unexpected(d_bind_error);
  };
  if (d_max_stores == 0) {
    // GCOV_EXCL_LINE: a source only exists with a mapped store table (create/open
    // return a value or nothing), so this guards the stub platform, where neither
    // can succeed and no instance can be constructed to reach it.
    return refuse(WorkspaceFileErrc::Unsupported); // GCOV_EXCL_LINE
  }
  // A reopened file's rows only become knowable once the selected root's snapshot
  // is adopted; binding before that would append a fresh row over a store the file
  // already owns. `Checkpointer::open` adopts, so this only fires on misuse.
  if (d_from_file && !d_snapshot_adopted) {
    return refuse(WorkspaceFileErrc::StoreDirectoryInconsistent);
  }

  WorkspaceStoreEntry* table_a = store_table(0);
  WorkspaceStoreEntry* table_b = store_table(1);
  std::uint32_t free_row = d_max_stores;
  for (std::uint32_t r = 0; r < d_max_stores; ++r) {
    if (table_a[r].slot_stride == 0) {
      if (free_row == d_max_stores) {
        free_row = r;
      }
      continue;
    }
    if (table_a[r].slot_stride != slot_stride || table_a[r].slot_align != slot_align) {
      continue;
    }
    // The size class is already on disk. Its slots-per-chunk must match this
    // build's, or the file's chunks do not carve into the slots this build would
    // read -- the debug-vs-release lane mismatch. Refuse as a value rather than
    // mis-route (doc 15: "a file whose store table disagrees with the reopening
    // build's strides is refused as a value").
    if (table_a[r].chunk_slots != chunk_slots) {
      return refuse(WorkspaceFileErrc::StoreLayoutMismatch);
    }
    // Re-stamp the identity columns into BOTH snapshots. Byte-identical to what is
    // already in A (that is why we matched), so this is a no-op there -- but it
    // REPAIRS a B whose row was lost to a torn write, which would otherwise become a
    // zero-stride row that the next commit gives a nonzero high-water to, and which a
    // later recovery would then have to refuse. Identity is per-store, not per-root,
    // so re-deriving it costs nothing and is always safe.
    table_a[r] = WorkspaceStoreEntry{slot_stride, slot_align, chunk_slots, table_a[r].high_water};
    table_b[r] = WorkspaceStoreEntry{slot_stride, slot_align, chunk_slots, table_b[r].high_water};
    if (d_views[r] == nullptr) {
      d_views[r].reset(new WorkspaceStoreView(*this, r));
    }
    return d_views[r].get();
  }

  if (free_row == d_max_stores) {
    return refuse(WorkspaceFileErrc::MaxStoresExceeded);
  }
  // A fresh row. The identity columns are per-store, not per-root, so they go into
  // BOTH snapshots; only `high_water` differs per snapshot, and it starts at 0 --
  // a zero high-water needs zero chunks, so a crash right after this leaves either
  // root recoverable. Row occupancy therefore stays identical in A and B.
  const WorkspaceStoreEntry row{slot_stride, slot_align, chunk_slots, 0};
  table_a[free_row] = row;
  table_b[free_row] = row;
  d_restored_stores[free_row] = row;
  d_views[free_row].reset(new WorkspaceStoreView(*this, free_row));
  return d_views[free_row].get();
}

#if ARBC_HAS_WORKSPACE_FILES

namespace {

#if defined(_WIN32)
// On Windows the syscall-level diagnostic is `GetLastError()` (a Win32 error
// code), captured into `sys_errno`. Both are same-machine diagnostics with no
// portability promise (doc 15:204-206).
WorkspaceFileError sys_error(WorkspaceFileErrc code) {
  return WorkspaceFileError{code, static_cast<int>(::GetLastError())};
}
#else
WorkspaceFileError sys_error(WorkspaceFileErrc code) { return WorkspaceFileError{code, errno}; }
#endif

// Portable aliases so the shared orchestration (acquire/sync/protect) never names
// a POSIX-only mmap/msync macro. The values that reach the `io_*` shims mirror the
// POSIX `PROT_*` bits so the write-bit test in `io_mprotect` is identical on both
// platforms; the map/msync-flag aliases are ignored by the Windows shims.
// `k_prot_read` is used only in the debug-only protect helpers, so it is
// [[maybe_unused]] to stay warning-clean in NDEBUG builds.
#if defined(_WIN32)
[[maybe_unused]] constexpr int k_prot_read = 0x1;
constexpr int k_prot_readwrite = 0x1 | 0x2;
constexpr int k_map_shared = 0;
constexpr int k_msync_sync = 0;
void* const k_map_failed = nullptr;
#else
[[maybe_unused]] constexpr int k_prot_read = PROT_READ;
constexpr int k_prot_readwrite = PROT_READ | PROT_WRITE;
constexpr int k_map_shared = MAP_SHARED;
constexpr int k_msync_sync = MS_SYNC;
void* const k_map_failed = MAP_FAILED;
#endif

} // namespace

// --- fault-injection shims (pool.crash_tests) --------------------------------
// When no injector is installed every shim is a single predictable branch over
// the real syscall. When installed, `before` may inject an errno (skipping the
// real call) or snapshot/kill the process; `after` observes the durable effect.

#if defined(_WIN32)

// On Windows an injected failure sets `GetLastError()` (via SetLastError) so the
// caller's `sys_error()` captures it exactly as the POSIX shims set `errno`.

int WorkspaceFileChunkSource::io_ftruncate(std::int64_t length) noexcept {
  auto do_call = [this](std::int64_t len) -> int {
    LARGE_INTEGER li;
    li.QuadPart = len;
    HANDLE h = static_cast<HANDLE>(d_fd);
    if (!::SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !::SetEndOfFile(h)) {
      return -1;
    }
    return 0;
  };
  if (d_injector != nullptr) {
    const int inj =
        d_injector->before(WorkspaceSyscall::Ftruncate, static_cast<std::uint64_t>(length), 0);
    if (inj != 0) {
      ::SetLastError(static_cast<DWORD>(inj));
      return -1;
    }
    const int rc = do_call(length);
    d_injector->after(WorkspaceSyscall::Ftruncate, static_cast<std::uint64_t>(length), 0);
    return rc;
  }
  return do_call(length);
}

void* WorkspaceFileChunkSource::io_mmap(std::size_t len, int prot, int flags,
                                        std::int64_t offset) noexcept {
  (void)prot;
  (void)flags;
  // Recreate the file-mapping object at the new file end (the file was already
  // extended by io_ftruncate), then map the new chunk. Existing views keep their
  // own reference, so closing the old mapping handle does not move them
  // (address stability, Decision 3).
  auto do_call = [this](std::size_t l, std::int64_t off) -> void* {
    const std::uint64_t max_size = static_cast<std::uint64_t>(off) + l;
    HANDLE file = static_cast<HANDLE>(d_fd);
    HANDLE mapping =
        ::CreateFileMappingW(file, nullptr, PAGE_READWRITE, static_cast<DWORD>(max_size >> 32),
                             static_cast<DWORD>(max_size & 0xFFFFFFFFu), nullptr);
    if (mapping == nullptr) {
      return nullptr;
    }
    void* view = ::MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, static_cast<DWORD>(static_cast<std::uint64_t>(off) >> 32),
        static_cast<DWORD>(static_cast<std::uint64_t>(off) & 0xFFFFFFFFu), l);
    if (view == nullptr) {
      const DWORD err = ::GetLastError();
      ::CloseHandle(mapping);
      ::SetLastError(err);
      return nullptr;
    }
    if (d_mapping != nullptr) {
      ::CloseHandle(static_cast<HANDLE>(d_mapping));
    }
    d_mapping = mapping;
    return view;
  };
  if (d_injector != nullptr) {
    const int inj =
        d_injector->before(WorkspaceSyscall::Mmap, static_cast<std::uint64_t>(offset), len);
    if (inj != 0) {
      ::SetLastError(static_cast<DWORD>(inj));
      return k_map_failed;
    }
    void* p = do_call(len, offset);
    d_injector->after(WorkspaceSyscall::Mmap, static_cast<std::uint64_t>(offset), len);
    return p;
  }
  return do_call(len, offset);
}

int WorkspaceFileChunkSource::io_fallocate(int mode, std::int64_t offset,
                                           std::int64_t len) noexcept {
  (void)mode;
  // Deallocate the range on a sparse NTFS file (the hole-punch). Best-effort:
  // a non-sparse volume fails here and punch_now degrades to directory-clear.
  auto do_call = [this](std::int64_t off, std::int64_t l) -> int {
    FILE_ZERO_DATA_INFORMATION zero;
    zero.FileOffset.QuadPart = off;
    zero.BeyondFinalZero.QuadPart = off + l;
    DWORD returned = 0;
    HANDLE h = static_cast<HANDLE>(d_fd);
    if (!::DeviceIoControl(h, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero), nullptr, 0, &returned,
                           nullptr)) {
      return -1;
    }
    return 0;
  };
  if (d_injector != nullptr) {
    const int inj =
        d_injector->before(WorkspaceSyscall::Fallocate, static_cast<std::uint64_t>(offset),
                           static_cast<std::size_t>(len));
    if (inj != 0) {
      ::SetLastError(static_cast<DWORD>(inj));
      return -1;
    }
    const int rc = do_call(offset, len);
    d_injector->after(WorkspaceSyscall::Fallocate, static_cast<std::uint64_t>(offset),
                      static_cast<std::size_t>(len));
    return rc;
  }
  return do_call(offset, len);
}

int WorkspaceFileChunkSource::io_msync(void* addr, std::size_t len, int flags,
                                       std::uint64_t file_offset) noexcept {
  (void)flags;
  // Mechanical body only (Decision 5): FlushViewOfFile pushes the dirty pages to
  // the file cache, FlushFileBuffers forces them to disk. Ordered-commit
  // correctness is pool.checkpoints_win32.
  auto do_call = [this](void* a, std::size_t l) -> int {
    if (!::FlushViewOfFile(a, l) || !::FlushFileBuffers(static_cast<HANDLE>(d_fd))) {
      return -1;
    }
    return 0;
  };
  if (d_injector != nullptr) {
    const int inj = d_injector->before(WorkspaceSyscall::Msync, file_offset, len);
    if (inj != 0) {
      ::SetLastError(static_cast<DWORD>(inj));
      return -1;
    }
    const int rc = do_call(addr, len);
    d_injector->after(WorkspaceSyscall::Msync, file_offset, len);
    return rc;
  }
  return do_call(addr, len);
}

int WorkspaceFileChunkSource::io_mprotect(void* addr, std::size_t len, int prot) noexcept {
  // Mechanical body only (Decision 5): map the POSIX-mirrored prot bits to a
  // Win32 page protection. Debug read-only-publish semantics are
  // pool.checkpoints_win32.
  auto do_call = [](void* a, std::size_t l, int p) -> int {
    const DWORD page_prot = (p & 0x2) != 0 ? PAGE_READWRITE : PAGE_READONLY;
    DWORD old = 0;
    if (!::VirtualProtect(a, l, page_prot, &old)) {
      return -1;
    }
    return 0;
  };
  if (d_injector != nullptr) {
    const int inj = d_injector->before(WorkspaceSyscall::Mprotect, 0, len);
    if (inj != 0) {
      ::SetLastError(static_cast<DWORD>(inj));
      return -1;
    }
    const int rc = do_call(addr, len, prot);
    d_injector->after(WorkspaceSyscall::Mprotect, 0, len);
    return rc;
  }
  return do_call(addr, len, prot);
}

#else // POSIX io_* shims

int WorkspaceFileChunkSource::io_ftruncate(std::int64_t length) noexcept {
  if (d_injector != nullptr) {
    const int inj =
        d_injector->before(WorkspaceSyscall::Ftruncate, static_cast<std::uint64_t>(length), 0);
    if (inj != 0) {
      errno = inj;
      return -1;
    }
    const int rc = ::ftruncate(d_fd, static_cast<off_t>(length));
    d_injector->after(WorkspaceSyscall::Ftruncate, static_cast<std::uint64_t>(length), 0);
    return rc;
  }
  return ::ftruncate(d_fd, static_cast<off_t>(length));
}

void* WorkspaceFileChunkSource::io_mmap(std::size_t len, int prot, int flags,
                                        std::int64_t offset) noexcept {
  if (d_injector != nullptr) {
    const int inj =
        d_injector->before(WorkspaceSyscall::Mmap, static_cast<std::uint64_t>(offset), len);
    if (inj != 0) {
      errno = inj;
      return MAP_FAILED;
    }
    void* p = ::mmap(nullptr, len, prot, flags, d_fd, static_cast<off_t>(offset));
    d_injector->after(WorkspaceSyscall::Mmap, static_cast<std::uint64_t>(offset), len);
    return p;
  }
  return ::mmap(nullptr, len, prot, flags, d_fd, static_cast<off_t>(offset));
}

#if defined(__linux__)
int WorkspaceFileChunkSource::io_fallocate(int mode, std::int64_t offset,
                                           std::int64_t len) noexcept {
  if (d_injector != nullptr) {
    const int inj =
        d_injector->before(WorkspaceSyscall::Fallocate, static_cast<std::uint64_t>(offset),
                           static_cast<std::size_t>(len));
    if (inj != 0) {
      errno = inj;
      return -1;
    }
    const int rc = ::fallocate(d_fd, mode, static_cast<off_t>(offset), static_cast<off_t>(len));
    d_injector->after(WorkspaceSyscall::Fallocate, static_cast<std::uint64_t>(offset),
                      static_cast<std::size_t>(len));
    return rc;
  }
  return ::fallocate(d_fd, mode, static_cast<off_t>(offset), static_cast<off_t>(len));
}
#endif

int WorkspaceFileChunkSource::io_msync(void* addr, std::size_t len, int flags,
                                       std::uint64_t file_offset) noexcept {
  if (d_injector != nullptr) {
    const int inj = d_injector->before(WorkspaceSyscall::Msync, file_offset, len);
    if (inj != 0) {
      errno = inj;
      return -1;
    }
    const int rc = ::msync(addr, len, flags);
    d_injector->after(WorkspaceSyscall::Msync, file_offset, len);
    return rc;
  }
  return ::msync(addr, len, flags);
}

int WorkspaceFileChunkSource::io_mprotect(void* addr, std::size_t len, int prot) noexcept {
  if (d_injector != nullptr) {
    const int inj = d_injector->before(WorkspaceSyscall::Mprotect, 0, len);
    if (inj != 0) {
      errno = inj;
      return -1;
    }
    const int rc = ::mprotect(addr, len, prot);
    d_injector->after(WorkspaceSyscall::Mprotect, 0, len);
    return rc;
  }
  return ::mprotect(addr, len, prot);
}

#endif // _WIN32 / POSIX io_* shims

expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
WorkspaceFileChunkSource::create(const std::string& path, const WorkspaceLayout& layout) {
#if defined(_WIN32)
  SYSTEM_INFO si;
  ::GetSystemInfo(&si);
  // MapViewOfFile requires the view offset be a multiple of the allocation
  // granularity (Decision 2), so the quantum is max(page, granularity).
  const std::size_t page = std::max<std::size_t>(si.dwPageSize, si.dwAllocationGranularity);
#else
  const long page_query = ::sysconf(_SC_PAGESIZE);
  const std::size_t page = page_query > 0 ? static_cast<std::size_t>(page_query) : 4096;
#endif

  const std::uint32_t max_chunks =
      layout.max_chunks == 0 ? k_workspace_default_max_chunks : layout.max_chunks;
  const std::uint32_t max_stores =
      layout.max_stores == 0 ? k_workspace_default_max_stores : layout.max_stores;
  const std::size_t directory_bytes =
      static_cast<std::size_t>(max_chunks) * sizeof(WorkspaceChunkEntry);
  // The two store-table snapshots sit AFTER the chunk directory and before the
  // page-aligned data region, so the directory's offset (immediately after the
  // header) does not move. Both struct sizes are multiples of 8, so the table
  // stays 8-aligned. For the default layout the tables fit entirely in the page
  // slack the directory already leaves, so the file grows by zero pages --
  // `data_offset` is a stored header field either way, so the layout is
  // self-describing regardless.
  const std::size_t store_table_offset = sizeof(WorkspaceHeader) + directory_bytes;
  const std::size_t store_table_bytes = std::size_t{2} * max_stores * sizeof(WorkspaceStoreEntry);
  const std::size_t header_bytes = align_up(store_table_offset + store_table_bytes, page);

#if defined(_WIN32)
  HANDLE fd = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fd == INVALID_HANDLE_VALUE) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
  // Mark sparse so FSCTL_SET_ZERO_DATA can reclaim disk on hole-punch; a
  // non-sparse volume simply fails this and punch degrades to directory-clear.
  {
    DWORD returned = 0;
    ::DeviceIoControl(fd, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
  }
  {
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(header_bytes);
    if (!::SetFilePointerEx(fd, li, nullptr, FILE_BEGIN) || !::SetEndOfFile(fd)) {
      WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
      ::CloseHandle(fd);
      return unexpected(err);
    }
  }
  HANDLE mapping =
      ::CreateFileMappingW(fd, nullptr, PAGE_READWRITE,
                           static_cast<DWORD>(static_cast<std::uint64_t>(header_bytes) >> 32),
                           static_cast<DWORD>(header_bytes & 0xFFFFFFFFu), nullptr);
  if (mapping == nullptr) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::CloseHandle(fd);
    return unexpected(err);
  }
  void* map = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, header_bytes);
  if (map == nullptr) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::CloseHandle(mapping);
    ::CloseHandle(fd);
    return unexpected(err);
  }
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
  if (::ftruncate(fd, static_cast<off_t>(header_bytes)) != 0) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::close(fd);
    return unexpected(err);
  }
  void* map = ::mmap(nullptr, header_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::close(fd);
    return unexpected(err);
  }
#endif

  // Zero only the fixed header; the directory and store-table regions read as zeros
  // from the ftruncate hole until acquire()/store_view() write entries, keeping the
  // file sparse. An all-zero store-table row IS the "unused" encoding
  // (slot_stride == 0), so no explicit initialization is needed.
  std::memset(map, 0, sizeof(WorkspaceHeader));
  auto* hdr = static_cast<WorkspaceHeader*>(map);
  hdr->magic = k_workspace_magic;
  hdr->format_major = k_workspace_format_major;
  hdr->format_minor = k_workspace_format_minor;
  hdr->page_size = static_cast<std::uint32_t>(page);
  hdr->max_chunks = max_chunks;
  hdr->data_offset = header_bytes;
  hdr->chunk_count = 0;
  hdr->root_slot_a = 0; // reserved for pool.checkpoints; layout is this task's contract
  hdr->root_slot_b = 0;
  hdr->store_table_offset = store_table_offset;
  hdr->max_stores = max_stores;

  auto source = std::unique_ptr<WorkspaceFileChunkSource>(new WorkspaceFileChunkSource());
  source->d_path = path;
  source->d_fd = fd;
#if defined(_WIN32)
  source->d_mapping = mapping;
#endif
  source->d_page = page;
  source->d_max_chunks = max_chunks;
  source->d_data_offset = header_bytes;
  source->d_next_offset = header_bytes;
  source->d_chunk_count = 0;
  source->d_header_map = map;
  source->d_header_bytes = header_bytes;
  source->init_store_directory(max_stores, store_table_offset);
  return source;
}

WorkspaceFileChunkSource::~WorkspaceFileChunkSource() {
#if defined(_WIN32)
  for (const auto& entry : d_live) {
    ::UnmapViewOfFile(entry.first);
  }
  // Remapped-on-open chunks never pulled back through acquire -- the untagged
  // queue and every per-store queue.
  for (std::size_t i = d_reopened_cursor; i < d_reopened.size(); ++i) {
    ::UnmapViewOfFile(d_reopened[i].base);
  }
  for (std::size_t r = 0; r < d_store_reopened.size(); ++r) {
    for (std::size_t i = d_store_cursor[r]; i < d_store_reopened[r].size(); ++i) {
      ::UnmapViewOfFile(d_store_reopened[r][i].base);
    }
  }
  if (d_header_map != nullptr) {
    ::UnmapViewOfFile(d_header_map);
  }
  if (d_mapping != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(d_mapping));
  }
  if (d_fd != nullptr) {
    ::CloseHandle(static_cast<HANDLE>(d_fd));
  }
#else
  for (const auto& entry : d_live) {
    ::munmap(entry.first, entry.second.size);
  }
  // Remapped-on-open chunks never pulled back through acquire -- the untagged
  // queue and every per-store queue.
  for (std::size_t i = d_reopened_cursor; i < d_reopened.size(); ++i) {
    ::munmap(d_reopened[i].base, d_reopened[i].size);
  }
  for (std::size_t r = 0; r < d_store_reopened.size(); ++r) {
    for (std::size_t i = d_store_cursor[r]; i < d_store_reopened[r].size(); ++i) {
      ::munmap(d_store_reopened[r][i].base, d_store_reopened[r][i].size);
    }
  }
  if (d_header_map != nullptr) {
    ::munmap(d_header_map, d_header_bytes);
  }
  if (d_fd >= 0) {
    ::close(d_fd);
  }
#endif
}

expected<ChunkSpan, PoolError> WorkspaceFileChunkSource::acquire(std::size_t size,
                                                                 std::size_t alignment) {
  // The untagged path: a caller using the source directly as a ChunkSource, with no
  // per-store view. Its chunks carry no owner tag and reopen through the one FIFO
  // queue, exactly as format 1 did.
  //
  // Recovery path: hand back an already-mapped file chunk (in directory order)
  // rather than growing the file. reserve_restored drives this to re-bind the
  // records the walk reads.
  if (d_reopened_cursor < d_reopened.size()) {
    const ReopenedChunk& rc = d_reopened[d_reopened_cursor++];
    d_live.emplace(rc.base, LiveChunk{rc.offset, rc.size, rc.index});
    return ChunkSpan{rc.base, rc.size};
  }
  return grow(size, alignment, k_workspace_no_owner);
}

expected<ChunkSpan, PoolError>
WorkspaceFileChunkSource::acquire_for(std::uint32_t id, std::size_t size, std::size_t alignment) {
  assert(id < d_max_stores);
  // A store view sees ONLY its own store's chunks. On a reopened file it drains them
  // in directory order -- which, because growth is strictly append-only and a punched
  // entry is never re-used, IS this store's acquisition order, so its k-th chunk
  // backs slot range [k*chunk_slots, (k+1)*chunk_slots): exactly what
  // `reserve_restored` assumes (Decision 6).
  std::vector<ReopenedChunk>& queue = d_store_reopened[id];
  std::size_t& cursor = d_store_cursor[id];
  if (cursor < queue.size()) {
    const ReopenedChunk& rc = queue[cursor++];
    d_live.emplace(rc.base, LiveChunk{rc.offset, rc.size, rc.index});
    return ChunkSpan{rc.base, rc.size};
  }
  return grow(size, alignment, id);
}

expected<ChunkSpan, PoolError>
WorkspaceFileChunkSource::grow(std::size_t size, std::size_t alignment, std::uint32_t owner) {
  // mmap hands back page-aligned addresses; every slot alignment we support is
  // far smaller, matching the AnonymousChunkSource contract.
  assert(alignment <= d_page);
  (void)alignment;

  if (d_chunk_count >= d_max_chunks) {
    d_last_error = WorkspaceFileError{WorkspaceFileErrc::DirectoryFull, 0};
    return unexpected(PoolError::OutOfMemory);
  }

  const std::size_t rounded = align_up(size == 0 ? 1 : size, d_page);
  const std::uint64_t offset = d_next_offset;
  const std::uint64_t new_end = offset + rounded;

  // ftruncate extends the file by whole chunks (disk-full surfaces here as a
  // value, never an abort).
  if (io_ftruncate(static_cast<std::int64_t>(new_end)) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
    return unexpected(PoolError::OutOfMemory);
  }
  void* base = io_mmap(rounded, k_prot_readwrite, k_map_shared, static_cast<std::int64_t>(offset));
  if (base == k_map_failed) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
    return unexpected(PoolError::OutOfMemory);
  }

  const std::uint32_t index = d_chunk_count;
  WorkspaceChunkEntry& slot = directory()[index];
  slot.offset = offset;
  slot.size = rounded;
  slot.state = k_workspace_chunk_live;
  slot.owner = owner; // the arena directory's routing tag

  d_chunk_count = index + 1;
  d_next_offset = new_end;
  header()->chunk_count = d_chunk_count;

  d_live.emplace(base, LiveChunk{offset, rounded, index});
  return ChunkSpan{base, rounded};
}

void WorkspaceFileChunkSource::release(ChunkSpan span) noexcept {
  auto it = d_live.find(span.base);
  if (it == d_live.end()) {
    return; // not one of ours; releasing an unknown span is a no-op
  }
  const LiveChunk chunk = it->second;

#if defined(_WIN32)
  ::UnmapViewOfFile(span.base);
#else
  ::munmap(span.base, chunk.size);
#endif
  d_live.erase(it);

  if (d_release_fence != nullptr) {
    // Durability fence: the chunk may still back the on-disk root, so defer the
    // hole-punch (and the directory clear) to the post-durable commit drain.
    d_release_fence->on_release(*this, chunk.offset, chunk.size, chunk.index);
    return;
  }

  punch_now(chunk.offset, chunk.size, chunk.index);
}

void WorkspaceFileChunkSource::punch_now(std::uint64_t offset, std::uint64_t size,
                                         std::uint32_t index) noexcept {
#if defined(_WIN32)
  // Return storage: FSCTL_SET_ZERO_DATA deallocates the chunk's range on a sparse
  // NTFS file, so both memory (unmapped in release) and disk come back. Best-effort:
  // on a non-sparse volume it fails and this degrades to directory-clear only.
  if (io_fallocate(0, static_cast<std::int64_t>(offset), static_cast<std::int64_t>(size)) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
  }
#elif defined(__linux__)
  // Return storage: hole-punch the chunk's file range while keeping the logical
  // size, so both memory and disk come back (fixing the grow-forever high-water
  // mark). Non-fatal if the filesystem lacks punch support.
  if (io_fallocate(FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, static_cast<std::int64_t>(offset),
                   static_cast<std::int64_t>(size)) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
  }
#else
  (void)offset;
  (void)size;
#endif
  directory()[index].state = k_workspace_chunk_free;
}

void WorkspaceFileChunkSource::discard_reopened(const ReopenedChunk& chunk) noexcept {
#if defined(_WIN32)
  ::UnmapViewOfFile(chunk.base);
#else
  ::munmap(chunk.base, chunk.size);
#endif
  // Unfenced on purpose: this chunk is above the SELECTED root's high-water, so
  // that root provably does not reference it. There is nothing to wait for.
  punch_now(chunk.offset, chunk.size, chunk.index);
}

expected<std::monostate, WorkspaceFileError> WorkspaceFileChunkSource::adopt_snapshot(int ab) {
  const WorkspaceStoreEntry* table = store_table(ab);
  d_restored_stores.assign(table, table + d_max_stores);

  const auto inconsistent = []() {
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::StoreDirectoryInconsistent, 0});
  };

  // Rows a store's chunk count is derived from must be geometry a reopening build
  // can act on at all: a power-of-two slots-per-chunk (the shift SlotStore indexes
  // chunks with, capped by its 10+10 directory split) over a power-of-two alignment
  // the stride is a multiple of. A corrupt row is a value error, never an
  // out-of-range shift or a division by zero.
  for (std::uint32_t r = 0; r < d_max_stores; ++r) {
    const WorkspaceStoreEntry& row = d_restored_stores[r];
    if (row.slot_stride == 0) {
      if (row.slot_align != 0 || row.chunk_slots != 0 || row.high_water != 0) {
        return inconsistent(); // an "unused" row carrying data is a torn table
      }
      continue;
    }
    // The row must ALSO be in the canonical form `Arena::store_for` produces --
    // alignment at least `alignof(max_align_t)`, stride a multiple of it. Otherwise
    // `reserve_restored_all` would hand `store_for` a row it normalizes to a
    // DIFFERENT key, find no matching row, and mint a second store over a file that
    // already holds the first one's chunks. Refuse the row instead.
    if (row.chunk_slots == 0 || (row.chunk_slots & (row.chunk_slots - 1)) != 0 ||
        row.chunk_slots > (1u << 12) || row.slot_align < alignof(std::max_align_t) ||
        (row.slot_align & (row.slot_align - 1)) != 0 || row.slot_stride % row.slot_align != 0) {
      return unexpected(WorkspaceFileError{WorkspaceFileErrc::StoreLayoutMismatch, 0});
    }
    // ASSERTED, NOT TRUSTED: the store's chunk set must COVER the high-water the
    // selected root published. A shortfall means a hole in the store's chunk
    // sequence, a mis-tagged chunk, or a torn directory -- all of which would make
    // `reserve_restored` bind a slot range to the wrong bytes. Refuse instead.
    const std::size_t needed =
        row.high_water == 0 ? 0 : ((row.high_water - 1) / row.chunk_slots) + 1;
    if (d_store_reopened[r].size() < needed) {
      return inconsistent();
    }
  }

  // Post-checkpoint chunk garbage: a crash after chunks were appended but before
  // the commit that would have published them leaves live chunks ABOVE the selected
  // root's high-water. `reserve_restored` never claims them (they are past
  // `chunks_needed`), so they would leak storage across an unclean shutdown. Drop
  // and hole-punch them -- the chunk-level analogue of the freed-slot durability
  // quarantine. A chunk tagged with a row this snapshot never bound is garbage too
  // (a crashed commit that was minting a new size class): its `needed` is 0.
  for (std::uint32_t r = 0; r < d_max_stores; ++r) {
    const WorkspaceStoreEntry& row = d_restored_stores[r];
    const std::size_t keep = row.slot_stride == 0 || row.high_water == 0
                                 ? 0
                                 : ((row.high_water - 1) / row.chunk_slots) + 1;
    std::vector<ReopenedChunk>& queue = d_store_reopened[r];
    for (std::size_t k = keep; k < queue.size(); ++k) {
      discard_reopened(queue[k]);
    }
    queue.resize(keep);
  }

  d_snapshot_adopted = true;
  return std::monostate{};
}

expected<WorkspaceHeader, WorkspaceFileError>
WorkspaceFileChunkSource::read_header(const std::string& path) {
  WorkspaceHeader hdr{};
#if defined(_WIN32)
  HANDLE fd = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fd == INVALID_HANDLE_VALUE) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
  DWORD got = 0;
  const BOOL ok = ::ReadFile(fd, &hdr, sizeof(hdr), &got, nullptr);
  const DWORD read_err = ::GetLastError();
  ::CloseHandle(fd);
  if (ok == FALSE || got != sizeof(hdr)) {
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed,
                                         ok == FALSE ? static_cast<int>(read_err) : 0});
  }
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
  const ssize_t got = ::pread(fd, &hdr, sizeof(hdr), 0);
  ::close(fd);
  if (got != static_cast<ssize_t>(sizeof(hdr))) {
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, got < 0 ? errno : 0});
  }
#endif
  if (hdr.magic != k_workspace_magic) {
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::BadMagic, 0});
  }
  if (hdr.format_major != k_workspace_format_major) {
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::UnsupportedFormat, 0});
  }
  return hdr;
}

expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
WorkspaceFileChunkSource::open(const std::string& path) {
#if defined(_WIN32)
  HANDLE fd = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fd == INVALID_HANDLE_VALUE) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
#else
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
#endif
  // Close the raw handle on any pre-source failure path (platform leaf).
  auto close_fd = [&]() noexcept {
#if defined(_WIN32)
    ::CloseHandle(fd);
#else
    ::close(fd);
#endif
  };

  // Read the fixed header first to learn page size / directory capacity, then
  // map the whole header+directory region.
  WorkspaceHeader fixed{};
#if defined(_WIN32)
  {
    DWORD got = 0;
    const BOOL ok = ::ReadFile(fd, &fixed, sizeof(fixed), &got, nullptr);
    const DWORD read_err = ::GetLastError();
    if (ok == FALSE || got != sizeof(fixed)) {
      WorkspaceFileError err{WorkspaceFileErrc::HeaderIoFailed,
                             ok == FALSE ? static_cast<int>(read_err) : 0};
      close_fd();
      return unexpected(err);
    }
  }
#else
  {
    const ssize_t got = ::pread(fd, &fixed, sizeof(fixed), 0);
    if (got != static_cast<ssize_t>(sizeof(fixed))) {
      WorkspaceFileError err{WorkspaceFileErrc::HeaderIoFailed, got < 0 ? errno : 0};
      close_fd();
      return unexpected(err);
    }
  }
#endif
  if (fixed.magic != k_workspace_magic) {
    close_fd();
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::BadMagic, 0});
  }
  if (fixed.format_major != k_workspace_format_major) {
    close_fd();
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::UnsupportedFormat, 0});
  }

  // The store table must lie wholly between the chunk directory's end and the data
  // region, so every later `store_table(ab)[r]` is inside the header mapping. A
  // corrupt/torn header is a value error, never an out-of-bounds map read.
  const std::uint64_t directory_end =
      sizeof(WorkspaceHeader) +
      static_cast<std::uint64_t>(fixed.max_chunks) * sizeof(WorkspaceChunkEntry);
  const std::uint64_t store_table_bytes =
      static_cast<std::uint64_t>(fixed.max_stores) * 2u * sizeof(WorkspaceStoreEntry);
  if (fixed.max_stores == 0 || fixed.store_table_offset < directory_end ||
      fixed.store_table_offset > fixed.data_offset ||
      store_table_bytes > fixed.data_offset - fixed.store_table_offset) {
    close_fd();
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, 0});
  }
  // The directory walk below indexes `dir[0 .. chunk_count)`, which the mapping only
  // covers up to `max_chunks`. A torn `chunk_count` must be a value error, never a
  // read past the header mapping.
  if (fixed.chunk_count > fixed.max_chunks) {
    close_fd();
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, 0});
  }

  const std::size_t header_bytes = static_cast<std::size_t>(fixed.data_offset);

  // A truncated (short) workspace file must surface as a value, never map past
  // EOF and SIGBUS on first access (pool.crash_tests short-file paths). Refuse
  // any file too small for its own header/directory region up front.
#if defined(_WIN32)
  LARGE_INTEGER fsz{};
  if (::GetFileSizeEx(fd, &fsz) == FALSE) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    close_fd();
    return unexpected(err);
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(fsz.QuadPart);
#else
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    close_fd();
    return unexpected(err); // GCOV_EXCL_LINE: fstat on a freshly-opened fd does not fail
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
#endif
  if (file_size < header_bytes) {
    close_fd();
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, 0});
  }

#if defined(_WIN32)
  // One mapping object over the whole file backs the header view and every
  // remapped chunk view; a post-reopen grow recreates it larger (io_mmap).
  HANDLE mapping =
      ::CreateFileMappingW(fd, nullptr, PAGE_READWRITE, static_cast<DWORD>(file_size >> 32),
                           static_cast<DWORD>(file_size & 0xFFFFFFFFu), nullptr);
  if (mapping == nullptr) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    close_fd();
    return unexpected(err);
  }
  void* map = ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, header_bytes);
  if (map == nullptr) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::CloseHandle(mapping);
    close_fd();
    return unexpected(err);
  }
#else
  void* map = ::mmap(nullptr, header_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    close_fd();
    return unexpected(err);
  }
#endif

  auto source = std::unique_ptr<WorkspaceFileChunkSource>(new WorkspaceFileChunkSource());
  source->d_path = path;
  source->d_fd = fd;
#if defined(_WIN32)
  source->d_mapping = mapping;
#endif
  source->d_page = fixed.page_size != 0 ? fixed.page_size : 4096;
  source->d_max_chunks = fixed.max_chunks;
  source->d_data_offset = fixed.data_offset;
  source->d_chunk_count = static_cast<std::uint32_t>(fixed.chunk_count);
  source->d_header_map = map;
  source->d_header_bytes = header_bytes;
  source->d_from_file = true;
  source->init_store_directory(static_cast<std::uint32_t>(fixed.max_stores),
                               fixed.store_table_offset);

  // Remap every live data chunk in directory order and queue it under its OWNING
  // store (untagged chunks go to the single legacy queue). Directory order within
  // one owner is that owner's acquisition order, so the queues need no sorting --
  // `adopt_snapshot` then checks each queue actually covers its store's high-water.
  std::uint64_t next_offset = fixed.data_offset;
  const WorkspaceChunkEntry* dir = source->directory();
  for (std::uint32_t i = 0; i < source->d_chunk_count; ++i) {
    const WorkspaceChunkEntry& entry = dir[i];
    if (entry.offset + entry.size > next_offset) {
      next_offset = entry.offset + entry.size;
    }
    if (entry.state != k_workspace_chunk_live) {
      continue; // released (hole-punched) — not part of the live graph
    }
    const std::uint32_t owner = entry.owner;
    if (owner != k_workspace_no_owner && owner >= source->d_max_stores) {
      // A tag naming a row the table cannot hold: a torn directory. Refuse before
      // mapping (the mapping would otherwise leak on this path).
      return unexpected(WorkspaceFileError{WorkspaceFileErrc::StoreDirectoryInconsistent, 0});
    }
    if (entry.offset + entry.size > file_size) {
      // Live chunk runs past EOF: the file was truncated mid-data-chunk. Refuse
      // rather than map past EOF (SIGBUS on access). `source`'s destructor drops
      // the header map, the chunks mapped so far, and the fd.
      return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, 0});
    }
#if defined(_WIN32)
    void* base = ::MapViewOfFile(static_cast<HANDLE>(source->d_mapping), FILE_MAP_ALL_ACCESS,
                                 static_cast<DWORD>(entry.offset >> 32),
                                 static_cast<DWORD>(entry.offset & 0xFFFFFFFFu), entry.size);
    if (base == nullptr) {
      return unexpected(sys_error(WorkspaceFileErrc::HeaderIoFailed));
    }
#else
    void* base = ::mmap(nullptr, entry.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        static_cast<off_t>(entry.offset));
    if (base == MAP_FAILED) {
      return unexpected(sys_error(WorkspaceFileErrc::HeaderIoFailed));
    }
#endif
    const ReopenedChunk chunk{base, entry.offset, entry.size, i};
    if (owner == k_workspace_no_owner) {
      source->d_reopened.push_back(chunk);
    } else {
      source->d_store_reopened[owner].push_back(chunk);
    }
  }
  source->d_next_offset = next_offset;
  return source;
}

expected<std::size_t, WorkspaceFileError> WorkspaceFileChunkSource::sync_data() noexcept {
  std::size_t synced = 0;
  for (const auto& entry : d_live) {
    if (io_msync(entry.first, entry.second.size, k_msync_sync, entry.second.offset) != 0) {
      d_last_error = sys_error(WorkspaceFileErrc::HeaderIoFailed);
      return unexpected(d_last_error);
    }
    ++synced;
  }
  return synced;
}

expected<std::monostate, WorkspaceFileError> WorkspaceFileChunkSource::sync_header() noexcept {
  if (d_header_map != nullptr && io_msync(d_header_map, d_header_bytes, k_msync_sync, 0) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    return unexpected(d_last_error);
  }
  return std::monostate{};
}

std::uint64_t WorkspaceFileChunkSource::root_slot(int ab) const noexcept {
  auto* hdr = static_cast<WorkspaceHeader*>(d_header_map);
  std::uint64_t& slot = ab == 0 ? hdr->root_slot_a : hdr->root_slot_b;
  return std::atomic_ref<std::uint64_t>(slot).load(std::memory_order_acquire);
}

void WorkspaceFileChunkSource::publish_root_slot(int ab, std::uint64_t value) noexcept {
  // The flip is a durability boundary the kill-sweep enumerates: notify the
  // injector around it (a store cannot fail, so its return is ignored).
  if (d_injector != nullptr) {
    d_injector->before(WorkspaceSyscall::RootFlip, 0, 0);
  }
  WorkspaceHeader* hdr = header();
  std::uint64_t* slot = ab == 0 ? &hdr->root_slot_a : &hdr->root_slot_b;
  // Single naturally-aligned 8-byte store: single-sector, torn-write-free.
  std::atomic_ref<std::uint64_t>(*slot).store(value, std::memory_order_release);
  if (d_injector != nullptr) {
    d_injector->after(WorkspaceSyscall::RootFlip, 0, 0);
  }
}

expected<std::monostate, WorkspaceFileError>
WorkspaceFileChunkSource::protect_data(bool read_only) noexcept {
#ifndef NDEBUG
  const int prot = read_only ? k_prot_read : k_prot_readwrite;
  for (const auto& entry : d_live) {
    if (io_mprotect(entry.first, entry.second.size, prot) != 0) {
      d_last_error = sys_error(WorkspaceFileErrc::HeaderIoFailed);
      return unexpected(d_last_error);
    }
  }
#else
  (void)read_only;
#endif
  return std::monostate{};
}

expected<std::monostate, WorkspaceFileError>
WorkspaceFileChunkSource::protect_range(void* addr, std::size_t size, bool read_only) noexcept {
#ifndef NDEBUG
  const auto raw = reinterpret_cast<std::uintptr_t>(addr);
  const std::uintptr_t page_base = raw & ~(static_cast<std::uintptr_t>(d_page) - 1);
  const std::size_t len = align_up((raw - page_base) + size, d_page);
  const int prot = read_only ? k_prot_read : k_prot_readwrite;
  if (io_mprotect(reinterpret_cast<void*>(page_base), len, prot) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    return unexpected(d_last_error);
  }
#else
  (void)addr;
  (void)size;
  (void)read_only;
#endif
  return std::monostate{};
}

#else // !ARBC_HAS_WORKSPACE_FILES

expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
WorkspaceFileChunkSource::create(const std::string&, const WorkspaceLayout&) {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

WorkspaceFileChunkSource::~WorkspaceFileChunkSource() = default;

expected<ChunkSpan, PoolError> WorkspaceFileChunkSource::acquire(std::size_t, std::size_t) {
  d_last_error = WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0};
  return unexpected(PoolError::OutOfMemory);
}

expected<ChunkSpan, PoolError> WorkspaceFileChunkSource::acquire_for(std::uint32_t, std::size_t,
                                                                     std::size_t) {
  d_last_error = WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0};
  return unexpected(PoolError::OutOfMemory);
}

expected<ChunkSpan, PoolError> WorkspaceFileChunkSource::grow(std::size_t, std::size_t,
                                                              std::uint32_t) {
  d_last_error = WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0};
  return unexpected(PoolError::OutOfMemory);
}

expected<std::monostate, WorkspaceFileError> WorkspaceFileChunkSource::adopt_snapshot(int) {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

void WorkspaceFileChunkSource::discard_reopened(const ReopenedChunk&) noexcept {}

void WorkspaceFileChunkSource::release(ChunkSpan) noexcept {}

void WorkspaceFileChunkSource::punch_now(std::uint64_t, std::uint64_t, std::uint32_t) noexcept {}

expected<WorkspaceHeader, WorkspaceFileError>
WorkspaceFileChunkSource::read_header(const std::string&) {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
WorkspaceFileChunkSource::open(const std::string&) {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

expected<std::size_t, WorkspaceFileError> WorkspaceFileChunkSource::sync_data() noexcept {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

expected<std::monostate, WorkspaceFileError> WorkspaceFileChunkSource::sync_header() noexcept {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

std::uint64_t WorkspaceFileChunkSource::root_slot(int) const noexcept { return 0; }
void WorkspaceFileChunkSource::publish_root_slot(int, std::uint64_t) noexcept {}

expected<std::monostate, WorkspaceFileError> WorkspaceFileChunkSource::protect_data(bool) noexcept {
  return std::monostate{};
}

expected<std::monostate, WorkspaceFileError>
WorkspaceFileChunkSource::protect_range(void*, std::size_t, bool) noexcept {
  return std::monostate{};
}

#endif // ARBC_HAS_WORKSPACE_FILES

} // namespace arbc
