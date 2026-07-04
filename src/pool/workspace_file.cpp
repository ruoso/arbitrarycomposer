// fallocate(2) and FALLOC_FL_* require _GNU_SOURCE under glibc; define it
// before any system header is pulled in.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include <arbc/pool/slot_store.hpp> // align_up
#include <arbc/pool/workspace_file.hpp>

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

  if (d_chunk_count >= d_max_chunks) {
    d_last_error = WorkspaceFileError{WorkspaceFileErrc::DirectoryFull, 0};
    return unexpected(PoolError::OutOfMemory);
  }

  const std::size_t rounded = align_up(size == 0 ? 1 : size, d_page);
  const std::uint64_t offset = d_next_offset;
  const std::uint64_t new_end = offset + rounded;

  // ftruncate extends the file by whole chunks (disk-full surfaces here as a
  // value, never an abort).
  if (::ftruncate(d_fd, static_cast<off_t>(new_end)) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
    return unexpected(PoolError::OutOfMemory);
  }
  void* base = ::mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_SHARED, d_fd,
                      static_cast<off_t>(offset));
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

#if defined(__linux__)
  // Return storage: hole-punch the chunk's file range while keeping the logical
  // size, so both memory and disk come back (fixing the grow-forever high-water
  // mark). Non-fatal if the filesystem lacks punch support.
  if (::fallocate(d_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                  static_cast<off_t>(chunk.offset), static_cast<off_t>(chunk.size)) != 0) {
    d_last_error = sys_error(WorkspaceFileErrc::GrowFailed);
  }
#endif

  directory()[chunk.index].state = k_workspace_chunk_free;
  d_live.erase(it);
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

expected<WorkspaceHeader, WorkspaceFileError>
WorkspaceFileChunkSource::read_header(const std::string&) {
  return unexpected(WorkspaceFileError{WorkspaceFileErrc::Unsupported, 0});
}

#endif // ARBC_HAS_WORKSPACE_FILES

} // namespace arbc
