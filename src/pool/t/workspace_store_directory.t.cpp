#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/chunk_source.hpp>
#include <arbc/pool/slot_store.hpp>
#include <arbc/pool/workspace_file.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The arena directory (doc 15): one workspace file backs SEVERAL size-class stores,
// and the file must say which chunk belongs to which store. These cases pin the
// on-disk store table, the per-store chunk routing on reopen, and the refusals that
// keep a disagreeing file from mis-routing. On platforms without workspace files the
// body compiles out with just the runtime-query check.
TEST_CASE("store-directory support tracks workspace-file support") {
  REQUIRE(arbc::workspace_files_supported() == (ARBC_HAS_WORKSPACE_FILES != 0));
}

#if ARBC_HAS_WORKSPACE_FILES

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <atomic>
#include <thread>

namespace {

// A self-cleaning unique workspace-file path under the temp dir (mirrors the
// checkpoint / crash_tests / workspace_file tests).
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "dir", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_storedir_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
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

// THE COLLISION THIS TASK EXISTS TO SURVIVE, forced explicitly so it bites in every
// lane (it is the ASan/debug lane's natural geometry): the model's HamtNode lands on
// a 288-byte stride at 128 slots/chunk, its ObjectRecord on a 144-byte stride at 256
// slots/chunk -- and BOTH produce a 36864-byte chunk. `chunk_bytes = chunk_slots *
// slot_stride` with a power-of-two `chunk_slots` is not monotone in stride, so chunk
// byte size is simply not a store identity. Nothing but the directory's owner tag can
// tell these two stores' chunks apart.
constexpr std::size_t k_align = alignof(std::max_align_t);
constexpr std::size_t k_node_stride = 288;
constexpr std::uint32_t k_node_bits = 7; // 128 slots/chunk
constexpr std::size_t k_record_stride = 144;
constexpr std::uint32_t k_record_bits = 8; // 256 slots/chunk

static_assert(k_node_stride % k_align == 0 && k_record_stride % k_align == 0,
              "both strides must already be a multiple of the store alignment, or store_for "
              "would round them and the two size classes would not be the ones under test");
static_assert((k_node_stride << k_node_bits) == (k_record_stride << k_record_bits),
              "the two size classes MUST collide on chunk BYTE size -- that collision is the "
              "bug under test, so a geometry change that dissolves it makes this file a "
              "regression test for nothing");

// A deterministic byte pattern for slot `slot` of the store tagged `tag`. The tag is
// in the pattern, so a mis-routed chunk yields the OTHER store's bytes and fails
// loudly rather than merely reading garbage.
std::uint8_t pattern_byte(std::uint32_t tag, std::uint32_t slot, std::size_t offset) {
  return static_cast<std::uint8_t>((tag * 131u) ^ (slot * 17u) ^ (offset * 7u));
}

void fill_slot(void* base, std::size_t stride, std::uint32_t tag, std::uint32_t slot) {
  auto* bytes = static_cast<std::uint8_t*>(base);
  for (std::size_t i = 0; i < stride; ++i) {
    bytes[i] = pattern_byte(tag, slot, i);
  }
}

bool slot_matches(const void* base, std::size_t stride, std::uint32_t tag, std::uint32_t slot) {
  const auto* bytes = static_cast<const std::uint8_t*>(base);
  for (std::size_t i = 0; i < stride; ++i) {
    if (bytes[i] != pattern_byte(tag, slot, i)) {
      return false;
    }
  }
  return true;
}

// Byte-copy the workspace file so recovery runs against an INDEPENDENT file (its own
// fd + mappings) whose bytes are the ones the last commit msync'd -- exactly what a
// post-crash reopen sees. Mirrors the landed checkpoint.t.cpp idiom.
void copy_file(const std::string& src, const std::string& dst) {
#if defined(_WIN32)
  REQUIRE(::CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0);
#else
  const int in = ::open(src.c_str(), O_RDONLY);
  REQUIRE(in >= 0);
  const int out = ::open(dst.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  REQUIRE(out >= 0);
  char buf[65536];
  ssize_t n = 0;
  while ((n = ::read(in, buf, sizeof(buf))) > 0) {
    ssize_t off = 0;
    while (off < n) {
      const ssize_t w = ::write(out, buf + off, static_cast<std::size_t>(n - off));
      REQUIRE(w > 0);
      off += w;
    }
  }
  REQUIRE(n == 0); // clean EOF
  ::close(in);
  ::close(out);
#endif
}

// Read `size` bytes at `offset` straight out of the file -- the on-disk truth, read
// without going through the source's mapping.
std::vector<std::uint8_t> read_bytes(const std::string& path, std::uint64_t offset,
                                     std::size_t size) {
  std::vector<std::uint8_t> out(size, 0);
#if defined(_WIN32)
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(offset);
  const BOOL sought = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
  DWORD got = 0;
  const BOOL ok = sought != 0 && ::ReadFile(h, out.data(), static_cast<DWORD>(size), &got, nullptr);
  ::CloseHandle(h);
  REQUIRE(ok != 0);
  REQUIRE(got == size);
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  REQUIRE(fd >= 0);
  const ssize_t got = ::pread(fd, out.data(), size, static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(got == static_cast<ssize_t>(size));
#endif
  return out;
}

// Overwrite `size` bytes at `offset` -- the corruption surgery primitive.
void write_at(const std::string& path, std::uint64_t offset, const void* data, std::size_t size) {
#if defined(_WIN32)
  HANDLE h = ::CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  REQUIRE(h != INVALID_HANDLE_VALUE);
  LARGE_INTEGER li;
  li.QuadPart = static_cast<LONGLONG>(offset);
  const BOOL sought = ::SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
  DWORD wrote = 0;
  const BOOL ok = sought != 0 && ::WriteFile(h, data, static_cast<DWORD>(size), &wrote, nullptr);
  ::CloseHandle(h);
  REQUIRE(ok != 0);
  REQUIRE(wrote == size);
#else
  const int fd = ::open(path.c_str(), O_RDWR);
  REQUIRE(fd >= 0);
  const ssize_t wrote = ::pwrite(fd, data, size, static_cast<off_t>(offset));
  ::close(fd);
  REQUIRE(wrote == static_cast<ssize_t>(size));
#endif
}

// On-disk ALLOCATED size (not logical length): drops when a sparse chunk is
// hole-punched. Mirrors the sibling workspace_file / checkpoint helper.
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

// The file's chunk directory, read from disk. `owner` is the routing tag under test.
std::vector<arbc::WorkspaceChunkEntry> read_directory(const std::string& path,
                                                      std::uint32_t count) {
  const std::vector<std::uint8_t> raw =
      read_bytes(path, sizeof(arbc::WorkspaceHeader),
                 static_cast<std::size_t>(count) * sizeof(arbc::WorkspaceChunkEntry));
  std::vector<arbc::WorkspaceChunkEntry> entries(count);
  std::memcpy(entries.data(), raw.data(), raw.size());
  return entries;
}

// File offset of store-table snapshot `ab` (0 = A, 1 = B). The snapshots are one PAGE
// apart -- each is page-resident so its generation stamp shares a writeback unit with
// the rows it stamps (pool.header_writeback_ordering).
std::uint64_t snapshot_offset(const arbc::WorkspaceHeader& hdr, int ab) {
  return hdr.store_table_offset + static_cast<std::uint64_t>(ab) * hdr.page_size;
}

// File offset of row `row` of snapshot `ab`: the rows follow the stamp inside the page.
std::uint64_t store_row_offset(const arbc::WorkspaceHeader& hdr, int ab, std::uint32_t row) {
  return snapshot_offset(hdr, ab) + sizeof(arbc::WorkspaceStoreSnapshot) +
         static_cast<std::uint64_t>(row) * sizeof(arbc::WorkspaceStoreEntry);
}

// One store-table row, read from disk. `ab` selects the snapshot (0 = A, 1 = B).
arbc::WorkspaceStoreEntry read_store_row(const std::string& path, const arbc::WorkspaceHeader& hdr,
                                         int ab, std::uint32_t row) {
  const std::vector<std::uint8_t> raw =
      read_bytes(path, store_row_offset(hdr, ab, row), sizeof(arbc::WorkspaceStoreEntry));
  arbc::WorkspaceStoreEntry entry{};
  std::memcpy(&entry, raw.data(), sizeof(entry));
  return entry;
}

// Snapshot `ab`'s generation stamp, read from disk: the generation of the root slot
// the commit that wrote this snapshot flipped.
std::uint32_t read_snapshot_stamp(const std::string& path, const arbc::WorkspaceHeader& hdr,
                                  int ab) {
  const std::vector<std::uint8_t> raw =
      read_bytes(path, snapshot_offset(hdr, ab), sizeof(arbc::WorkspaceStoreSnapshot));
  arbc::WorkspaceStoreSnapshot snap{};
  std::memcpy(&snap, raw.data(), sizeof(snap));
  return snap.generation;
}

} // namespace

// enforces: 15-memory-model#reopen-routes-chunks-to-owning-store
TEST_CASE("two colliding-chunk-size stores reopen to exactly their own chunks", "[pool]") {
  // THE REGRESSION. Two stores whose chunks are byte-for-byte the same SIZE allocate
  // INTERLEAVED into one workspace file. Reopen must hand each store back its own
  // chunks. The pre-task code drained one size-blind, owner-blind FIFO, so the node
  // store would have claimed the record store's first chunk and read records as
  // nodes; a size-based route (candidate B) cannot save it either, because both
  // chunks are 36864 bytes.
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;

  // The routed arena: every size-class store is handed its own per-store facade.
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
  arbc::SlotStore& records = arena.store_for(k_record_stride, k_align, k_record_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);
  ckpt.register_store(records);

  // The two stores mint their chunks in an interleaved directory order: node chunk 0,
  // record chunk 0, node chunk 1, node chunk 2, record chunk 1. No contiguous
  // per-store run, so a FIFO reopen mis-routes and a size-keyed reopen cannot decide.
  constexpr std::uint32_t k_count = 300;
  for (std::uint32_t i = 0; i < k_count; ++i) {
    const arbc::SlotIndex n = *nodes.allocate();
    fill_slot(nodes.resolve(n), k_node_stride, 1, n);
    const arbc::SlotIndex r = *records.allocate();
    fill_slot(records.resolve(r), k_record_stride, 2, r);
  }
  REQUIRE(nodes.high_water() == k_count);   // 300 slots / 128 -> 3 chunks
  REQUIRE(records.high_water() == k_count); // 300 slots / 256 -> 2 chunks
  REQUIRE(ws.chunk_count() == 5);
  REQUIRE(ckpt.commit(0).has_value());

  // The arena directory tags each chunk with its owner -- node = row 0 (bound first),
  // record = row 1 -- and the tags are genuinely interleaved.
  const std::vector<arbc::WorkspaceChunkEntry> dir = read_directory(path.str(), 5);
  const std::vector<std::uint32_t> owners{dir[0].owner, dir[1].owner, dir[2].owner, dir[3].owner,
                                          dir[4].owner};
  REQUIRE(owners == std::vector<std::uint32_t>{0, 1, 0, 0, 1});
  // ... and the two stores' chunks really are the same byte size, so the tag is the
  // only thing that distinguishes them.
  REQUIRE(dir[0].size == dir[1].size);

  // Recovery against an independent copy (the writer stays alive: its teardown would
  // hole-punch the very chunks a crash would have left behind).
  TempPath recovered;
  copy_file(path.str(), recovered.str());
  auto opened = arbc::Checkpointer::open(recovered.str());
  REQUIRE(opened.has_value());
  REQUIRE(opened->valid);

  arbc::WorkspaceFileChunkSource& ws2 = *opened->source;
  arbc::Arena arena2(ws2.router());
  arbc::Checkpointer ckpt2(ws2, arena2);

  const std::uint64_t chunks_before = ws2.chunk_count();
  REQUIRE(ckpt2.reserve_restored_all(arena2).has_value());
  // Behavioral counter: restoring an N-store file re-binds already-mapped chunks and
  // grows the file ZERO times.
  REQUIRE(ws2.chunk_count() == chunks_before);

  arbc::SlotStore& nodes2 = arena2.store_for(k_node_stride, k_align, k_node_bits);
  arbc::SlotStore& records2 = arena2.store_for(k_record_stride, k_align, k_record_bits);
  REQUIRE(nodes2.high_water() == k_count);   // the high-water came off the store table
  REQUIRE(records2.high_water() == k_count); // -- nothing was passed in by the caller

  // Every record resolves to exactly the bytes it had before the crash. A mis-route
  // would surface here as the other store's tag in the pattern.
  for (std::uint32_t i = 0; i < k_count; ++i) {
    REQUIRE(slot_matches(nodes2.resolve(i), k_node_stride, 1, i));
    REQUIRE(slot_matches(records2.resolve(i), k_record_stride, 2, i));
  }
}

TEST_CASE("store_view appends one row to both snapshots and is idempotent per size class") {
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;

  auto nodes = ws.store_view(k_node_stride, k_align, 1u << k_node_bits);
  REQUIRE(nodes.has_value());
  // A second bind of the same size class returns the SAME facade -- one view per
  // store, so two typed views over one size class cannot split its chunks.
  auto again = ws.store_view(k_node_stride, k_align, 1u << k_node_bits);
  REQUIRE(again.has_value());
  REQUIRE(*again == *nodes);

  auto records = ws.store_view(k_record_stride, k_align, 1u << k_record_bits);
  REQUIRE(records.has_value());
  REQUIRE(*records != *nodes);

  REQUIRE(ws.store_id(k_node_stride, k_align) == 0);
  REQUIRE(ws.store_id(k_record_stride, k_align) == 1);
  REQUIRE(ws.store_id(999, k_align) == arbc::k_workspace_no_owner);

  // The identity columns are per-STORE, not per-root, so a bind writes them into
  // BOTH snapshots; only `high_water` differs per snapshot, and it starts at zero.
  const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(hdr.has_value());
  for (int ab = 0; ab < 2; ++ab) {
    const arbc::WorkspaceStoreEntry row0 = read_store_row(path.str(), *hdr, ab, 0);
    REQUIRE(row0.slot_stride == k_node_stride);
    REQUIRE(row0.slot_align == k_align);
    REQUIRE(row0.chunk_slots == (1u << k_node_bits));
    REQUIRE(row0.high_water == 0);
    const arbc::WorkspaceStoreEntry row1 = read_store_row(path.str(), *hdr, ab, 1);
    REQUIRE(row1.slot_stride == k_record_stride);
    REQUIRE(row1.chunk_slots == (1u << k_record_bits));
  }
  // Unused rows stay all-zero (slot_stride == 0 IS the "unused" encoding).
  REQUIRE(read_store_row(path.str(), *hdr, 0, 2).slot_stride == 0);
}

// enforces: 15-memory-model#store-layout-mismatch-rejected
TEST_CASE("a reopening build whose geometry disagrees with the store table is refused") {
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);
  for (std::uint32_t i = 0; i < 10; ++i) {
    fill_slot(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
  }
  REQUIRE(ckpt.commit(0).has_value());

  SECTION("a different slots-per-chunk for the same size class -> StoreLayoutMismatch") {
    // The debug-vs-release lane mismatch: same stride, different chunk geometry. The
    // file's chunks do not carve into the slots this build would read, so binding is
    // refused as a VALUE -- never a silent mis-route.
    TempPath dst;
    copy_file(path.str(), dst.str());
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE(opened.has_value());
    arbc::WorkspaceFileChunkSource& ws2 = *opened->source;

    auto view = ws2.store_view(k_node_stride, k_align, 64); // file says 128
    REQUIRE_FALSE(view.has_value());
    REQUIRE(view.error().code == arbc::WorkspaceFileErrc::StoreLayoutMismatch);

    // Through the arena the refusal is structural: the store is minted, but backed by
    // a source that hands out nothing, so it can never serve another store's chunks.
    arbc::Arena arena2(ws2.router());
    arbc::SlotStore& mismatched = arena2.store_for(k_node_stride, k_align, /*chunk_bits=*/6);
    REQUIRE(ws2.bind_error().code == arbc::WorkspaceFileErrc::StoreLayoutMismatch);
    REQUIRE_FALSE(mismatched.allocate().has_value());

    arbc::Checkpointer ckpt2(ws2, arena2);
    auto restored = ckpt2.reserve_restored_all(arena2);
    REQUIRE_FALSE(restored.has_value());
    REQUIRE(restored.error().code == arbc::WorkspaceFileErrc::StoreLayoutMismatch);
  }

  SECTION("a format-1 file -> UnsupportedFormat, never an untagged-chunk guess") {
    // v1 chunks are untagged; a later reader could only guess at ownership -- the very
    // bug being fixed. Refuse. The workspace file is a same-machine session artifact
    // beside the doc 08 JSON document, so this costs a reload from JSON, not data.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint32_t v1 = 1;
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, format_major), &v1, sizeof(v1));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::UnsupportedFormat);
    auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE_FALSE(hdr.has_value());
    REQUIRE(hdr.error().code == arbc::WorkspaceFileErrc::UnsupportedFormat);
  }

  SECTION("a format-2 file -> UnsupportedFormat, never an unstamped snapshot read as stamped") {
    // A v2 store table sits at a different offset, has no generation stamp, and shares
    // its page with its twin. Reading one under the v3 layout would pair a root with
    // bytes that are not its snapshot -- exactly the distrust this format bump exists
    // to enforce. Pre-release: no migration is owed (Decision 7).
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint32_t v2 = 2;
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, format_major), &v2, sizeof(v2));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::UnsupportedFormat);
    auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE_FALSE(hdr.has_value());
    REQUIRE(hdr.error().code == arbc::WorkspaceFileErrc::UnsupportedFormat);
  }

  SECTION("a high-water the store's chunks do not cover -> StoreDirectoryInconsistent") {
    // A hole in a store's chunk sequence, a mis-tagged chunk, or a torn directory all
    // land here: the invariant that a store's k-th chunk backs its k-th slot range is
    // ASSERTED on open, not trusted (Decision 6).
    TempPath dst;
    copy_file(path.str(), dst.str());
    const auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE(hdr.has_value());
    // The first commit published into snapshot A. Claim a high-water needing 3 chunks
    // where the store has 1.
    const std::uint32_t inflated = 3u * (1u << k_node_bits);
    write_at(dst.str(),
             store_row_offset(*hdr, 0, 0) + offsetof(arbc::WorkspaceStoreEntry, high_water),
             &inflated, sizeof(inflated));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::StoreDirectoryInconsistent);
  }

  SECTION("a chunk tagged with a row the table cannot hold -> StoreDirectoryInconsistent") {
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint32_t bogus_owner = 4096; // >= max_stores, but not the no-owner tag
    write_at(dst.str(), sizeof(arbc::WorkspaceHeader) + offsetof(arbc::WorkspaceChunkEntry, owner),
             &bogus_owner, sizeof(bogus_owner));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::StoreDirectoryInconsistent);
  }

  SECTION("a non-power-of-two slots-per-chunk -> StoreLayoutMismatch, never a bad shift") {
    // SlotStore indexes chunks by SHIFTING the slot index, so a row whose
    // slots-per-chunk is not a power of two is geometry no build can act on. Refuse
    // it as a value rather than derive a nonsense chunk-bits from it.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE(hdr.has_value());
    const std::uint32_t not_a_power_of_two = 100;
    write_at(dst.str(),
             store_row_offset(*hdr, 0, 0) + offsetof(arbc::WorkspaceStoreEntry, chunk_slots),
             &not_a_power_of_two, sizeof(not_a_power_of_two));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::StoreLayoutMismatch);
  }

  SECTION("a non-canonical slot alignment -> StoreLayoutMismatch, never a duplicate store") {
    // `Arena::store_for` NORMALIZES its key (alignment at least alignof(max_align_t),
    // stride rounded up to it). A row below that canonical form would be normalized
    // into a DIFFERENT key on restore, find no matching row, and mint a second store
    // over a file that already holds the first one's chunks. Refuse the row instead.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE(hdr.has_value());
    const std::uint32_t under_aligned = 8; // a power of two, but below the canonical floor
    write_at(dst.str(),
             store_row_offset(*hdr, 0, 0) + offsetof(arbc::WorkspaceStoreEntry, slot_align),
             &under_aligned, sizeof(under_aligned));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::StoreLayoutMismatch);
  }

  SECTION("a chunk_count past the directory's capacity -> HeaderIoFailed, never an OOB read") {
    // The reopen walk indexes dir[0 .. chunk_count); the mapping only covers
    // max_chunks entries. A torn count must be a value, not a read past the mapping.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const std::uint64_t absurd = 100000; // >> the default max_chunks
    write_at(dst.str(), offsetof(arbc::WorkspaceHeader, chunk_count), &absurd, sizeof(absurd));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::HeaderIoFailed);
  }

  SECTION("an unused row carrying data -> StoreDirectoryInconsistent, a torn table") {
    // `slot_stride == 0` IS the unused encoding, so a zero-stride row with a nonzero
    // high-water is a table torn mid-write, not an empty slot.
    TempPath dst;
    copy_file(path.str(), dst.str());
    const auto hdr = arbc::WorkspaceFileChunkSource::read_header(dst.str());
    REQUIRE(hdr.has_value());
    const std::uint32_t junk = 7;
    write_at(dst.str(),
             store_row_offset(*hdr, 0, 1) + offsetof(arbc::WorkspaceStoreEntry, high_water), &junk,
             sizeof(junk));
    auto opened = arbc::Checkpointer::open(dst.str());
    REQUIRE_FALSE(opened.has_value());
    REQUIRE(opened.error().code == arbc::WorkspaceFileErrc::StoreDirectoryInconsistent);
  }
}

TEST_CASE("a refused store is backed by a source that hands out nothing") {
  // The structural half of the refusal: whatever the reason, a store the router turned
  // down gets a backing that can only fail, so it can never end up serving another
  // store's chunks. Releasing to it is a no-op -- it never handed anything out.
  arbc::RefusingChunkSource refusing;
  auto span = refusing.acquire(4096, k_align);
  REQUIRE_FALSE(span.has_value());
  REQUIRE(span.error() == arbc::PoolError::OutOfMemory);
  refusing.release(arbc::ChunkSpan{});
}

TEST_CASE("binding more size classes than the store table holds is refused as a value") {
  arbc::WorkspaceLayout layout;
  layout.max_stores = 2;

  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str(), layout);
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;

  REQUIRE(ws.store_view(32, k_align, 128).has_value());
  REQUIRE(ws.store_view(64, k_align, 128).has_value());
  auto third = ws.store_view(128, k_align, 128);
  REQUIRE_FALSE(third.has_value());
  REQUIRE(third.error().code == arbc::WorkspaceFileErrc::MaxStoresExceeded);

  // Through the arena, the over-the-cap store allocates nothing rather than sharing
  // (and corrupting) another store's chunks.
  arbc::Arena arena(ws.router());
  arbc::SlotStore& overflow = arena.store_for(256, k_align);
  REQUIRE(ws.bind_error().code == arbc::WorkspaceFileErrc::MaxStoresExceeded);
  REQUIRE_FALSE(overflow.allocate().has_value());
}

TEST_CASE("the default layout's store tables cost exactly two extra pages") {
  // Format 3 buys the root/snapshot pairing check with page residency: each snapshot
  // gets a page of its own, so its stamp cannot become durable without its rows and no
  // other writer dirties the page. The bill is two pages of `data_offset` over format 1
  // -- 8 KiB on a 4 KiB-page machine, 0.5% of the 388 KiB default header region (112 B
  // header + 16384 * 24 B directory = 393328 B, rounded up to 397312). This pins that
  // price: a future field that silently doubled it fails here.
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str()); // default layout
  REQUIRE(src.has_value());
  const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(hdr.has_value());
  REQUIRE(hdr->max_stores == arbc::k_workspace_default_max_stores);
  REQUIRE(hdr->max_chunks == arbc::k_workspace_default_max_chunks);

  const std::uint64_t format1_data_offset = arbc::align_up(
      sizeof(arbc::WorkspaceHeader) +
          std::size_t{arbc::k_workspace_default_max_chunks} * sizeof(arbc::WorkspaceChunkEntry),
      hdr->page_size);
  // The snapshots begin where format 1's data region did, and the data region moves up
  // by exactly the two pages they occupy -- no third page of slack, no rounding slop.
  REQUIRE(hdr->store_table_offset == format1_data_offset);
  REQUIRE(hdr->data_offset == format1_data_offset + 2u * hdr->page_size);

  // ... and each snapshot really is page-resident: stamp + every row inside one page,
  // which is what makes the generation stamp prove the pairing (Decision 3).
  REQUIRE(sizeof(arbc::WorkspaceStoreSnapshot) +
              hdr->max_stores * sizeof(arbc::WorkspaceStoreEntry) <=
          hdr->page_size);
  REQUIRE(hdr->store_table_offset % hdr->page_size == 0);
}

TEST_CASE("commit publishes every registered store's high-water with zero extra syscalls",
          "[pool]") {
  // THE LOAD-BEARING CLAIM that high-water durability is FREE (Decision 4). The
  // store-table write is a plain store into the already-mapped header, made durable by
  // the SAME header msync that publishes the root -- so the commit's syscall sequence
  // is byte-for-byte what it was before this task: N data msyncs + 1 header msync.
  class CountingInjector final : public arbc::SyscallInjector {
  public:
    int before(arbc::WorkspaceSyscall kind, std::uint64_t file_offset,
               std::size_t) noexcept override {
      if (kind == arbc::WorkspaceSyscall::Msync) {
        (file_offset == 0 ? header_msyncs : data_msyncs) += 1;
      } else if (kind == arbc::WorkspaceSyscall::RootFlip) {
        ++root_flips;
      }
      return 0;
    }
    void after(arbc::WorkspaceSyscall, std::uint64_t, std::size_t) noexcept override {}
    long data_msyncs{0};
    long header_msyncs{0};
    long root_flips{0};
  };

  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
  arbc::SlotStore& records = arena.store_for(k_record_stride, k_align, k_record_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);
  ckpt.register_store(records);

  for (std::uint32_t i = 0; i < 300; ++i) {
    (void)nodes.allocate();
    (void)records.allocate();
  }
  const std::size_t live_chunks = ws.live_chunk_count();
  REQUIRE(live_chunks == 5);

  CountingInjector inj;
  ws.set_syscall_injector(&inj);
  REQUIRE(ckpt.commit(0).has_value());
  ws.set_syscall_injector(nullptr);

  REQUIRE(inj.data_msyncs == static_cast<long>(live_chunks)); // one per live data chunk
  REQUIRE(inj.header_msyncs == 1);                            // and exactly one header sync
  REQUIRE(inj.root_flips == 1);
  REQUIRE(ckpt.data_msyncs() == live_chunks);
  REQUIRE(ckpt.header_msyncs() == 1);

  // The high-waters landed in the snapshot the flipped root owns (A, generation 1).
  const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(hdr.has_value());
  REQUIRE(read_store_row(path.str(), *hdr, 0, 0).high_water == 300);
  REQUIRE(read_store_row(path.str(), *hdr, 0, 1).high_water == 300);
  // The OTHER snapshot -- the one the surviving root owns -- was not touched.
  REQUIRE(read_store_row(path.str(), *hdr, 1, 0).high_water == 0);
  REQUIRE(read_store_row(path.str(), *hdr, 1, 1).high_water == 0);
}

TEST_CASE("a commit stamps the snapshot it writes with the generation it flips into the root",
          "[pool]") {
  // The pairing the torn-header check rests on: the snapshot a commit writes and the
  // root slot it flips carry the SAME generation, and the other snapshot -- the one the
  // surviving root owns -- is left alone. Read back off disk, not through the mapping.
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);

  for (std::uint32_t i = 0; i < 10; ++i) {
    fill_slot(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
  }
  REQUIRE(ckpt.commit(0).has_value()); // generation 1, root slot A, snapshot A

  const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(hdr.has_value());
  REQUIRE(arbc::decode_root(ws.root_slot(0)).generation == 1);
  REQUIRE(read_snapshot_stamp(path.str(), *hdr, 0) == 1);
  REQUIRE(read_snapshot_stamp(path.str(), *hdr, 1) == 0); // untouched: B's root is still 0

  for (std::uint32_t i = 10; i < 20; ++i) {
    fill_slot(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
  }
  REQUIRE(ckpt.commit(0).has_value()); // generation 2, root slot B, snapshot B

  REQUIRE(arbc::decode_root(ws.root_slot(1)).generation == 2);
  REQUIRE(read_snapshot_stamp(path.str(), *hdr, 1) == 2);
  REQUIRE(read_snapshot_stamp(path.str(), *hdr, 0) == 1); // generation 1's snapshot, intact
  // ... and each stamp still names the root that owns it: A pairs with the old
  // high-water, B with the new one. Never a mixture.
  REQUIRE(read_store_row(path.str(), *hdr, 0, 0).high_water == 10);
  REQUIRE(read_store_row(path.str(), *hdr, 1, 0).high_water == 20);
}

TEST_CASE("a fresh file's zeroed roots and zeroed stamps pair trivially", "[pool]") {
  // Generation 0 is "never written" in BOTH the root slot and the snapshot stamp, so a
  // fresh file's roots are eligible by the same rule that governs a committed one -- no
  // special case in the selection, and no refusal of a file that simply has no root yet.
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  (*src).reset(); // close it: recovery reads the file, not the live source

  const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path.str());
  REQUIRE(hdr.has_value());
  REQUIRE(hdr->root_slot_a == 0);
  REQUIRE(hdr->root_slot_b == 0);
  REQUIRE(read_snapshot_stamp(path.str(), *hdr, 0) == 0);
  REQUIRE(read_snapshot_stamp(path.str(), *hdr, 1) == 0);

  auto opened = arbc::Checkpointer::open(path.str());
  REQUIRE(opened.has_value());
  REQUIRE_FALSE(opened->valid); // no durable root -- but a clean open, not a refusal
  REQUIRE(opened->generation == 0);
}

TEST_CASE("a max_stores whose snapshot would straddle a page is refused as a value") {
  // The stamp only proves the pairing if it cannot land without its rows, which holds
  // only while stamp + rows share one page. A `max_stores` that would push the rows
  // onto a second page is therefore a geometry the format cannot honour -- refused at
  // create time as a value, never laid out and silently mis-trusted (Decision 3).
  TempPath probe_path;
  auto probe = arbc::WorkspaceFileChunkSource::create(probe_path.str());
  REQUIRE(probe.has_value());
  const std::size_t page = (*probe)->page();

  arbc::WorkspaceLayout layout;
  layout.max_stores = static_cast<std::uint32_t>(
      ((page - sizeof(arbc::WorkspaceStoreSnapshot)) / sizeof(arbc::WorkspaceStoreEntry)) + 1);
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str(), layout);
  REQUIRE_FALSE(src.has_value());
  REQUIRE(src.error().code == arbc::WorkspaceFileErrc::MaxStoresExceeded);

  // One row fewer fits exactly, and is accepted.
  layout.max_stores -= 1;
  TempPath fits;
  auto ok = arbc::WorkspaceFileChunkSource::create(fits.str(), layout);
  REQUIRE(ok.has_value());
}

TEST_CASE("alternating commits reopen on the newest root with that root's high-waters", "[pool]") {
  // The round trip the generation stamp must not disturb: N commits alternate A/B, and
  // every reopen lands on the newest root and restores exactly the high-waters that
  // root's snapshot published -- not the other snapshot's, which is a whole batch behind.
  constexpr std::uint32_t k_batch = 40;
  for (std::uint32_t commits = 1; commits <= 4; ++commits) {
    TempPath path;
    auto src = arbc::WorkspaceFileChunkSource::create(path.str());
    REQUIRE(src.has_value());
    arbc::WorkspaceFileChunkSource& ws = **src;
    arbc::Arena arena(ws.router());
    arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
    arbc::SlotStore& records = arena.store_for(k_record_stride, k_align, k_record_bits);
    arbc::Checkpointer ckpt(ws, arena);
    ckpt.register_store(nodes);
    ckpt.register_store(records);

    std::uint32_t written = 0;
    for (std::uint32_t c = 0; c < commits; ++c) {
      for (std::uint32_t i = written; i < written + k_batch; ++i) {
        fill_slot(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
        fill_slot(records.resolve(*records.allocate()), k_record_stride, 2, i);
      }
      written += k_batch;
      REQUIRE(ckpt.commit(written - 1).has_value());
    }
    REQUIRE(ckpt.generation() == commits);

    // The commits really did alternate slots, and each snapshot is stamped with the
    // generation of the root beside it.
    const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path.str());
    REQUIRE(hdr.has_value());
    for (int ab = 0; ab < 2; ++ab) {
      REQUIRE(read_snapshot_stamp(path.str(), *hdr, ab) ==
              arbc::decode_root(ws.root_slot(ab)).generation);
    }

    TempPath recovered;
    copy_file(path.str(), recovered.str());
    auto opened = arbc::Checkpointer::open(recovered.str());
    REQUIRE(opened.has_value());
    REQUIRE(opened->valid);
    REQUIRE(opened->generation == commits);
    REQUIRE(opened->root_index == written - 1);

    arbc::WorkspaceFileChunkSource& ws2 = *opened->source;
    arbc::Arena arena2(ws2.router());
    arbc::Checkpointer ckpt2(ws2, arena2);
    REQUIRE(ckpt2.reserve_restored_all(arena2).has_value());
    arbc::SlotStore& nodes2 = arena2.store_for(k_node_stride, k_align, k_node_bits);
    arbc::SlotStore& records2 = arena2.store_for(k_record_stride, k_align, k_record_bits);
    REQUIRE(nodes2.high_water() == written);
    REQUIRE(records2.high_water() == written);
    for (std::uint32_t i = 0; i < written; ++i) {
      REQUIRE(slot_matches(nodes2.resolve(i), k_node_stride, 1, i));
      REQUIRE(slot_matches(records2.resolve(i), k_record_stride, 2, i));
    }
    // The reopened checkpointer must publish into the OTHER slot than the one it
    // recovered from, or its next commit would overwrite the only coherent snapshot.
    REQUIRE(ckpt2.commit(0).has_value());
    REQUIRE(arbc::decode_root(ws2.root_slot(commits % 2)).generation == commits + 1);
  }
}

TEST_CASE("chunks appended after the last checkpoint are hole-punched on reopen", "[pool]") {
#if defined(__linux__) || defined(_WIN32)
  // Post-checkpoint chunk garbage: a crash after chunks were appended but before the
  // commit that would have published them leaves live directory entries above the
  // selected root's high-water. `reserve_restored` never claims them, so reopen
  // punches them -- the chunk-level analogue of the freed-slot quarantine, keeping the
  // file from leaking storage across an unclean shutdown. Guarded to sparse-capable
  // volumes, matching the sibling #hole-punch-returns-storage test.
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);

  // One full chunk (128 slots), committed: the durable root covers chunk 0 only.
  constexpr std::uint32_t k_chunk_slots = 1u << k_node_bits;
  for (std::uint32_t i = 0; i < k_chunk_slots; ++i) {
    fill_slot(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
  }
  REQUIRE(ckpt.commit(0).has_value());
  REQUIRE(ws.chunk_count() == 1);

  // Now grow two more chunks and flush them to disk -- but never commit. This is the
  // state an unclean shutdown leaves: real allocated blocks no durable root reaches.
  for (std::uint32_t i = k_chunk_slots; i < 3 * k_chunk_slots; ++i) {
    fill_slot(nodes.resolve(*nodes.allocate()), k_node_stride, 1, i);
  }
  REQUIRE(ws.chunk_count() == 3);
  REQUIRE(ws.sync_data().has_value());

  TempPath dst;
  copy_file(path.str(), dst.str());
  const std::uint64_t before = allocated_size(dst.str());

  auto opened = arbc::Checkpointer::open(dst.str());
  REQUIRE(opened.has_value());
  REQUIRE(opened->valid);
  // Two orphan chunks came back to the filesystem.
  REQUIRE(allocated_size(dst.str()) < before);

  // ... and the durable state is intact: chunk 0's 128 slots restore exactly.
  arbc::WorkspaceFileChunkSource& ws2 = *opened->source;
  arbc::Arena arena2(ws2.router());
  arbc::Checkpointer ckpt2(ws2, arena2);
  REQUIRE(ckpt2.reserve_restored_all(arena2).has_value());
  arbc::SlotStore& nodes2 = arena2.store_for(k_node_stride, k_align, k_node_bits);
  REQUIRE(nodes2.high_water() == k_chunk_slots);
  for (std::uint32_t i = 0; i < k_chunk_slots; ++i) {
    REQUIRE(slot_matches(nodes2.resolve(i), k_node_stride, 1, i));
  }
#else
  SUCCEED("hole-punch storage return is a sparse-file (Linux/Windows) guarantee");
#endif
}

TEST_CASE("TSan litmus: readers resolve while the writer grows and commits two stores", "[pool]") {
  // `commit` now writes the store table into the same header mapping it flips the root
  // in, while readers hold resolved pointers into the DATA mappings and read them. The
  // two page sets must stay disjoint: the store table is header memory, never a data
  // page, so a reader's resolve/read races nothing the commit writes. Growth (a
  // directory write) is likewise header-side. TSan is the judge.
  TempPath path;
  auto src = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;
  arbc::Arena arena(ws.router());
  arbc::SlotStore& nodes = arena.store_for(k_node_stride, k_align, k_node_bits);
  arbc::SlotStore& records = arena.store_for(k_record_stride, k_align, k_record_bits);
  arbc::Checkpointer ckpt(ws, arena);
  ckpt.register_store(nodes);
  ckpt.register_store(records);

  // The writer publishes a slot only after it has filled it; readers read strictly
  // below the published watermark, so the data itself is properly ordered and any
  // report TSan makes is a real race on the mapping, not on this test's handshake.
  // Readers report through atomics and the assertions run on the main thread after
  // the join -- Catch2's macros are not thread-safe.
  std::atomic<std::uint32_t> published{0};
  std::atomic<bool> done{false};
  std::atomic<bool> mismatch{false};
  std::atomic<std::uint64_t> reads{0};
  constexpr std::uint32_t k_total = 600; // spans several chunks in both stores

  std::vector<std::thread> readers;
  for (int t = 0; t < 3; ++t) {
    readers.emplace_back([&] {
      while (!done.load(std::memory_order_acquire)) {
        const std::uint32_t limit = published.load(std::memory_order_acquire);
        for (std::uint32_t i = 0; i < limit; ++i) {
          if (!slot_matches(nodes.resolve(i), k_node_stride, 1, i) ||
              !slot_matches(records.resolve(i), k_record_stride, 2, i)) {
            mismatch.store(true, std::memory_order_relaxed);
          }
          reads.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (std::uint32_t i = 0; i < k_total; ++i) {
    const arbc::SlotIndex n = *nodes.allocate();
    fill_slot(nodes.resolve(n), k_node_stride, 1, n);
    const arbc::SlotIndex r = *records.allocate();
    fill_slot(records.resolve(r), k_record_stride, 2, r);
    published.store(i + 1, std::memory_order_release);
    if (i % 64 == 63) {
      // Writes the store table AND flips the root, both inside the header mapping,
      // while the readers hold pointers into the data mappings.
      const bool committed = ckpt.commit(n).has_value();
      if (!committed) {
        mismatch.store(true, std::memory_order_relaxed);
        break;
      }
    }
  }
  done.store(true, std::memory_order_release);
  for (std::thread& th : readers) {
    th.join();
  }

  REQUIRE_FALSE(mismatch.load());
  REQUIRE(reads.load() > 0); // the readers really did race the writer
  REQUIRE(ckpt.commit(0).has_value());
  REQUIRE(nodes.high_water() == k_total);
  REQUIRE(records.high_water() == k_total);
}

// --- byte-exact format golden (doc 16's deterministic-format default) ------------
//
// Freezes the header + chunk directory + both store-table snapshots of a fixed-layout
// file, so a silent format drift -- the failure mode that makes a workspace file
// unopenable by the next build -- fails here loudly. Byte comparison; no tolerance.
//
// REGENERATE PROCEDURE (doc 16 tier-3 "regenerate with an audited script"): an
// intended format change deliberately re-freezes this table; it never regenerates
// silently, and re-freezing it is a decision to bump `k_workspace_format_major`. Build
// this target and run only the hidden dump case, which prints a paste-ready literal:
//
//     cmake --build --preset dev --target arbc_pool_t
//     ./build/dev/src/pool/arbc_pool_t "[regen]"
//
// then replace the FROZEN block below with its output. The dump case is hidden
// (`[.regen]`) so it never runs in the default suite, and GCOV-excluded because it is a
// maintenance tool, not a covered assertion.

namespace {

constexpr std::uint32_t k_golden_max_chunks = 8;
constexpr std::uint32_t k_golden_max_stores = 4;
// Header + chunk directory (contiguous from offset 0), then each page-resident
// snapshot's occupied head. The pad between the directory's end and snapshot A, and
// between the two snapshots, is page-size-dependent zero fill -- a platform fact, not a
// format fact -- so the golden splices the three occupied runs together instead.
constexpr std::size_t k_golden_dir_bytes =
    sizeof(arbc::WorkspaceHeader) + k_golden_max_chunks * sizeof(arbc::WorkspaceChunkEntry);
constexpr std::size_t k_golden_snapshot_bytes =
    sizeof(arbc::WorkspaceStoreSnapshot) + k_golden_max_stores * sizeof(arbc::WorkspaceStoreEntry);
constexpr std::size_t k_golden_bytes =
    k_golden_dir_bytes + 2 * k_golden_snapshot_bytes; // 304 + 2*72 = 448

// Build the golden file: a fresh fixed-layout workspace with the two colliding size
// classes bound and one chunk grown through the first, then read back the header +
// directory and both snapshots. `page_size` and `data_offset` are normalized to zero
// (they are platform facts, not format facts); the 4 KiB page guard keeps the rest --
// including `store_table_offset` and the directory entry's offset and size --
// deterministic.
std::vector<std::uint8_t> golden_region(const std::string& path) {
  arbc::WorkspaceLayout layout;
  layout.max_chunks = k_golden_max_chunks;
  layout.max_stores = k_golden_max_stores;
  auto src = arbc::WorkspaceFileChunkSource::create(path, layout);
  REQUIRE(src.has_value());
  arbc::WorkspaceFileChunkSource& ws = **src;

  auto nodes = ws.store_view(k_node_stride, k_align, 1u << k_node_bits);
  REQUIRE(nodes.has_value());
  auto records = ws.store_view(k_record_stride, k_align, 1u << k_record_bits);
  REQUIRE(records.has_value());
  REQUIRE((*nodes)->acquire(1024, k_align).has_value()); // one owner-tagged directory entry

  const auto hdr = arbc::WorkspaceFileChunkSource::read_header(path);
  REQUIRE(hdr.has_value());
  std::vector<std::uint8_t> bytes = read_bytes(path, 0, k_golden_dir_bytes);
  for (int ab = 0; ab < 2; ++ab) {
    const std::vector<std::uint8_t> snap =
        read_bytes(path, snapshot_offset(*hdr, ab), k_golden_snapshot_bytes);
    bytes.insert(bytes.end(), snap.begin(), snap.end());
  }
  std::memset(bytes.data() + offsetof(arbc::WorkspaceHeader, page_size), 0,
              sizeof(arbc::WorkspaceHeader::page_size));
  std::memset(bytes.data() + offsetof(arbc::WorkspaceHeader, data_offset), 0,
              sizeof(arbc::WorkspaceHeader::data_offset));
  return bytes;
}

// FROZEN EXPECTED TABLE -- regenerate only via the procedure above.
// Reads, for the record, as: magic "ARBCWSBF"; format 3.0; max_chunks 8;
// chunk_count 1; both roots zero; store_table_offset 0x1000 (page-aligned, so each
// snapshot owns its page); max_stores 4. Then one live directory entry at file offset
// 0x3000 (== store_table_offset + two snapshot pages), 0x9000 bytes, owner row 0. Then
// BOTH snapshots: a zero generation stamp (never committed) followed by the identical
// identity rows {288, 16, 128, hw 0} and {144, 16, 256, hw 0}.
constexpr std::uint8_t k_golden_header_region[k_golden_bytes] = {
    0x41, 0x52, 0x42, 0x43, 0x57, 0x53, 0x42, 0x46, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x01, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x90, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

} // namespace

TEST_CASE("byte-exact golden: a fresh workspace file's header, directory, and store tables") {
  TempPath path;
  const auto probe = arbc::WorkspaceFileChunkSource::create(path.str());
  REQUIRE(probe.has_value());
  if ((*probe)->page() != 4096) {
    SUCCEED("format golden is frozen against a 4 KiB page (chunk offsets are page-derived)");
    return;
  }

  TempPath golden_path;
  const std::vector<std::uint8_t> got = golden_region(golden_path.str());
  REQUIRE(got.size() == k_golden_bytes);
  REQUIRE(std::memcmp(got.data(), k_golden_header_region, k_golden_bytes) == 0);
}

// GCOV_EXCL_START -- maintenance tool, not an assertion (see REGENERATE PROCEDURE).
TEST_CASE("regen: dump the workspace-format golden", "[.regen]") {
  TempPath path;
  const std::vector<std::uint8_t> bytes = golden_region(path.str());
  std::printf("constexpr std::uint8_t k_golden_header_region[k_golden_bytes] = {\n   ");
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    std::printf(" 0x%02X,", bytes[i]);
    if ((i % 12) == 11) {
      std::printf("\n   ");
    }
  }
  std::printf("\n};\n");
}
// GCOV_EXCL_STOP

#endif // ARBC_HAS_WORKSPACE_FILES
