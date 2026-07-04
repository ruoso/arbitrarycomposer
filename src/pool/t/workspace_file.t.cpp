#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// The file-backed source is POSIX-only; on other platforms it compiles out and
// callers fall back to anonymous backing. Everything below the capability gate
// is guarded so non-POSIX builds stay green with just the runtime-query check.
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

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace {

// A self-cleaning unique workspace-file path under the temp dir.
class TempPath {
public:
  TempPath() {
    char tmpl[] = "/tmp/arbc_ws_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd); // create() reopens with O_TRUNC; we only needed a unique name
    }
    d_path = tmpl;
  }
  ~TempPath() { ::unlink(d_path.c_str()); }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;

  const std::string& str() const { return d_path; }

private:
  std::string d_path;
};

blkcnt_t block_count(const std::string& path) {
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return st.st_blocks;
}

off_t file_size(const std::string& path) {
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return st.st_size;
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
    const int fd = ::open(path.str().c_str(), O_RDWR | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    // Large enough to fill a full header read; zeroed magic is not ours.
    char junk[sizeof(arbc::WorkspaceHeader) + 32] = {0};
    REQUIRE(::write(fd, junk, sizeof(junk)) == static_cast<ssize_t>(sizeof(junk)));
    ::close(fd);
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
  const off_t size_after_first_chunk = file_size(path.str());

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
#if defined(__linux__)
  TempPath path;
  auto source = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(source.has_value());
  arbc::WorkspaceFileChunkSource& ws = **source;

  constexpr std::size_t chunk = 256 * 1024; // several pages so st_blocks is clear
  std::vector<arbc::ChunkSpan> chunks;
  for (int i = 0; i < 8; ++i) {
    auto span = ws.acquire(chunk, alignof(std::max_align_t));
    REQUIRE(span.has_value());
    // Dirty every page, then flush so blocks are actually allocated (some
    // filesystems delay allocation until writeback).
    std::memset(span->base, 0xAB, span->size);
    REQUIRE(::msync(span->base, span->size, MS_SYNC) == 0);
    chunks.push_back(*span);
  }
  const blkcnt_t before = block_count(path.str());

  for (arbc::ChunkSpan span : chunks) {
    ws.release(span); // hole-punches on Linux
  }
  const blkcnt_t after = block_count(path.str());

  // Storage came back: the punched data chunks dominate the tiny header.
  REQUIRE(after < before);
  const blkcnt_t reclaimed = before - after;
  const blkcnt_t expected_blocks = static_cast<blkcnt_t>(8 * chunk / 512);
  REQUIRE(reclaimed >= expected_blocks / 2);
#else
  SUCCEED("hole-punch storage return is a Linux-only guarantee");
#endif
}

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

TEST_CASE("position-independence debug hook accepts index-only records") {
  // A record made only of small integer indices holds no pointer into the map.
  std::uint32_t record[4] = {0, 1, 2, 3};
  char mapping[4096] = {0};
  arbc::debug_assert_position_independent(record, sizeof(record), mapping, sizeof(mapping));
  SUCCEED("no smuggled pointer into the mapping");
}

#endif // ARBC_HAS_WORKSPACE_FILES
