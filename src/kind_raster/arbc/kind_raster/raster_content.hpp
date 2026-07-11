#pragma once

#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_traits.hpp>   // WorkingPixel, PixelTraits
#include <arbc/media/surface_format.hpp> // SurfaceFormat
#include <arbc/model/model.hpp>          // Model::Transaction
#include <arbc/model/records.hpp>        // StateHandle, k_state_none
#include <arbc/pool/big_block_pool.hpp>  // BigBlockPool, BlockSlotRef (doc 15 bulk pool)
#include <arbc/pool/chunk_source.hpp>    // ChunkSource, AnonymousChunkSource

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace arbc {

// org.arbc.raster (docs 03/14/15): the decoded-buffer raster reference content
// kind. A finite-bounds, Static, visual-only Content that takes an
// already-decoded pixel buffer (codec-free -- doc 17:145-153) and serves it at
// any requested scale from a mip/tile pyramid, clamping at native resolution and
// reporting `achieved_scale` below the request when asked past native
// (BestEffort). Its editable state is a persistent copy-on-write tile table --
// the reference proof of doc 14's capture discipline (doc 14:164-171): a paint
// stroke copies only the tiles it touches, so capture is O(touched tiles), undo
// memory is O(touched tiles), and reported damage equals the stroke's tile set.

// The power-of-two tile edge (doc 15:240-242 tile geometry). Default matches the
// compositor's power-of-two tile geometry; overridable per instance so tests can
// exercise multi-tile CoW and byte-exact goldens on small buffers.
inline constexpr int k_default_tile_edge = 256;

// Every format in the set is 4-channel; tiles hold decoded working-linear
// premultiplied floats (doc 07), so mip downsample and format-generic render go
// through `PixelTraits` and media's format-agnostic filter bank
// (`arbc/media/image_resampler.hpp`) without a backend-cpu kernel.
inline constexpr std::size_t k_tile_channels = 4;

// An already-decoded pixel buffer handed to a RasterContent (codec-free input,
// doc 17:145-153): raw storage bytes plus the dimensions and SurfaceFormat that
// size and interpret them. Anything codec-backed is the out-of-lib
// arbc-plugin-imageseq's job.
struct DecodedImage {
  int width{0};
  int height{0};
  SurfaceFormat format{k_working_rgba32f};
  std::vector<std::byte> bytes;
};

// One mip level: a grid of `tiles_x * tiles_y` tile blobs (row-major) covering a
// `width x height` logical pixel field. Level 0 is the decoded buffer; each
// higher level is a 2:1 Lanczos-3 half-band decimation of the level below
// (doc 07's resampling-filter policy; doc 14:219 scale rungs).
//
// Each tile is an immutable `edge*edge` working-linear premultiplied RGBA-float
// blob allocated from the content's `BigBlockPool` (doc 15:19-20 "bulk pixel
// payloads … dedicated big-block pool"); the in-record, position-independent,
// 8-byte `BlockSlotRef` is what the tile *table* holds in slabs (doc 15:257).
// An untouched tile keeps its `BlockSlotRef` identity across a paint (shared by
// the pool's inside-out refcount); a touched tile gets a fresh blob (structural
// sharing, doc 14:164-171).
struct Level {
  int width{0};
  int height{0};
  int tiles_x{0};
  int tiles_y{0};
  std::vector<BlockSlotRef> tiles;
};

// One immutable captured tile-table version (the persistent CoW state, doc
// 15:240-242 "tile table in slabs"). A paint produces a new TileTable that
// shares every untouched tile blob with its predecessor by the pool's per-slot
// refcount. The table holds exactly one pool count on every `BlockSlotRef` it
// stores: the constructor retains each ref and the destructor releases each --
// so the caller keeps its own counts on the refs it hands in (a fresh blob's
// allocate-count, or a predecessor version's shared count).
class TileTable {
public:
  TileTable(int width, int height, int edge, std::vector<Level> levels, BigBlockPool* pool)
      : d_width(width), d_height(height), d_edge(edge), d_levels(std::move(levels)), d_pool(pool) {
    for (Level& lvl : d_levels) {
      for (const BlockSlotRef& ref : lvl.tiles) {
        (void)d_pool->retain(ref);
      }
    }
  }
  ~TileTable() {
    // Runs on the writer/drain thread (never an RT render worker, Decision 4):
    // releasing a no-longer-shared blob reclaims it by the pool refcount.
    for (Level& lvl : d_levels) {
      for (const BlockSlotRef& ref : lvl.tiles) {
        d_pool->release(ref);
      }
    }
  }
  TileTable(const TileTable&) = delete;
  TileTable& operator=(const TileTable&) = delete;

  int width() const { return d_width; }
  int height() const { return d_height; }
  int edge() const { return d_edge; }
  std::size_t level_count() const { return d_levels.size(); }
  const Level& level(std::size_t l) const { return d_levels[l]; }
  const std::vector<Level>& levels() const { return d_levels; }

  // The working-float value of level `l`'s logical pixel (x, y), reading the
  // owning tile through `pool.peek` (a zero-refcount immutable-after-fill read)
  // and clamping to level bounds (so tile padding is never sampled).
  WorkingPixel pixel(std::size_t l, int x, int y) const;

  // Level `l`'s logical pixels as a flat width*height*4 float buffer, in
  // row-major order -- the byte-exact golden surface for mip-pyramid tests.
  std::vector<float> level_pixels(std::size_t l) const;

  // Total blob byte footprint of this version (journal budgeting seam).
  std::size_t byte_cost() const;

private:
  int d_width;
  int d_height;
  int d_edge;
  std::vector<Level> d_levels;
  BigBlockPool* d_pool;
};
using TileTablePtr = std::shared_ptr<const TileTable>;

// The persistent CoW tile store: builds tile tables from decoded buffers, paints
// new versions sharing untouched blobs, and interns each version under a
// `StateHandle` slot the model pins. Version slots carry a refcount the model's
// StateRefSink drives (retain on record publish, release at record reclaim, doc
// 14:173-176); a zero-count non-base version drops its blobs, which reclaims any
// no-longer-shared tile-pixel blob by the pool refcount.
//
// The store owns one dedicated `BigBlockPool` per content (doc 15's "dedicated"
// bulk-media population, Decision 2) over an `AnonymousChunkSource` it owns by
// default; the `ChunkSource` is a construction-time injection point so the
// runtime can later supply a durable workspace-file source (Decision 3). The
// pool's lifetime is the content's -- content teardown reclaims every blob.
//
// Thread-safety: `resolve()`/`intern()`/`paint()`/retain/release take a short
// mutex only to copy or publish the version `shared_ptr`; the returned immutable
// TileTable is then read lock-free via `pool.peek` (data pages immutable after
// fill), so render workers read frozen blobs while the editor paints new
// versions (doc 14:159-162, refinement Decision 4). `pool.allocate` is
// writer-only; blob retain/release run only on the writer/drain thread via the
// version-lifetime path, never on an RT render worker.
class RasterStore {
public:
  // Default: own an AnonymousChunkSource and a BigBlockPool over it.
  RasterStore();
  // Injected: borrow an external ChunkSource (e.g. a workspace-file source the
  // runtime supplies). The source must outlive the store.
  explicit RasterStore(ChunkSource& source);
  RasterStore(const RasterStore&) = delete;
  RasterStore& operator=(const RasterStore&) = delete;

  // Build the native pyramid from a decoded buffer and intern it as a version.
  StateHandle build(const DecodedImage& image, int edge);

  // Resolve a version, or nullptr if the slot is unknown/absent.
  TileTablePtr resolve(StateHandle handle) const;

  // The current live base version (what an unpinned render reads). Held pinned
  // independently of the model-driven per-slot refcount, so the base always
  // resolves even if the model releases its slot.
  TileTablePtr base_table() const;
  void set_base(TileTablePtr table);

  // Paint `color` over `region` (in level-0 pixel units) onto the version `base`
  // resolves to, returning a NEW interned version. Copies only the level-0 tiles
  // the region touches (plus the mip tiles geometrically above them) and shares
  // every untouched blob with `base` by the pool refcount (doc 14:164-171).
  // Records the touched-tile bounding rect for the caller's damage.
  StateHandle paint(StateHandle base, const Rect& region, const WorkingPixel& color,
                    Rect& touched_out);

  // StateRefSink seam (writer/drain-thread only, doc 14:173-176).
  void retain_version(StateHandle handle);
  void release_version(StateHandle handle);

  std::size_t state_cost(StateHandle handle) const;

  // --- behavioral-counter / inspection surface (doc 16:54-62) ---
  // Total tile-pixel blobs ever allocated -- the witness that a paint allocates
  // exactly |touched tiles| (+ mip blobs above) and shares the rest. Backed by
  // the dedicated pool's own allocation counter (refinement Constraint 8).
  std::uint64_t blobs_allocated() const;
  // Live interned versions (a still-referenced version count).
  std::size_t live_versions() const;
  std::uint32_t version_refcount(StateHandle handle) const;

  // The dedicated tile-pixel pool (inspection: alignment, per-slot refcount,
  // arena accounting for the doc-15 reference-proof tests).
  BigBlockPool& pool() { return d_pool; }
  const BigBlockPool& pool() const { return d_pool; }

private:
  struct Version {
    TileTablePtr table;
    std::uint32_t refcount{0};
  };

  // Intern `table` at a fresh slot with refcount 0 (the model's retain brings it
  // to 1). Caller must keep a `shared_ptr` (or retain) alive until then.
  StateHandle intern(TileTablePtr table);

  // Declared before the version state so it outlives every TileTable: a
  // TileTable destructor releases blobs into `d_pool`, and members destruct in
  // reverse declaration order (d_base_table/d_versions first, then the pool,
  // then the owned source).
  std::unique_ptr<ChunkSource> d_owned_source; // non-null only for the default ctor
  BigBlockPool d_pool;                         // over the owned or injected source

  mutable std::mutex d_mutex;
  std::vector<Version> d_versions; // indexed by slot
  std::vector<SlotIndex> d_free;   // recycled slots
  TileTablePtr d_base_table;       // pins the current live base version
};

// A visual-only decoded-buffer raster content (Content + Editable facets). Owns
// its RasterStore and a live "base" version; render is a pure read of the pinned
// (or base) version's immutable tiles.
class RasterContent final : public Content, public Editable {
public:
  explicit RasterContent(DecodedImage image, int tile_edge = k_default_tile_edge);

  static constexpr const char* kind_id = "org.arbc.raster";

  // --- Content ---
  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;
  Editable* editable() override { return this; }

  // --- Editable ---
  StateHandle capture() override;
  void restore(StateHandle state) override;
  std::size_t state_cost(StateHandle state) const override;
  // The state-handle reference lifecycle the runtime binding drives off the
  // model's `StateRefSink` (doc 14:173-176): a published record's handle is
  // retained, and releasing it at the record's zero-count reclaim drops the
  // version's no-longer-shared tile blobs by the pool refcount. Writer/drain
  // thread only -- these are exactly `RasterStore`'s version-refcount seams.
  void retain(StateHandle state) override;
  void release(StateHandle state) override;

  // Paint `color` over `region` under transactional discipline (doc 03:152-158):
  // copies only the touched tiles into a new version, assigns it via
  // `set_content_state`, and adds the touched-tile damage to the transaction.
  // `self` is this content's object id. The gesture's coalescing is the caller's
  // (via `Transaction::coalesce`).
  void paint(Model::Transaction& txn, ObjectId self, const Rect& region, const WorkingPixel& color);

  // The live base version handle (what an unpinned render reads).
  StateHandle base_handle() const { return d_base; }
  RasterStore& store() { return d_store; }
  const RasterStore& store() const { return d_store; }

private:
  // Resolve the version a request pins: the snapshot handle if it names a known
  // version, else the live base (doc 14:181-187; an unknown/`k_state_none`
  // snapshot renders the current base state).
  TileTablePtr resolve_for_render(StateHandle snapshot) const;

  RasterStore d_store;
  Rect d_bounds;
  StateHandle d_base;
};

// The model-side sink adapters that used to live here (RasterStateRefSink /
// RasterStateCostFn / RasterRestoreSink) are gone: the runtime now binds an
// editable content's state sinks generically through the `Editable` facet
// (`arbc/runtime/editable_binding.hpp`, `kinds.raster_runtime_binding`), so a
// kind ships no model-sink adapter of its own -- and stays dlopen-safe, since
// the runtime reaches it through virtual facet dispatch, never a concrete type.

} // namespace arbc
