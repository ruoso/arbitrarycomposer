#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The file-backed source is available on POSIX (mmap) and Windows (MapViewOfFile);
// on any other platform it compiles out and callers fall back to anonymous backing.
// Everything below the capability gate is guarded so unsupported builds stay green
// with just the runtime-query check, and the lifecycle body carries a Windows arm
// alongside the POSIX one.
TEST_CASE("workspace-file support is reported honestly") {
#if ARBC_HAS_WORKSPACE_FILES
  REQUIRE(arbc::workspace_files_supported());
#else
  REQUIRE_FALSE(arbc::workspace_files_supported());
  auto source = arbc::WorkspaceFileChunkSource::create("unused");
  REQUIRE_FALSE(source.has_value());
  REQUIRE(source.error().code == arbc::WorkspaceFileErrc::Unsupported);
#endif
}

#if ARBC_HAS_WORKSPACE_FILES

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace {

// A self-cleaning unique workspace-file path under the temp dir.
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    // GetTempFileNameA creates the file; create()/open() reopen with CREATE_ALWAYS.
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "arb", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_ws_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd); // create() reopens with O_TRUNC; we only needed a unique name
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
#if defined(_WIN32)
    ::DeleteFileA(d_path.c_str());
#else
    ::unlink(d_path.c_str());
#endif
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;

  const std::string& str() const { return d_path; }

private:
  std::string d_path;
};

// On-disk allocated size in bytes: st_blocks*512 on POSIX, GetCompressedFileSize on
// Windows (both drop when a sparse chunk is hole-punched).
std::uint64_t allocated_size(const std::string& path) {
#if defined(_WIN32)
  DWORD high = 0;
  const DWORD low = ::GetCompressedFileSizeA(path.c_str(), &high);
  REQUIRE(low != INVALID_FILE_SIZE);
  return (static_cast<std::uint64_t>(high) << 32) | low;
#else
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return static_cast<std::uint64_t>(st.st_blocks) * 512u;
#endif
}

// Logical file size in bytes.
std::uint64_t file_size(const std::string& path) {
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA fad{};
  REQUIRE(::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad) != 0);
  return (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
#else
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return static_cast<std::uint64_t>(st.st_size);
#endif
}

} // namespace

TEST_CASE("fresh workspace create/grow/release lifecycle") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;

  std::vector<arbc::ChunkSpan> chunks;
  for (int i = 0; i < 3; ++i) {
    auto span = ws.acquire(64 * 1024, alignof(std::max_align_t));
    REQUIRE(span.has_value());
    REQUIRE(span->base != nullptr);
    REQUIRE(span->size >= 64 * 1024);
    chunks.push_back(*span);
  }
  REQUIRE(ws.chunk_count() == 3);

  for (arbc::ChunkSpan span : chunks) {
    ws.release(span);
  }
  // The file keeps its logical size (hole-punch KEEP_SIZE), and no crash: the
  // released chunks' directory entries are cleared.
  REQUIRE(ws.chunk_count() == 3);
}

// enforces: 15-memory-model#chunk-growth-preserves-addresses
TEST_CASE("file-backed chunk contents survive remap-free growth") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;

  auto first = ws.acquire(64 * 1024, alignof(std::uint64_t));
  REQUIRE(first.has_value());
  auto* sentinel = static_cast<std::uint64_t*>(first->base);
  *sentinel = 0xFEED'BEEF'CAFE'0001ull;

  // Append many more chunks — each is its own mapping, so the first never moves.
  std::vector<arbc::ChunkSpan> more;
  for (int i = 0; i < 32; ++i) {
    auto span = ws.acquire(64 * 1024, alignof(std::uint64_t));
    REQUIRE(span.has_value());
    more.push_back(*span);
  }

  REQUIRE(first->base == static_cast<void*>(sentinel));
  REQUIRE(*sentinel == 0xFEED'BEEF'CAFE'0001ull);
}

TEST_CASE("workspace header round-trips through a read-only reopen") {
  TempPath path;
  arbc::WorkspaceLayout layout;
  layout.max_chunks = 128;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str(), layout);
  REQUIRE(source.has_value());

  auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(header.has_value());
  REQUIRE(header->magic == arbc::k_workspace_magic);
  REQUIRE(header->format_major == arbc::k_workspace_format_major);
  REQUIRE(header->format_minor == arbc::k_workspace_format_minor);
  REQUIRE(header->page_size > 0);
  REQUIRE(header->max_chunks == 128);
  REQUIRE(header->data_offset % header->page_size == 0);
  // A/B root slots are reserved for pool.checkpoints and written zero now.
  REQUIRE(header->root_slot_a == 0);
  REQUIRE(header->root_slot_b == 0);
}

TEST_CASE("read_header rejects a non-workspace file") {
  TempPath path;
  {
    // Large enough to fill a full header read; zeroed magic is not ours.
    char junk[sizeof(arbc::WorkspaceHeader) + 32] = {0};
#if defined(_WIN32)
    HANDLE h = ::CreateFileA(path.str().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    DWORD wrote = 0;
    REQUIRE(::WriteFile(h, junk, sizeof(junk), &wrote, nullptr) != 0);
    REQUIRE(wrote == sizeof(junk));
    ::CloseHandle(h);
#else
    const int fd = ::open(path.str().c_str(), O_RDWR | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, junk, sizeof(junk)) == static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);
#endif
  }
  auto header = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE_FALSE(header.has_value());
  REQUIRE(header.error().code == arbc::WorkspaceFileErrc::BadMagic);
}

// Bookkeeping stays anonymous, verified structurally: with the store wired to
// the file source for data, pure free-list traffic (allocate/release within
// already-backed chunks) never routes through the ChunkSource, so the workspace
// file does not grow.
TEST_CASE("free-list traffic never grows the workspace file") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::Arena arena(**source);
  // Four slots per chunk so one chunk is backed quickly.
  arbc::SlotStore& store = arena.store_for(64, alignof(std::max_align_t), /*chunk_bits=*/2);

  std::vector<arbc::SlotIndex> live;
  for (int i = 0; i < 4; ++i) {
    auto slot = store.allocate();
    REQUIRE(slot.has_value());
    live.push_back(*slot);
  }
  REQUIRE((*source)->chunk_count() == 1);
  const std::uint64_t size_after_first_chunk = file_size(path.str());

  // Churn the free list hard: release and re-allocate within the backed chunk.
  for (int round = 0; round < 1000; ++round) {
    store.release(live.back());
    live.pop_back();
    auto slot = store.allocate();
    REQUIRE(slot.has_value());
    live.push_back(*slot);
  }

  // No new chunk was acquired and the file did not grow — bookkeeping is anon.
  REQUIRE((*source)->chunk_count() == 1);
  REQUIRE(file_size(path.str()) == size_after_first_chunk);
}

// enforces: 15-memory-model#hole-punch-returns-storage
TEST_CASE("releasing file-backed chunks returns allocated storage") {
#if defined(__linux__) || defined(_WIN32)
  // Sparse-file hole-punch reclaims on-disk storage: Linux `fallocate(PUNCH_HOLE)`
  // and Windows `FSCTL_SET_ZERO_DATA` on an NTFS sparse file. Guarded to those
  // sparse-capable volumes, as macOS / non-NTFS keeps the logical bytes.
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;

  constexpr std::size_t chunk = 256 * 1024; // several pages so allocated size is clear
  std::vector<arbc::ChunkSpan> chunks;
  for (int i = 0; i < 8; ++i) {
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    REQUIRE(span.has_value());
    // Dirty every page, then flush so blocks are actually allocated (some
    // filesystems delay allocation until writeback).
    std::memset(span->base, 0xAB, span->size);
#if defined(_WIN32)
    REQUIRE(::FlushViewOfFile(span->base, span->size) != 0);
#else
    REQUIRE(::msync(span->base, span->size, MS_SYNC) == 0);
#endif
    chunks.push_back(*span);
  }
  const std::uint64_t before = allocated_size(path.str());

  for (arbc::ChunkSpan span : chunks) {
    ws.release(span); // hole-punches on a sparse-capable volume
  }
  const std::uint64_t after = allocated_size(path.str());

  // Storage came back: the punched data chunks dominate the tiny header.
  REQUIRE(after < before);
  const std::uint64_t reclaimed = before - after;
  const std::uint64_t expected_bytes = static_cast<std::uint64_t>(8) * chunk;
  REQUIRE(reclaimed >= expected_bytes / 2);
#else
  SUCCEED("hole-punch storage return is a sparse-file (Linux/Windows) guarantee");
#endif
}

// enforces: 15-memory-model#workspace-io-faults-surface-as-values
#if !defined(_WIN32)
TEST_CASE("disk-full growth surfaces an error, never an abort") {
  // RLIMIT_FSIZE caps file size; ftruncate past it fails with EFBIG. The default
  // SIGXFSZ would kill us, so ignore it for the duration of the test.
  struct sigaction old_action{};
  struct sigaction ignore{};
  ignore.sa_handler = SIG_IGN;
  REQUIRE(::sigaction(SIGXFSZ, &ignore, &old_action) == 0);

  struct rlimit old_limit{};
  REQUIRE(::getrlimit(RLIMIT_FSIZE, &old_limit) == 0);

  TempPath path;
  arbc::WorkspaceLayout layout;
  layout.max_chunks = 8; // tiny header so it fits comfortably under the cap
  auto source = arbc::WorkspaceFileChunkSource::create(path.str(), layout);
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;

  constexpr std::size_t chunk = 64 * 1024;
  const off_t header_bytes = file_size(path.str());

  struct rlimit small = old_limit;
  small.rlim_cur = static_cast<rlim_t>(header_bytes) + chunk + 4096; // room for one chunk
  REQUIRE(::setrlimit(RLIMIT_FSIZE, &small) == 0);

  auto first = ws.acquire(chunk, alignof(std::max_align_t));
  REQUIRE(first.has_value()); // one chunk fits under the cap

  auto second = ws.acquire(chunk, alignof(std::max_align_t)); // second exceeds it
  REQUIRE_FALSE(second.has_value());
  REQUIRE(second.error() == arbc::PoolError::OutOfMemory);
  REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
  REQUIRE(ws.last_error().sys_errno == EFBIG);

  REQUIRE(::setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
  REQUIRE(::sigaction(SIGXFSZ, &old_action, nullptr) == 0);
}
#else  // _WIN32: no RLIMIT_FSIZE — inject the growth failure via the syscall seam.
namespace {
// Injects a Win32 error at a chosen syscall (doc 16's sanctioned mocking seam),
// the platform-neutral substitute for POSIX's real RLIMIT disk-full.
class WinErrorInjector final : public arbc::SyscallInjector {
public:
  WinErrorInjector(arbc::WorkspaceSyscall target, int err) : d_target(target), d_err(err) {}
  int before(arbc::WorkspaceSyscall kind, std::uint64_t, std::size_t) noexcept override {
    if (kind == d_target) {
      d_fired = true;
      return d_err;
    }
    return 0;
  }
  void after(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {}
  bool fired() const { return d_fired; }

private:
  arbc::WorkspaceSyscall d_target;
  int d_err;
  bool d_fired{false};
};
} // namespace

TEST_CASE("disk-full growth surfaces an error via the injector, never an abort") {
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;

  WinErrorInjector inj(arbc::WorkspaceSyscall::Ftruncate, ERROR_DISK_FULL);
  ws.set_syscall_injector(&inj);
  auto span = ws.acquire(64 * 1024, alignof(std::max_align_t));
  ws.set_syscall_injector(nullptr);

  REQUIRE(inj.fired());
  REQUIRE_FALSE(span.has_value()); // a value, never an abort
  REQUIRE(span.error() == arbc::PoolError::OutOfMemory);
  REQUIRE(ws.last_error().code == arbc::WorkspaceFileErrc::GrowFailed);
  REQUIRE(ws.last_error().sys_errno == ERROR_DISK_FULL); // Win32 GetLastError code
}
#endif // _WIN32

TEST_CASE("position-independence debug hook accepts index-only records") {
  // A record made only of small integer indices holds no pointer into the map.
  std::uint32_t record[4] = {0, 1, 2, 3};
  char mapping[4096] = {0};
  arbc::debug_assert_position_independent(record, sizeof(record), mapping, sizeof(mapping));
  SUCCEED("no smuggled pointer into the mapping");
}

#endif // ARBC_HAS_WORKSPACE_FILES
