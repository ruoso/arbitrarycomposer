#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/pool/chunk_source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

// Capability macro for file-backed workspace arenas (doc 15). File backing
// needs POSIX `mmap`/`ftruncate`/`munmap`; hole-punch reclamation additionally
// needs Linux `fallocate(FALLOC_FL_PUNCH_HOLE)`. On platforms without them the
// source compiles out and callers fall back to `AnonymousChunkSource` — the
// universal backing. The Windows (`MapViewOfFile`) port is tracked as
// `pool.mmap_backing_win32` (doc 15).
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#define ARBC_HAS_WORKSPACE_FILES 1
#else
#define ARBC_HAS_WORKSPACE_FILES 0
#endif

namespace arbc {

// Runtime companion to ARBC_HAS_WORKSPACE_FILES: the Document construction
// policy (doc 15) queries this to tell the user whether crash recovery is
// available, rather than silently substituting anonymous memory.
inline bool workspace_files_supported() noexcept { return ARBC_HAS_WORKSPACE_FILES != 0; }

// Failure taxonomy for the workspace-file source. Distinct from PoolError:
// PoolError is the store-facing "grow refused" value; WorkspaceFileErrc carries
// the syscall-level detail (with errno) that the store interface cannot express.
enum class WorkspaceFileErrc {
  Ok,
  Unsupported,       // built without ARBC_HAS_WORKSPACE_FILES
  OpenFailed,        // open(2) of the workspace file failed
  HeaderIoFailed,    // ftruncate/mmap/pread of the header region failed
  BadMagic,          // reopened file is not a workspace file
  UnsupportedFormat, // format-major mismatch
  GrowFailed,        // ftruncate/mmap while appending a chunk (incl. disk-full)
  DirectoryFull,     // exceeded the file's chunk-directory capacity
};

// Errors as values, never thrown (doc 10). `sys_errno` is the errno captured at
// the failing syscall, or 0 when not syscall-derived.
struct WorkspaceFileError {
  WorkspaceFileErrc code{WorkspaceFileErrc::Ok};
  int sys_errno{0};
};

// On-disk workspace-file header (doc 15). Native endianness / padding — this is
// a same-machine session artifact with NO portability promise; the doc 08 JSON
// document remains the interchange format. Fixed-width, standard-layout so a
// fresh file's header round-trips; content/bookkeeping rebuild on reopen belongs
// to `pool.checkpoints`.
struct WorkspaceHeader {
  std::uint64_t magic;        // k_workspace_magic
  std::uint32_t format_major; // k_workspace_format_major
  std::uint32_t format_minor; // k_workspace_format_minor
  std::uint32_t page_size;    // sysconf(_SC_PAGESIZE) at create time
  std::uint32_t max_chunks;   // chunk-directory capacity
  std::uint64_t data_offset;  // file offset where chunk 0 begins (page-aligned)
  std::uint64_t chunk_count;  // high-water number of chunks ever appended
  // Two root slots reserved for pool.checkpoints' A/B protocol. This task owns
  // the layout (written zero); the protocol that flips them is pool.checkpoints.
  std::uint64_t root_slot_a;
  std::uint64_t root_slot_b;
  std::uint64_t reserved[7]; // zeroed padding, room for future header fields
};

// One chunk-directory entry, laid out immediately after the header on disk. The
// mapping's file offset is recorded explicitly because one source may back
// stores of differing chunk sizes. Cleared (state = 0) on hole-punch release.
struct WorkspaceChunkEntry {
  std::uint64_t offset; // file offset of this chunk's mapping
  std::uint64_t size;   // mapped byte length
  std::uint32_t state;  // WorkspaceChunkState
  std::uint32_t reserved;
};

enum WorkspaceChunkState : std::uint32_t {
  k_workspace_chunk_free = 0, // never used, or released (hole-punched)
  k_workspace_chunk_live = 1, // currently mapped and handed out
};

inline constexpr std::uint64_t k_workspace_magic = 0x4642'5357'4342'5241ull; // "ARBCWSBF" LE
inline constexpr std::uint32_t k_workspace_format_major = 1;
inline constexpr std::uint32_t k_workspace_format_minor = 0;
inline constexpr std::uint32_t k_workspace_default_max_chunks = 16384;

static_assert(sizeof(WorkspaceHeader) % 8 == 0, "directory must stay 8-aligned after header");
static_assert(sizeof(WorkspaceChunkEntry) == 24, "on-disk directory entry layout is a contract");

// Construction-time policy knobs for a fresh workspace file.
struct WorkspaceLayout {
  std::uint32_t max_chunks{k_workspace_default_max_chunks};
};

class WorkspaceFileChunkSource;

// Durability fence for chunk hole-punch (doc 15). An emptied chunk freed after
// the last checkpoint may still back the on-disk root, so its `fallocate(
// PUNCH_HOLE)` must wait until the emptying is durable. When installed on a
// source, `release` unmaps the chunk but defers the punch to the fence, which
// calls back `punch_now` at the post-durable commit drain. Default is no fence
// (eager punch — safe only for anonymous/pre-checkpoint use).
class ChunkReleaseFence {
public:
  virtual ~ChunkReleaseFence() = default;
  // The source has already unmapped the chunk; its file range at `offset` of
  // `size` bytes (directory entry `index`) is punched later by `punch_now`.
  virtual void on_release(WorkspaceFileChunkSource& source, std::uint64_t offset,
                          std::uint64_t size, std::uint32_t index) = 0;
};

// File-backed ChunkSource (doc 15, "File-backed arenas"): only immutable data
// chunks flow through here; bookkeeping (directory, free list) stays anonymous
// because it never touches a ChunkSource at all — the inside-out split is the
// persistence split, enforced structurally by the store never routing
// bookkeeping through `acquire`.
//
// Growth is chunk-at-a-time: `ftruncate` extends the file by whole chunks and
// each chunk gets its OWN `mmap(MAP_SHARED)` region, so existing mappings never
// move (address stability, mirroring the two-level directory). Release
// hole-punches the chunk (`fallocate(PUNCH_HOLE|KEEP_SIZE)` on Linux) so both
// memory and disk come back, then drops the mapping and clears the directory
// entry.
//
// Scope: this source creates and grows FRESH files. Opening an existing file,
// root discovery, and bookkeeping rebuild are `pool.checkpoints`.
class WorkspaceFileChunkSource final : public ChunkSource {
public:
  // Create a fresh workspace file at `path` (truncating any existing file) and
  // return a source backed by it. Fails as a value on any syscall error (with
  // errno) and on unsupported platforms.
  static expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
  create(const std::string& path, const WorkspaceLayout& layout = {});

  ~WorkspaceFileChunkSource() override;

  expected<ChunkSpan, PoolError> acquire(std::size_t size, std::size_t alignment) override;
  void release(ChunkSpan span) noexcept override;

  // Reopen a workspace file read-only and validate its fixed header fields
  // (magic, format, page size, roots). Content/bookkeeping rebuild is out of
  // scope (pool.checkpoints). Errors as values with errno context.
  static expected<WorkspaceHeader, WorkspaceFileError> read_header(const std::string& path);

  // Reopen an existing workspace file read-write and re-establish the mappings
  // (pool.checkpoints recovery). Validates the header, maps the header/directory
  // region, and remaps every live data chunk in directory order. The remapped
  // chunks are then served, in order, by the next `acquire` calls — so a store's
  // recovery `reserve_restored` re-binds the file's records without growing the
  // file. Errors as values with errno context.
  static expected<std::unique_ptr<WorkspaceFileChunkSource>, WorkspaceFileError>
  open(const std::string& path);

  // --- pool.checkpoints protocol helpers (no on-disk layout change) ---------

  // msync(MS_SYNC) every live data-chunk mapping — step 1 of an ordered commit.
  // Returns the number of chunks synced on success.
  expected<std::size_t, WorkspaceFileError> sync_data() noexcept;

  // msync(MS_SYNC) the header/directory mapping — step 3 of an ordered commit
  // (publishes the flipped root durably).
  expected<std::monostate, WorkspaceFileError> sync_header() noexcept;

  // Read / atomically publish one of the two header A/B root slots (0 = A,
  // 1 = B). The publish is a single naturally-aligned 8-byte store into the
  // mapped header (single-sector, torn-write-free).
  std::uint64_t root_slot(int ab) const noexcept;
  void publish_root_slot(int ab, std::uint64_t value) noexcept;

  // Debug hardening (doc 15): mprotect every live data mapping read-only after a
  // checkpoint publishes it, or a single range writable when a quarantined slot
  // is handed back for reuse. No-op / unsupported outside POSIX debug builds.
  expected<std::monostate, WorkspaceFileError> protect_data(bool read_only) noexcept;
  expected<std::monostate, WorkspaceFileError> protect_range(void* addr, std::size_t size,
                                                             bool read_only) noexcept;

  // Un-fenced hole-punch of a previously-released chunk, called by a
  // ChunkReleaseFence once the emptying is durable. Punches the file range and
  // clears the directory entry.
  void punch_now(std::uint64_t offset, std::uint64_t size, std::uint32_t index) noexcept;

  // Install (or clear, with nullptr) the durability fence that defers chunk
  // hole-punch until durable. Default is eager punch. Writer-only setup.
  void set_release_fence(ChunkReleaseFence* fence) noexcept { d_release_fence = fence; }

  std::size_t live_chunk_count() const noexcept { return d_live.size(); }
  std::size_t page() const noexcept { return d_page; }

  // Diagnostics: the last errno-bearing failure from acquire()/release(). The
  // ChunkSource interface can only return a bare PoolError, so this is how a
  // disk-full growth surfaces its errno without an abort.
  const WorkspaceFileError& last_error() const noexcept { return d_last_error; }

  const std::string& path() const noexcept { return d_path; }
  std::uint64_t chunk_count() const noexcept { return d_chunk_count; }

private:
  WorkspaceFileChunkSource() = default;

  WorkspaceHeader* header() noexcept;
  WorkspaceChunkEntry* directory() noexcept;

  struct LiveChunk {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t index;
  };

  // A chunk remapped by `open` and not yet served back through `acquire`. On
  // reopen the live data chunks are mapped and queued here in directory order;
  // recovery's `reserve_restored` pulls them out (front-to-back) instead of
  // growing the file.
  struct ReopenedChunk {
    void* base;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t index;
  };

  std::string d_path;
  int d_fd{-1};
  std::size_t d_page{0};
  std::uint32_t d_max_chunks{0};
  std::uint64_t d_data_offset{0};
  std::uint64_t d_next_offset{0};
  std::uint32_t d_chunk_count{0};
  void* d_header_map{nullptr};
  std::size_t d_header_bytes{0};
  std::unordered_map<void*, LiveChunk> d_live;
  std::vector<ReopenedChunk> d_reopened;
  std::size_t d_reopened_cursor{0};
  ChunkReleaseFence* d_release_fence{nullptr};
  WorkspaceFileError d_last_error{};
};

// Debug hook for doc 15's "position independence is asserted, not assumed":
// records may reference other slots only through index-only SlotRefs (pool.refs)
// — never a raw pointer into a file mapping, which has no stable base address.
// The model layer can call this after mutating a record to fault-fast on a
// smuggled pointer. Scans pointer-width aligned words in [record, record+size)
// and asserts none fall within [map_base, map_base+map_bytes). No-op under
// NDEBUG.
void debug_assert_position_independent(const void* record, std::size_t size, const void* map_base,
                                       std::size_t map_bytes) noexcept;

} // namespace arbc
