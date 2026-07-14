#pragma once

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/media/pixel_traits.hpp> // WorkingPixel
#include <arbc/surface/surface.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbc::image {

// `org.arbc.image` -- a still image decoded from an EXTERNAL asset URI, and the half of
// the pixel-persistence split that stores NOTHING in the document (doc 08 Principle 3).
//
// Two properties define it, and both are structural rather than conventional:
//
//   * It is READ-ONLY. `editable()` stays at its `nullptr` default (doc 03:256-268).
//     That single omission is what makes non-destructive editing structural: you cannot
//     paint on a photograph, so retouching one MUST stack an editable `org.arbc.raster`
//     above a referenced `org.arbc.image`. It is not an omission to be "fixed" later.
//   * It STORES NOTHING. Its `params` is exactly `{"source": "<authored-uri>"}` -- no
//     pixels, no tiles, no intrinsic size, no decoded cache. The pixels live in the file
//     the URI names; the document holds the reference and nothing else.
//
// It is the sibling of `org.arbc.imageseq` and, like it, ships OUTSIDE `libarbc` behind
// doc 17's codec line -- but only the DECODER crosses that line. The serialize codec that
// reads `params.source` lives in `runtime` (`src/runtime/codec_image.cpp`), because it
// parses our own JSON and a URI string and never an encoded image byte (doc 17 "The codec
// line is a decoder line"). Consequently THIS PLUGIN PERFORMS NO FILE I/O AT ALL: the
// core resolves the URI through `LoadContext` and fetches the bytes through the
// `AssetSource` hook, then hands the already-fetched bytes here through the opaque,
// kind-defined `ContentConfig` (kinds/image.md Decision 5).
//
// Contract shape:
//   * stability() == Static; time_extent() == nullopt; quantize_time keeps its nullopt
//     default; achieved_time stays nullopt; audio() == nullptr; inputs() empty.
//   * render() answers with a content-PROVIDED surface (doc 09:150-182) covering exactly
//     the requested region at the achieved scale -- never a whole decoded frame
//     (Decision 1). It is non-transient and refcounted; its release callback returns it
//     to a small plugin-owned free list.
//   * render_thread_safe() == true: the pyramid is immutable and PUBLISHED EXACTLY ONCE,
//     atomically, so a render is a pure read of a pointer that is either null or final and
//     the core may compute it on worker threads (Decision 4, as amended by
//     kinds.image_async_pending Decision 6).
//   * An UNAVAILABLE image (missing / unreadable / undecodable asset, or no `AssetSource`
//     installed) reports EMPTY bounds and renders nothing, keeping its authored URI
//     verbatim so the layer re-saves byte-identically (Decision 7). It is never a load
//     error (doc 08:126-134).
//   * A PENDING image -- one whose `AssetSource` has not ANSWERED yet -- is minted in
//     exactly the unavailable shape and GAINS its pixels later, through `install_asset()`
//     on the writer thread (kinds.image_async_pending). Pending and unavailable differ by
//     whether the source answered, never by the bytes being empty; the kind cannot tell
//     them apart and does not need to, because doc 08:135-144 already made an extent-less
//     image render nothing.

// An immutable mip pyramid over one decoded master. Level 0 is the decoded image in the
// working space (`Rgba32fLinearPremul`); each level above is a 2:1 Lanczos-3 half-band
// decimation of the one below, through `media`'s frozen kernel bank -- the SAME kernels
// `org.arbc.raster` uses, reached through `arbc::media` (L2), which is exactly where a
// plugin may reach. Built once at construction and never mutated: no CoW, no
// `StateHandle`, no `ChunkSource`, no pool backing, no versioning (Decision 4).
// Read-only implies immutable implies none of raster's tile machinery is needed -- and
// immutability is what makes `render_thread_safe()` an obvious `true` rather than a
// question.
class Pyramid {
public:
  // Decode `encoded` (an stb-class encoded still) into a fresh pyramid, or nullptr when
  // the bytes are absent or undecodable. Errors are values; nothing throws (Constraint 7).
  static std::shared_ptr<const Pyramid> decode(std::span<const unsigned char> encoded);

  int width() const noexcept { return d_width; }
  int height() const noexcept { return d_height; }
  std::size_t level_count() const noexcept { return d_levels.size(); }

  // The working-space pixel at level `level`, CLAMPED TO EDGE -- matching the convention
  // raster's `TileTable::pixel` uses, so both resample paths read a border rather than a
  // zero surround (which would darken every edge).
  WorkingPixel pixel(std::size_t level, int x, int y) const;

  // Resident bytes across every level -- the measuring instrument a later byte-budgeted
  // eviction (`kinds.image_master_budget`) needs.
  std::size_t resident_bytes() const noexcept;

private:
  struct Level {
    int width{0};
    int height{0};
    std::vector<float> px; // rgba32f, tightly packed, row-major
  };

  int d_width{0};
  int d_height{0};
  std::vector<Level> d_levels;
};

using PyramidPtr = std::shared_ptr<const Pyramid>;

// The plugin-side pyramid cache, keyed by RESOLVED URI (doc 08:116-122: cross-file sharing
// dedups by resolved identity, not by spelling). N layers whose authored refs resolve to
// one URI -- `bg.png` and `./bg.png` -- share ONE decoded pyramid and issue exactly ONE
// decode; `decodes_issued()` is the behavioral counter that pins it (doc 16:54-62), never a
// wall-clock assertion.
//
// Entries are `weak_ptr`, so a pyramid dies with the last content referencing it and the
// cache never keeps pixels alive on its own. Mutex-guarded: construction runs on the
// load/writer thread in production, but several contents resolving to one URI may be
// constructed concurrently, and the TSan driver races exactly that.
class PyramidCache {
public:
  // The pyramid for `resolved_uri`, decoding `encoded` on a miss. nullptr when the bytes
  // are absent or undecodable -- the UNAVAILABLE state, never an error. An empty
  // `resolved_uri` is never cached (it names no identity to dedup on).
  PyramidPtr resolve(std::string_view resolved_uri, std::span<const unsigned char> encoded);

  // One per genuine decode. A second content resolving to a URI already resident bumps it
  // zero; so does re-rendering an unchanged image (render never decodes).
  std::uint64_t decodes_issued() const noexcept;

private:
  mutable std::mutex d_mutex;
  std::unordered_map<std::string, std::weak_ptr<const Pyramid>> d_by_uri;
  std::uint64_t d_decodes_issued{0};
};

// The process-wide cache the registered `ContentFactory` resolves through, so two LAYERS
// of one document (each built by its own factory call) share a decode. A test may pass its
// own cache to `ImageContent` directly to keep its counter isolated.
PyramidCache& default_pyramid_cache();

// A working-space CPU surface the plugin owns outright -- `std::vector<std::byte>` storage,
// format `k_working_rgba32f`, NO `Backend` edge (the `FrameSurface` shape imageseq
// established). A plugin allocates no surface through a `Backend`: `backend-cpu` is L3 and
// forbidden to it, and pixels route only through `arbc::media` (Constraint 1).
class TileSurface final : public Surface {
public:
  TileSurface(int width, int height);

  int width() const override { return d_width; }
  int height() const override { return d_height; }
  SurfaceFormat format() const override { return d_format; }
  std::span<std::byte> cpu_bytes() override { return d_data; }
  std::span<const std::byte> cpu_bytes() const override { return d_data; }

private:
  int d_width;
  int d_height;
  SurfaceFormat d_format{k_working_rgba32f};
  std::vector<std::byte> d_data;
};

// The small plugin-owned free list a provided surface is returned to when the compositor
// releases it (Decision 1). Held by `shared_ptr` and captured by the release callback, so
// it outlives the content whenever a `SurfaceRef` does -- doc 09:176-182 pins the surface
// until the compositor has composited or copied it, and that may outlast the content.
class TileStore {
public:
  // A surface of exactly (width, height), recycled from the free list when one of that
  // size is resting there. Never null.
  std::shared_ptr<TileSurface> acquire(int width, int height);

  // Return a released surface to the free list (bounded; the surplus is simply dropped).
  void release(std::shared_ptr<TileSurface> surface);

  // Behavioral counter: how many surfaces were allocated rather than recycled.
  std::uint64_t allocations() const noexcept;

private:
  static constexpr std::size_t k_max_resting = 8;

  mutable std::mutex d_mutex;
  std::vector<std::shared_ptr<TileSurface>> d_free;
  std::uint64_t d_allocations{0};
};

class ImageContent final : public Content {
public:
  static constexpr const char* kind_id = "org.arbc.image";

  // `authored_uri` is the reference EXACTLY AS THE DOCUMENT SPELLED IT -- the thing that
  // round-trips (Constraint 5). `pyramid` may be null: that is the unavailable state (and
  // also the PENDING one, which is minted in the same shape), and it is a perfectly
  // ordinary content, not an error (Decision 7).
  //
  // The RESOLVED URI is kept too, because a pending image's bytes arrive after
  // construction and `install_asset` must key the SAME `PyramidCache` entry the inline
  // path would have -- which is what makes a pending-then-settled image cost exactly ONE
  // decode, and N contents on one URI exactly one decode between them
  // (kinds.image_async_pending Decision 6). The overload without it builds a content with
  // no late-install identity: correct for a test that hands over an already-decoded
  // pyramid, and for any caller that will never defer.
  ImageContent(std::string authored_uri, PyramidPtr pyramid);
  ImageContent(std::string authored_uri, std::string resolved_uri, PyramidPtr pyramid);

  // --- Content (description) ---
  // The decoded master's extent, or an EMPTY rect when the asset is unavailable. Empty
  // rather than a fabricated placeholder rectangle: the intrinsic size is knowable only by
  // decoding, and Constraint 4 forbids caching it in the document, so there is literally no
  // rectangle to draw a placeholder over. Inventing one would let a MISSING file change the
  // composition's geometry (Decision 7, doc 08 Principle 3 as amended).
  std::optional<Rect> bounds() const override;
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }

  // --- Content (render) ---
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;
  // The pyramid is immutable and PUBLISHED EXACTLY ONCE, atomically, so a render is a pure
  // read of a pointer that is either null or final: the core may dispatch it to workers
  // (doc 00:203's leaf-only worker dispatch). This is the real win over imageseq, whose
  // stateful decoder + LRU force per-content serialization.
  //
  // The older argument -- "the pyramid is immutable AFTER CONSTRUCTION" -- was retired by
  // kinds.image_async_pending Decision 6, which gave the kind a late install. It is the
  // MONOTONICITY of that install (null -> non-null, never reverting, never replaced) that
  // keeps the flag true: a racing worker can observe only two values, and both are
  // self-consistent -- the empty state, which the compositor culls because bounds are
  // empty, and the finished pyramid. There is no intermediate, so there is no lock.
  bool render_thread_safe() const override { return true; }

  // --- Content (discovery) ---
  // `editable()` is deliberately NOT overridden (Constraint 2).
  std::string_view external_asset_ref() const override { return d_uri; }

  // The pixels of a PENDING asset, arriving after construction on the writer thread. One
  // release store of the decoded pyramid, at most once per content: a second arrival for a
  // content that already has pixels installs nothing and returns true. Empty or undecodable
  // bytes leave the content unavailable and return false -- a value, never a throw.
  bool install_asset(std::string_view encoded) override;

  // --- plugin-local observers ---
  bool available() const noexcept { return pyramid() != nullptr; }
  // BY VALUE, not by reference: the pointer publishes concurrently with a worker's render,
  // so every reader takes its own acquire-loaded copy.
  PyramidPtr pyramid() const noexcept { return d_pyramid.load(std::memory_order_acquire); }
  // The resolved identity the pyramid cache dedups this content's decode on. Empty when the
  // content was built without one (see the two-argument constructor).
  const std::string& resolved_ref() const noexcept { return d_resolved; }
  // How many surfaces the free list had to allocate rather than recycle.
  std::uint64_t tile_allocations() const noexcept { return d_tiles->allocations(); }

private:
  std::string d_uri;
  std::string d_resolved;
  // Neither a plain `shared_ptr` (a torn read across the late install) nor a mutex (which
  // would serialize every worker render behind a lock taken on a pointer that changes at
  // most once in the content's lifetime, on the hot path of the one leaf kind that has
  // none). An atomic is what keeps `render_thread_safe()` a promise rather than a hope.
  std::atomic<PyramidPtr> d_pyramid;
  std::shared_ptr<TileStore> d_tiles;
};

// The `ContentConfig` framing the core's serialize codec hands to this factory
// (Decision 5). `ContentConfig` is explicitly an opaque, kind-defined `string_view`
// (`registry.hpp:35`), so a length-delimited frame needs no ABI change:
//
//     "<authored-uri>\n<resolved-uri>\n<encoded-bytes>"
//
// split at the FIRST two newlines. A URI cannot contain a raw newline and `normalize_uri`
// is purely lexical, so the framing is unambiguous. Both URIs are carried because they
// answer different questions: the AUTHORED one is what round-trips into `params.source`
// (Constraint 5), while the RESOLVED one is the identity the pyramid cache dedups on (doc
// 08:116-122) -- `bg.png` and `./bg.png` are two authored refs and one decode.
//
// EMPTY bytes after the second `\n` mean the asset is unavailable: the factory still
// yields a content (URI kept, no pixels), because a missing external file is a condition of
// the environment, never a read error (Constraint 6). Only a MALFORMED FRAME -- a config
// that is not two-newline-delimited at all, which is a caller bug, not user data -- is an
// error value.
expected<std::unique_ptr<Content>, std::string> make_image_content(ContentConfig config);

// Build the `ContentConfig` frame above. Exposed so the codec in `runtime` and the tests
// agree on the framing by construction rather than by comment.
std::string image_config(std::string_view authored_uri, std::string_view resolved_uri,
                         std::string_view encoded_bytes);

} // namespace arbc::image
