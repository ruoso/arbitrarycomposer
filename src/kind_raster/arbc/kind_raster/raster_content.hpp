#pragma once

#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_traits.hpp>   // WorkingPixel, PixelTraits
#include <arbc/media/surface_format.hpp> // SurfaceFormat
#include <arbc/model/model.hpp>          // Model::Transaction, StateRefSink
#include <arbc/model/journal.hpp>        // StateCostFn, RestoreSink
#include <arbc/model/records.hpp>        // StateHandle, k_state_none

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
// through `PixelTraits` without a backend-cpu kernel (refinement Decision 3).
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

// One immutable tile-pixel blob: `edge*edge` working-linear premultiplied RGBA
// floats (doc 15:19-20 "bulk pixel payloads"). Shared across versions by
// refcount (a `shared_ptr`); an untouched tile keeps its blob identity across a
// paint, a touched tile gets a fresh blob (structural sharing, doc 14:164-171).
struct TileBlob {
  std::vector<float> px; // size edge*edge*k_tile_channels
};
using TileBlobPtr = std::shared_ptr<const TileBlob>;

// One mip level: a grid of `tiles_x * tiles_y` tile blobs (row-major) covering a
// `width x height` logical pixel field. Level 0 is the decoded buffer; each
// higher level is a 2x box-downsample of the level below (doc 14:219 scale
// rungs).
struct Level {
  int width{0};
  int height{0};
  int tiles_x{0};
  int tiles_y{0};
  std::vector<TileBlobPtr> tiles;
};

// One immutable captured tile-table version (the persistent CoW state, doc
// 15:240-242 "tile table in slabs"). A paint produces a new TileTable that
// shares every untouched tile blob with its predecessor by refcount.
class TileTable {
public:
  TileTable(int width, int height, int edge, std::vector<Level> levels)
      : d_width(width), d_height(height), d_edge(edge), d_levels(std::move(levels)) {}

  int width() const { return d_width; }
  int height() const { return d_height; }
  int edge() const { return d_edge; }
  std::size_t level_count() const { return d_levels.size(); }
  const Level& level(std::size_t l) const { return d_levels[l]; }
  const std::vector<Level>& levels() const { return d_levels; }

  // The working-float value of level `l`'s logical pixel (x, y), reading the
  // owning tile and clamping to level bounds (so tile padding is never sampled).
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
};
using TileTablePtr = std::shared_ptr<const TileTable>;

// The persistent CoW tile store: builds tile tables from decoded buffers, paints
// new versions sharing untouched blobs, and interns each version under a
// `StateHandle` slot the model pins. Version slots carry a refcount the model's
// StateRefSink drives (retain on record publish, release at record reclaim, doc
// 14:173-176); a zero-count non-base version drops its blobs, which reclaims any
// no-longer-shared tile-pixel blob by refcount.
//
// Thread-safety: `resolve()`/`intern()`/`paint()`/retain/release take a short
// mutex only to copy or publish the version `shared_ptr`; the returned immutable
// TileTable is then read lock-free, so render workers read frozen blobs while the
// editor paints new versions (doc 14:159-162, refinement Decision "renders
// inline; render_thread_safe() == true").
class RasterStore {
public:
  RasterStore() = default;
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
  // every untouched blob with `base` by refcount (doc 14:164-171). Records the
  // touched-tile bounding rect for the caller's damage.
  StateHandle paint(StateHandle base, const Rect& region, const WorkingPixel& color,
                    Rect& touched_out);

  // StateRefSink seam (writer/drain-thread only, doc 14:173-176).
  void retain_version(StateHandle handle);
  void release_version(StateHandle handle);

  std::size_t state_cost(StateHandle handle) const;

  // --- behavioral-counter / inspection surface (doc 16:54-62) ---
  // Total tile-pixel blobs ever allocated -- the witness that a paint allocates
  // exactly |touched tiles| (+ mip blobs above) and shares the rest.
  std::uint64_t blobs_allocated() const;
  // Live interned versions (a still-referenced version count).
  std::size_t live_versions() const;
  std::uint32_t version_refcount(StateHandle handle) const;

private:
  struct Version {
    TileTablePtr table;
    std::uint32_t refcount{0};
  };

  // Intern `table` at a fresh slot with refcount 0 (the model's retain brings it
  // to 1). Caller must keep a `shared_ptr` (or retain) alive until then.
  StateHandle intern(TileTablePtr table);

  mutable std::mutex d_mutex;
  std::vector<Version> d_versions;    // indexed by slot
  std::vector<SlotIndex> d_free;      // recycled slots
  TileTablePtr d_base_table;          // pins the current live base version
  std::uint64_t d_blobs_allocated{0}; // monotonic allocation counter
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
  int d_edge;
};

// The model-side sinks raster registers onto a live Model/Journal (refinement
// Constraint 9). Production auto-registration is the deferred
// `kinds.raster_runtime_binding`; raster's tests register these directly.

// Adjusts a version's refcount on record publish/reclaim (doc 14:173-176).
class RasterStateRefSink final : public StateRefSink {
public:
  explicit RasterStateRefSink(RasterStore& store) : d_store(&store) {}
  void retain(StateHandle handle) override { d_store->retain_version(handle); }
  void release(StateHandle handle) override { d_store->release_version(handle); }

private:
  RasterStore* d_store;
};

// The tile-table byte cost of a captured handle (journal budgeting, doc 14:120).
class RasterStateCostFn final : public StateCostFn {
public:
  explicit RasterStateCostFn(RasterStore& store) : d_store(&store) {}
  std::size_t cost(const StateHandle& handle) const override {
    return d_store->state_cost(handle);
  }

private:
  RasterStore* d_store;
};

// Rebases the live content on undo/redo navigation (doc 14:117); a no-op for a
// navigation event naming a different content object.
class RasterRestoreSink final : public RestoreSink {
public:
  RasterRestoreSink(RasterContent& content, ObjectId object)
      : d_content(&content), d_object(object) {}
  // The bound content id (known only after the model allocates it) may be set
  // once the object is created.
  void set_object(ObjectId object) { d_object = object; }
  void on_restore(ObjectId content, StateHandle target) override {
    if (content == d_object) {
      d_content->restore(target);
    }
  }

private:
  RasterContent* d_content;
  ObjectId d_object;
};

} // namespace arbc
