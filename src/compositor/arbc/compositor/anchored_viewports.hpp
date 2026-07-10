#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/model/model.hpp>

#include <functional>

// Doc 04 concretized: anchored viewports + rebasing. ArbitraryComposer has no
// global coordinate space -- the camera maps ANCHOR space (not root space) to
// device pixels, and as the user zooms the viewport re-anchors to a node in view
// so the transform the pipeline actually computes with stays permanently
// well-conditioned (doc 04:49-86). This header adds three coupled pieces over
// `arbc::compositor`:
//   1. the `Viewport::anchor` field (in `compositor.hpp`) -- the camera is now
//      the `(anchor node, matrix)` pair doc 04:81-84 mandates;
//   2. `rebase()` -- a PURE per-frame decision function that, when the composed
//      anchor->device scale leaves the well-conditioned band, re-picks the anchor
//      and rebuilds the camera relative to it (composed appearance preserved to
//      within one double rounding), reporting a host-visible re-anchor event;
//   3. `cull_walk()`/`render_frame_anchored()` -- the viewport-outward walk that
//      composes transforms from the anchor and culls a subtree the moment its
//      composed on-screen extent drops sub-pixel, never descending (doc 04:70-75).
//
// A pure per-frame library contribution (refinement Decision 2): no persistent
// state and no host-API surface. The compositor never holds the `Viewport`
// across frames; the caller (runtime, L5) owns the persistent camera, applies
// the `rebase()` result value, and surfaces the re-anchor event
// (`runtime.host_objects`).

namespace arbc {

// The root-anchor sentinel: the default (invalid) `ObjectId`. A viewport anchored
// here uses `render_frame`'s flat global walk (byte-identical to pre-anchor
// behavior, doc 04:56-61).
inline constexpr ObjectId k_root_anchor{};

// The composed anchor->device scale (`Affine::max_scale()`, the larger singular
// value) above which the camera has left the well-conditioned band and the
// viewport re-anchors to a DESCENDANT (zoom in), and below whose reciprocal it
// re-anchors to an ANCESTOR (zoom out). Doc 04:63-66 writes "(say 2^16)" --
// explicitly illustrative, so this is a NAMED TUNABLE, not designed behavior
// (refinement Decision 3): the tests pin the observable invariants (appearance
// preserved, conditioning bounded, sub-pixel culled), never this literal. Sibling
// of `k_tile_size` / `k_max_fallback_octaves` "a later task can tune".
inline constexpr double k_reanchor_scale_threshold = 65536.0; // 2^16

// A nested-composition subtree whose composed on-screen extent falls below this
// many device pixels contributes nothing and is culled WITHOUT descending (doc
// 04:70-75) -- the viewport-outward walk's structural bound on depth. This is the
// subtree analog of `render_layer`'s per-leaf zero-pixel cull, at the coarser
// granularity of a whole subtree's mapped extent; like `k_reanchor_scale_threshold`
// it is a named tunable, and the tests pin the invariant (a sub-pixel subtree
// emits zero requests), not this literal.
inline constexpr double k_subpixel_cull_extent = 1.0; // device pixels

// Which way the conditioning test says to re-anchor this frame.
enum class RebaseNeed { none, zoom_in, zoom_out };

// Pure conditioning test on the composed anchor->device scale. A degenerate or
// non-finite scale is a cull concern (doc 04:115-117), not a rebase, so it
// reports `none`.
RebaseNeed rebase_need(double anchor_to_device_scale);

// The re-anchor event value (doc 04:81-84): the host-visible old->new anchor
// switch `runtime.host_objects` surfaces. `occurred == false` leaves from/to
// unset.
struct Reanchor {
  bool occurred{false};
  ObjectId from{};
  ObjectId to{};
};

// The value a single rebase step returns (refinement Decision 2): the
// possibly-rebased viewport, the re-anchor event, and the conditioning `need`
// observed this frame. The caller (runtime) owns the persistent camera across
// frames and applies this value.
struct RebaseResult {
  Viewport viewport;
  Reanchor event;
  RebaseNeed need{RebaseNeed::none};
  // The descent edge applied on a zoom-in re-anchor (`runtime.host_objects`
  // Decision 4): the child layer's transform mapping NEW-anchor-local ->
  // old-anchor-local, i.e. the exact matrix this step composed into the camera.
  // The runtime-held anchor path stores it so zoom-out inverts THAT matrix
  // (`reanchor_camera(camera, edge.inverse())`, doc 04:62-69), making the
  // round-trip appearance-preserving to within one double rounding. Identity
  // when no re-anchor occurred (`event.occurred == false`).
  Affine edge{};
};

// Rebuild the camera when re-anchoring across `edge`, where `edge` maps
// NEW-anchor-local -> CURRENT-anchor-local space (doc 04:62-69). Returns the
// camera mapping new-anchor-local -> device: `compose(camera, edge)`. Recomputed
// from the stored per-edge matrix, never an incremental patch (doc 04:44-47), so
// the on-screen image is preserved exactly to within one double rounding:
//   compose(camera, edge).apply(p) == camera.apply(edge.apply(p)).
// Descendant re-anchor (zoom in) passes the child layer's transform; ancestor
// re-anchor (zoom out) passes the current anchor edge's inverse.
Affine reanchor_camera(const Affine& camera, const Affine& edge);

// Single-step viewport rebase (doc 04:62-69). If the composed anchor->device
// scale has left the well-conditioned band and a DESCENDANT node in view is
// reachable through the composition graph in `state`, re-anchor to it and return
// the rebased viewport + host-visible event; otherwise return `viewport`
// unchanged with `event.occurred == false`. `need` reports the conditioning test
// regardless.
//
// Zoom-OUT ancestor selection walks the runtime-held anchor path (the persistent
// cross-frame anchor state is L5, refinement Decision 2 / deferral to
// `runtime.host_objects`), so this pure per-frame step reports `need ==
// zoom_out` and leaves the viewport for the caller to re-anchor upward via
// `reanchor_camera(camera, ancestor_edge.inverse())`. Depth-agnostic: it descends
// whatever composition chain the graph exposes, needing no change as
// nested-composition rendering lands (`compositor.operator_graph`, Decision 5).
// Pure: reads the pinned `state`, mutates nothing.
RebaseResult rebase(const DocRoot& state, const Viewport& viewport);

// Viewport-outward cull walk (doc 04:70-75). Visits each visible leaf layer
// reachable from `viewport.anchor`, composing anchor->device transforms outward,
// and prunes any nested-composition subtree whose composed placement is
// off-viewport or sub-pixel BEFORE descending (so it issues no request for it).
//
//  - `viewport.anchor == k_root_anchor`: the flat walking-skeleton scene -- every
//    layer in global `for_each_layer` order composed with the camera
//    (backward-compat: byte-identical layer set/order to `render_frame`).
//  - a composition id: walk that composition's membership
//    (`for_each_layer_in`), descending nested compositions (`find_composition`)
//    until it reaches leaf layers, pruning sub-pixel/off-view subtrees.
//
// Pure: reads `state`, emits via `visit`, mutates nothing. Depth-agnostic
// (Decision 5).
void cull_walk(const DocRoot& state, const Viewport& viewport,
               const std::function<void(const LayerRecord& layer, const Affine& composed)>& visit);

// The anchored frame driver: `render_frame` generalized over `viewport.anchor`.
// Drives the surviving leaves `cull_walk` emits through the SAME per-layer path
// `render_frame` uses (`render_layer`), so `anchor == k_root_anchor` is
// byte-identical to `render_frame` (refinement byte-exact golden).
void render_frame_anchored(const DocRoot& state, const ContentResolver& resolve,
                           const Viewport& viewport, Backend& backend, SurfacePool& pool,
                           Surface& target);

} // namespace arbc
