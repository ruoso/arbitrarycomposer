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

// Capability macro for file-backed workspace arenas (doc 15). POSIX backs it with
// `mmap`/`ftruncate`/`munmap` (hole-punch reclamation additionally needs Linux
// `fallocate(FALLOC_FL_PUNCH_HOLE)`); Windows backs it with `CreateFileMapping`/
// `MapViewOfFile` and `FSCTL_SET_ZERO_DATA` hole-punch (pool.mmap_backing_win32).
// On any other platform the source compiles out and callers fall back to
// `AnonymousChunkSource` — the universal backing.
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)) || defined(_WIN32)
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
  // --- arena directory (pool.workspace_store_directory) ----------------------
  StoreLayoutMismatch,        // a store-table row's geometry disagrees with this build
  MaxStoresExceeded,          // more distinct size classes than the store table holds
  StoreDirectoryInconsistent, // a store's chunk set does not cover its recorded high-water
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
  // The arena directory's store table (pool.workspace_store_directory): file
  // offset of snapshot A and the row count of each snapshot. TWO snapshots, one per
  // root slot: a commit writes only the INACTIVE one, so the high-water a recovery
  // reads always belongs to the root it selected (doc 15, "the store table rides the
  // same A/B discipline"). Each snapshot is PAGE-RESIDENT: `store_table_offset` is
  // page-aligned, the snapshots are one page apart, and stamp + rows fit inside a
  // single page -- one unit of kernel writeback, so a snapshot's generation stamp
  // cannot become durable without the rows it stamps
  // (pool.header_writeback_ordering). Snapshot `ab` therefore begins at
  // `store_table_offset + ab * page_size`.
  std::uint64_t store_table_offset;
  std::uint64_t max_stores;
  std::uint64_t reserved[5]; // zeroed padding, room for future header fields
};

// One chunk-directory entry, laid out immediately after the header on disk. The
// mapping's file offset is recorded explicitly because one source may back
// stores of differing chunk sizes. Cleared (state = 0) on hole-punch release.
struct WorkspaceChunkEntry {
  std::uint64_t offset; // file offset of this chunk's mapping
  std::uint64_t size;   // mapped byte length
  std::uint32_t state;  // WorkspaceChunkState
  // The store-table row of the store that owns this chunk, or k_workspace_no_owner.
  // This tag is what makes reopen unambiguous: chunk BYTE SIZE is not a store
  // identity (a 288-byte stride at 128 slots/chunk and a 144-byte stride at 256
  // slots/chunk both yield 36864-byte chunks), so routing must read the owner, not
  // infer it from geometry. The chunk's slot range is DERIVED from its position in
  // its owner's directory-ordered chunk list -- growth is strictly append-only and a
  // punched entry is never re-used, so per-owner directory order is per-owner
  // acquisition order, permanently (Decision 6).
  std::uint32_t owner;
};

enum WorkspaceChunkState : std::uint32_t {
  k_workspace_chunk_free = 0, // never used, or released (hole-punched)
  k_workspace_chunk_live = 1, // currently mapped and handed out
};

// One store-table row: the geometry that identifies a size-class store on disk,
// plus the slot high-water the checkpoint that published this snapshot made
// durable. GEOMETRY AND HIGH-WATER ONLY -- refcounts, free pools, generation tags,
// and the reclaim queue are anonymous runtime state rebuilt on open (doc
// 15:199-205), so no bookkeeping ever reaches disk. `slot_stride == 0` marks an
// unused row (no separate state field).
struct WorkspaceStoreEntry {
  std::uint32_t slot_stride;
  std::uint32_t slot_align;
  std::uint32_t chunk_slots;
  std::uint32_t high_water;
};

// The head of one store-table snapshot, immediately followed by that snapshot's
// `max_stores` rows inside the same page. `generation` is the root generation of the
// commit that wrote this snapshot -- the SAME value that commit put in the root slot
// it flipped. Open trusts a root only when its snapshot's stamp equals its
// generation, which is what turns the root/snapshot pairing from an assumption about
// kernel writeback ordering (the header's root-slot page and its snapshot pages are
// distinct pages, and nothing orders their writeback) into a checkable fact: a torn
// header that persists the new root without its snapshot is DETECTED, and open falls
// back to the older root that is still coherent. `generation == 0` is the create-time
// zero, which matches a never-written root slot -- so a fresh file is trivially
// paired. Written by the commit as a plain store into a page the header msync
// already covers: zero extra syscalls (pool.header_writeback_ordering).
struct WorkspaceStoreSnapshot {
  std::uint32_t generation;
  std::uint32_t reserved; // zeroed padding, keeps the rows 8-aligned
};

// A chunk grown through the source's own `acquire` rather than through a
// per-store view belongs to no store; the untagged reopen queue serves it back in
// directory order, exactly as format 1 did.
inline constexpr std::uint32_t k_workspace_no_owner = 0xFFFF'FFFFu;

inline constexpr std::uint64_t k_workspace_magic = 0x4642'5357'4342'5241ull; // "ARBCWSBF" LE
// 2: the arena directory. A format-1 file's chunks are untagged and it has no
// store table, so a two-store reopen could only GUESS at ownership.
// 3: the generation-stamped, page-resident store-table snapshot. A format-2 snapshot
// carries no stamp and shares its page with its twin and the directory's tail, so a
// format-3 reader could not tell whether it belongs to the root beside it -- which is
// the exact hole (an unordered partial writeback of the header) this bump fixes.
// Older files are refused as an UnsupportedFormat value; the workspace file is a
// same-machine session artifact beside the doc 08 JSON document (doc 15:214-218), so
// a refused file costs a reload from JSON, never data (Decision 7).
inline constexpr std::uint32_t k_workspace_format_major = 3;
inline constexpr std::uint32_t k_workspace_format_minor = 0;
inline constexpr std::uint32_t k_workspace_default_max_chunks = 16384;
inline constexpr std::uint32_t k_workspace_default_max_stores = 16;

static_assert(sizeof(WorkspaceHeader) == 112, "on-disk header layout is a contract");
static_assert(sizeof(WorkspaceHeader) % 8 == 0, "directory must stay 8-aligned after header");
static_assert(sizeof(WorkspaceChunkEntry) == 24, "on-disk directory entry layout is a contract");
static_assert(sizeof(WorkspaceStoreEntry) == 16, "on-disk store-table row layout is a contract");
static_assert(sizeof(WorkspaceStoreSnapshot) == 8, "on-disk snapshot head layout is a contract");
static_assert(sizeof(WorkspaceStoreSnapshot) % 8 == 0, "rows must stay 8-aligned after the stamp");

// Construction-time policy knobs for a fresh workspace file.
struct WorkspaceLayout {
  std::uint32_t max_chunks{k_workspace_default_max_chunks};
  std::uint32_t max_stores{k_workspace_default_max_stores};
};

// Platform file handle for the workspace file's private state: a POSIX fd or a
// Win32 `HANDLE`. Kept as `void*` on Windows so this public header needs no
// `<windows.h>` (the syscall leaves that use it live behind the seam in the .cpp).
#if defined(_WIN32)
using WorkspaceFileHandle = void*; // Win32 HANDLE
#else
using WorkspaceFileHandle = int; // POSIX fd
#endif

class WorkspaceFileChunkSource;

// The per-store ChunkSource facade (doc 15's arena directory). Handed to exactly
// one size-class store, it serves ONLY that store's chunks: on a reopened file it
// drains the chunks the directory tags with this store's row, in directory order
// (== that store's acquisition order), and every chunk it grows is tagged with
// that row. A store therefore physically cannot see another store's chunks --
// mis-routing is structurally impossible rather than merely discouraged
// (Decision 2). `release` routes to the source's fenced hole-punch path
// unchanged. Owned by the source; obtained through `store_view`.
class WorkspaceStoreView final : public ChunkSource {
public:
  expected<ChunkSpan, PoolError> acquire(std::size_t size, std::size_t alignment) override;
  void release(ChunkSpan span) noexcept override;

private:
  friend class WorkspaceFileChunkSource;
  WorkspaceStoreView(WorkspaceFileChunkSource& source, std::uint32_t id) noexcept
      : d_source(&source), d_id(id) {}

  WorkspaceFileChunkSource* d_source;
  std::uint32_t d_id;
};

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

// The workspace-file syscalls the fault-injection shim can intercept
// (pool.crash_tests). `RootFlip` is not a syscall but the A/B root-slot publish
// store; it is routed through the same seam because it is a durability boundary
// the kill-sweep enumerates alongside the msyncs.
enum class WorkspaceSyscall {
  Mmap,
  Ftruncate,
  Fallocate,
  Msync,
  Mprotect,
  RootFlip,
};

// Installable fault-injection shim over WorkspaceFileChunkSource's I/O (doc 16's
// sanctioned mocking exception, :227-229). Nullptr-default on the source: when
// unset every syscall routes straight to the real `::` call behind one
// predictable branch, so there is no behavior change and no measurable cost. A
// test installs an injector to (a) count each syscall to enumerate injection
// points, (b) inject an errno failure at a chosen call (disk-full sweeps), or
// (c) capture a durable snapshot / terminate the process at a chosen call
// (kill-at-every-syscall sweeps). This is the exact installable-hook idiom
// `set_release_fence` established, kept inside the one class that owns the fd —
// never process-global interposition (LD_PRELOAD), which cannot target a single
// instance. The `pread` calls live in the static `open`/`read_header` factories
// (no instance yet), so their fault paths are exercised by real truncated files
// rather than this member seam.
class SyscallInjector {
public:
  virtual ~SyscallInjector() = default;

  // Consulted immediately before the real syscall. Return 0 to let the real
  // call proceed, or a positive errno to inject as the call's failure (the
  // helper returns the syscall's error sentinel with `errno` set and skips the
  // real call). The injector may also snapshot the file or terminate the
  // process here (kill-before-effect). `file_offset` is the targeted file
  // offset (0 when the call has none).
  virtual int before(WorkspaceSyscall kind, std::uint64_t file_offset,
                     std::size_t len) noexcept = 0;

  // Consulted immediately after a real syscall that actually ran (i.e. when
  // `before` returned 0). Lets the injector observe the now-durable state (a
  // header msync publishes the root) or terminate the process (kill-after).
  virtual void after(WorkspaceSyscall kind, std::uint64_t file_offset,
                     std::size_t len) noexcept = 0;
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
  // errno) and on unsupported platforms. A `max_stores` whose snapshot (stamp +
  // rows) would not fit inside one page is `MaxStoresExceeded`: a straddling
  // snapshot could have its stamp made durable on one page while stale rows sat on
  // the next, which is precisely the pairing the stamp exists to prove.
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

  // --- arena directory / store table (pool.workspace_store_directory) --------

  // Bind the size-class store `(slot_stride, slot_align)` -- the very key
  // `Arena::store_for` already uses -- to its own chunk-source facade.
  //
  // On a fresh file this appends a store-table row to BOTH snapshots: the identity
  // columns are per-store, not per-root, so they are duplicated; only `high_water`
  // differs per snapshot. On a reopened file it matches the row the adopted
  // snapshot records and VALIDATES `chunk_slots` -- a disagreement (a debug-vs-
  // release lane mismatch, say) is `StoreLayoutMismatch`, a clean value, never a
  // silent mis-route. Repeated binds of the same size class return the same facade.
  // `MaxStoresExceeded` when the table is full. Writer-only setup.
  expected<ChunkSource*, WorkspaceFileError>
  store_view(std::uint32_t slot_stride, std::uint32_t slot_align, std::uint32_t chunk_slots);

  // The per-store routing seam an Arena binds to: `Arena arena(ws.router())` hands
  // every size-class store its own `store_view` facade. `Arena arena(ws)` -- the
  // plain ChunkSource constructor -- keeps the untagged single-queue behavior, so
  // every existing single-store caller is unchanged.
  ChunkSourceRouter& router() noexcept { return d_router; }

  // The last `store_view` refusal. The router reports a refusal as a null source
  // (it cannot return a value); this carries the reason. `Ok` until a bind fails.
  const WorkspaceFileError& bind_error() const noexcept { return d_bind_error; }

  // Adopt the store-table snapshot the selected root published (0 = A, 1 = B) --
  // the recovery step that makes reopen unambiguous. Validates every row's
  // geometry; checks that each store's reopened chunk set COVERS its recorded
  // high-water (`StoreDirectoryInconsistent` otherwise -- an asserted invariant,
  // never a trusted one); and hole-punches the post-checkpoint chunk garbage,
  // i.e. the live chunks a crashed commit appended above the selected root's
  // high-water, which `reserve_restored` would never claim. That punch is the
  // chunk-level analogue of the freed-slot durability quarantine and is what keeps
  // the file from leaking storage across an unclean shutdown. Called once, by
  // `Checkpointer::open`.
  expected<std::monostate, WorkspaceFileError> adopt_snapshot(int ab);

  // The adopted snapshot's rows (empty until `adopt_snapshot`; all-zero on a file
  // with no stores). Unused rows have `slot_stride == 0`.
  const std::vector<WorkspaceStoreEntry>& restored_stores() const noexcept {
    return d_restored_stores;
  }

  // The store-table row index of `(slot_stride, slot_align)`, or
  // `k_workspace_no_owner` when that size class has no row.
  std::uint32_t store_id(std::uint32_t slot_stride, std::uint32_t slot_align) noexcept;

  // Write `high_water` into store-table snapshot `ab`'s row `id` -- the commit's
  // high-water publish. A plain store into the already-mapped header: the commit
  // only ever writes the INACTIVE snapshot, and the same `sync_header()` that makes
  // the flipped root durable makes these durable with it. Zero extra syscalls
  // (Decision 4).
  void publish_store_high_water(int ab, std::uint32_t id, std::uint32_t high_water) noexcept;

  // Read / write snapshot `ab`'s generation stamp: the generation of the root slot
  // that owns it. The commit stamps the snapshot it just wrote with the same
  // generation it flips into the root slot, in the same critical section, and both
  // are published by the one `sync_header()` -- another plain store into a page that
  // msync already covers, so the stamp is free. `Checkpointer`'s root selection
  // trusts a root only when `snapshot_generation(ab) == root(ab).generation`
  // (pool.header_writeback_ordering).
  std::uint32_t snapshot_generation(int ab) const noexcept;
  void publish_snapshot_generation(int ab, std::uint32_t generation) noexcept;

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

  // Install (or clear, with nullptr) the fault-injection shim (pool.crash_tests).
  // Default is no injector: every syscall runs directly. Writer/test-only setup.
  void set_syscall_injector(SyscallInjector* injector) noexcept { d_injector = injector; }

  std::size_t live_chunk_count() const noexcept { return d_live.size(); }
  std::size_t page() const noexcept { return d_page; }

  // Diagnostics: the last errno-bearing failure from acquire()/release(). The
  // ChunkSource interface can only return a bare PoolError, so this is how a
  // disk-full growth surfaces its errno without an abort.
  const WorkspaceFileError& last_error() const noexcept { return d_last_error; }

  const std::string& path() const noexcept { return d_path; }
  std::uint64_t chunk_count() const noexcept { return d_chunk_count; }

private:
  friend class WorkspaceStoreView;
  WorkspaceFileChunkSource() = default;

  WorkspaceHeader* header() noexcept;
  WorkspaceChunkEntry* directory() noexcept;
  // Store-table snapshot `ab` (0 = A, 1 = B) inside the mapped header: its stamp,
  // and row 0 of the rows that follow the stamp in the same page. The snapshots are
  // one page apart, so `store_table(0)` and `store_table(1)` never share a page --
  // nor does either share one with the root slots.
  WorkspaceStoreSnapshot* store_snapshot(int ab) const noexcept;
  WorkspaceStoreEntry* store_table(int ab) const noexcept;

  // Size the per-store state to the file's store table and arm the router. Called
  // once by `create` / `open`, after the header mapping is established.
  void init_store_directory(std::uint32_t max_stores, std::uint64_t table_offset);

  // Append a fresh chunk to the file, tagging its directory entry with `owner`
  // (`k_workspace_no_owner` for the untagged path). The single growth path behind
  // both `acquire` and every store view's `acquire`.
  expected<ChunkSpan, PoolError> grow(std::size_t size, std::size_t alignment, std::uint32_t owner);

  // Serve store `id`: drain its reopened chunks first (in directory order, which is
  // its acquisition order), then grow. The body of WorkspaceStoreView::acquire.
  expected<ChunkSpan, PoolError> acquire_for(std::uint32_t id, std::size_t size,
                                             std::size_t alignment);

  // Adapter feeding an Arena's per-store binds into `store_view` (the same
  // self-pointer idiom as Checkpointer's ChunkFence).
  struct Router final : ChunkSourceRouter {
    WorkspaceFileChunkSource* self{nullptr};
    ChunkSource* source_for(std::size_t slot_stride, std::size_t slot_align,
                            std::size_t chunk_slots) override;
  };

  // Syscall shims: each routes through `d_injector` when installed (one branch),
  // else calls the real syscall directly. Defined only under
  // ARBC_HAS_WORKSPACE_FILES; never ODR-used otherwise. `io_fallocate` is
  // additionally Linux-only (matching `punch_now`).
  int io_ftruncate(std::int64_t length) noexcept;
  void* io_mmap(std::size_t len, int prot, int flags, std::int64_t offset) noexcept;
  int io_fallocate(int mode, std::int64_t offset, std::int64_t len) noexcept;
  int io_msync(void* addr, std::size_t len, int flags, std::uint64_t file_offset) noexcept;
  int io_mprotect(void* addr, std::size_t len, int prot) noexcept;

  struct LiveChunk {
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t index;
  };

  // A chunk remapped by `open` and not yet served back through `acquire`. On
  // reopen the live data chunks are mapped and queued in directory order --
  // per-owner for tagged chunks, in one untagged queue for the rest; recovery's
  // `reserve_restored` pulls them out (front-to-back) instead of growing the file.
  struct ReopenedChunk {
    void* base;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint32_t index;
  };

  // Drop a reopened chunk no store will claim (post-checkpoint garbage): unmap it,
  // hole-punch its file range, and clear its directory entry.
  void discard_reopened(const ReopenedChunk& chunk) noexcept;

  std::string d_path;
#if defined(_WIN32)
  WorkspaceFileHandle d_fd{nullptr};
  void* d_mapping{nullptr}; // Win32 file-mapping HANDLE, recreated on each grow
#else
  WorkspaceFileHandle d_fd{-1};
#endif
  std::size_t d_page{0};
  std::uint32_t d_max_chunks{0};
  std::uint64_t d_data_offset{0};
  std::uint64_t d_next_offset{0};
  std::uint32_t d_chunk_count{0};
  void* d_header_map{nullptr};
  std::size_t d_header_bytes{0};
  std::unordered_map<void*, LiveChunk> d_live;
  // Reopened chunks with no owner tag (grown through the source's own `acquire`),
  // served back by `acquire` in directory order.
  std::vector<ReopenedChunk> d_reopened;
  std::size_t d_reopened_cursor{0};

  // --- arena directory state (pool.workspace_store_directory) ----------------
  std::uint32_t d_max_stores{0};
  std::uint64_t d_store_table_offset{0};
  Router d_router{};
  // All four indexed by store-table row (StoreId), sized `d_max_stores`.
  std::vector<std::unique_ptr<WorkspaceStoreView>> d_views; // null until bound
  std::vector<std::vector<ReopenedChunk>> d_store_reopened; // per-owner, directory order
  std::vector<std::size_t> d_store_cursor;
  std::vector<WorkspaceStoreEntry> d_restored_stores; // the adopted snapshot's rows
  WorkspaceFileError d_bind_error{};
  bool d_from_file{false};        // reopened an existing file (vs created fresh)
  bool d_snapshot_adopted{false}; // adopt_snapshot has run

  ChunkReleaseFence* d_release_fence{nullptr};
  SyscallInjector* d_injector{nullptr};
  WorkspaceFileError d_last_error{};
};

// The facade forwards into its owning source (complete only here).
inline expected<ChunkSpan, PoolError> WorkspaceStoreView::acquire(std::size_t size,
                                                                  std::size_t alignment) {
  return d_source->acquire_for(d_id, size, alignment);
}

inline void WorkspaceStoreView::release(ChunkSpan span) noexcept { d_source->release(span); }

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
