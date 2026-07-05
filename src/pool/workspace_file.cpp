// fallocate(2) and FALLOC_FL_* require _GNU_SOURCE under glibc; define it
// before any system header is pulled in.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <arbc/pool/slot_store.hpp> // align_up
#include <arbc/pool/workspace_file.hpp>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if ARBC_HAS_WORKSPACE_FILES
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#if defined(__linux__)
#include <linux/falloc.h>
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

#if ARBC_HAS_WORKSPACE_FILES

namespace {

WorkspaceFileError sys_error(WorkspaceFileErrc code) { return WorkspaceFileError{code, errno}; }

} // namespace

// --- fault-injection shims (pool.crash_tests) --------------------------------
// When no injector is installed every shim is a single predictable branch over
// the real syscall. When installed, `before` may inject an errno (skipping the
// real call) or snapshot/kill the process; `after` observes the durable effect.

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

expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
WorkspaceFileChunkSource::create(const std::string& path, const WorkspaceLayout& layout) {
  const long page_query = ::sysconf(_SC_PAGESIZE);
  const std::size_t page = page_query > 0 ? static_cast<std::size_t>(page_query) : 4096;

  const std::uint32_t max_chunks =
      layout.max_chunks == 0 ? k_workspace_default_max_chunks : layout.max_chunks;
  const std::size_t directory_bytes =
      static_cast<std::size_t>(max_chunks) * sizeof(WorkspaceChunkEntry);
  const std::size_t header_bytes = align_up(sizeof(WorkspaceHeader) + directory_bytes, page);

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

  // Zero only the fixed header; the directory region reads as zeros from the
  // ftruncate hole until acquire() writes entries, keeping the file sparse.
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

  auto source = std::unique_ptr<WorkspaceFileChunkSource>(new WorkspaceFileChunkSource());
  source->d_path = path;
  source->d_fd = fd;
  source->d_page = page;
  source->d_max_chunks = max_chunks;
  source->d_data_offset = header_bytes;
  source->d_next_offset = header_bytes;
  source->d_chunk_count = 0;
  source->d_header_map = map;
  source->d_header_bytes = header_bytes;
  return source;
}

WorkspaceFileChunkSource::~WorkspaceFileChunkSource() {
  for (const auto& entry : d_live) {
    ::munmap(entry.first, entry.second.size);
  }
  // Remapped-on-open chunks never pulled back through acquire.
  for (std::size_t i = d_reopened_cursor; i < d_reopened.size(); ++i) {
    ::munmap(d_reopened[i].base, d_reopened[i].size);
  }
  if (d_header_map != nullptr) {
    ::munmap(d_header_map, d_header_bytes);
  }
  if (d_fd >= 0) {
    ::close(d_fd);
  }
}

expected<ChunkSpan, PoolError> WorkspaceFileChunkSource::acquire(std::size_t size,
                                                                 std::size_t alignment) {
  // mmap hands back page-aligned addresses; every slot alignment we support is
  // far smaller, matching the AnonymousChunkSource contract.
  assert(alignment <= d_page);
  (void)alignment;

  // Recovery path: hand back an already-mapped file chunk (in directory order)
  // rather than growing the file. reserve_restored drives this to re-bind the
  // records the walk reads.
  if (d_reopened_cursor < d_reopened.size()) {
    const ReopenedChunk& rc = d_reopened[d_reopened_cursor++];
    d_live.emplace(rc.base, LiveChunk{rc.offset, rc.size, rc.index});
    return ChunkSpan{rc.base, rc.size};
  }

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
  void* base =
      io_mmap(rounded, PROT_READ | PROT_WRITE, MAP_SHARED, static_cast<std::int64_t>(offset));
  if (base == MAP_FAILED) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
    return unexpected(PoolError::OutOfMemory);
  }

  const std::uint32_t index = d_chunk_count;
  WorkspaceChunkEntry& slot = directory()[index];
  slot.offset = offset;
  slot.size = rounded;
  slot.state = k_workspace_chunk_live;
  slot.reserved = 0;

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

  ::munmap(span.base, chunk.size);
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
#if defined(__linux__)
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

expected<WorkspaceHeader, WorkspaceFileError>
WorkspaceFileChunkSource::read_header(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }
  WorkspaceHeader hdr{};
  const ssize_t got = ::pread(fd, &hdr, sizeof(hdr), 0);
  ::close(fd);
  if (got != static_cast<ssize_t>(sizeof(hdr))) {
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, got < 0 ? errno : 0});
  }
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
  const int fd = ::open(path.c_str(), O_RDWR);
  if (fd < 0) {
    return unexpected(sys_error(WorkspaceFileErrc::OpenFailed));
  }

  // Read the fixed header first to learn page size / directory capacity, then
  // map the whole header+directory region.
  WorkspaceHeader fixed{};
  const ssize_t got = ::pread(fd, &fixed, sizeof(fixed), 0);
  if (got != static_cast<ssize_t>(sizeof(fixed))) {
    WorkspaceFileError err{WorkspaceFileErrc::HeaderIoFailed, got < 0 ? errno : 0};
    ::close(fd);
    return unexpected(err);
  }
  if (fixed.magic != k_workspace_magic) {
    ::close(fd);
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::BadMagic, 0});
  }
  if (fixed.format_major != k_workspace_format_major) {
    ::close(fd);
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::UnsupportedFormat, 0});
  }

  const std::size_t header_bytes = static_cast<std::size_t>(fixed.data_offset);

  // A truncated (short) workspace file must surface as a value, never map past
  // EOF and SIGBUS on first access (pool.crash_tests short-file paths). Refuse
  // any file too small for its own header/directory region up front.
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::close(fd);
    return unexpected(err); // GCOV_EXCL_LINE: fstat on a freshly-opened fd does not fail
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(st.st_size);
  if (file_size < header_bytes) {
    ::close(fd);
    return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, 0});
  }

  void* map = ::mmap(nullptr, header_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (map == MAP_FAILED) {
    WorkspaceFileError err = sys_error(WorkspaceFileErrc::HeaderIoFailed);
    ::close(fd);
    return unexpected(err);
  }

  auto source = std::unique_ptr<WorkspaceFileChunkSource>(new WorkspaceFileChunkSource());
  source->d_path = path;
  source->d_fd = fd;
  source->d_page = fixed.page_size != 0 ? fixed.page_size : 4096;
  source->d_max_chunks = fixed.max_chunks;
  source->d_data_offset = fixed.data_offset;
  source->d_chunk_count = static_cast<std::uint32_t>(fixed.chunk_count);
  source->d_header_map = map;
  source->d_header_bytes = header_bytes;

  // Remap every live data chunk in directory order and queue it for acquire.
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
    if (entry.offset + entry.size > file_size) {
      // Live chunk runs past EOF: the file was truncated mid-data-chunk. Refuse
      // rather than map past EOF (SIGBUS on access). `source`'s destructor drops
      // the header map, the chunks mapped so far, and the fd.
      return unexpected(WorkspaceFileError{WorkspaceFileErrc::HeaderIoFailed, 0});
    }
    void* base = ::mmap(nullptr, entry.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        static_cast<off_t>(entry.offset));
    if (base == MAP_FAILED) {
      return unexpected(sys_error(WorkspaceFileErrc::HeaderIoFailed));
    }
    source->d_reopened.push_back(ReopenedChunk{base, entry.offset, entry.size, i});
  }
  source->d_next_offset = next_offset;
  return source;
}

expected<std::size_t, WorkspaceFileError> WorkspaceFileChunkSource::sync_data() noexcept {
  std::size_t synced = 0;
  for (const auto& entry : d_live) {
    if (io_msync(entry.first, entry.second.size, MS_SYNC, entry.second.offset) != 0) {
      d_last_error = sys_error(WorkspaceFileErrc::HeaderIoFailed);
      return unexpected(d_last_error);
    }
    ++synced;
  }
  return synced;
}

expected<std::monostate, WorkspaceFileError> WorkspaceFileChunkSource::sync_header() noexcept {
  if (d_header_map != nullptr && io_msync(d_header_map, d_header_bytes, MS_SYNC, 0) != 0) {
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
  const int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
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
  const int prot = read_only ? PROT_READ : (PROT_READ | PROT_WRITE);
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
