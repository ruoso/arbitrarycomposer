#include <arbc/compositor/anchored_viewports.hpp>

#include <cmath>
#include <optional>

namespace arbc {
namespace {

// Does the subtree anchored by a nested composition of `canvas_w x canvas_h`
// local extent, placed by `composed` (its local -> device), fall off the
// viewport or below the sub-pixel extent? Mirrors `render_layer`'s cull sequence
// (degenerate inverse cull, empty-region cull, non-finite scale cull) at the
// coarser granularity of the whole composition canvas, so pruning here matches
// the outward-walk contract (doc 04:70-75) and the doc 04:115-117 no-NaN
// guarantee at the subtree level.
bool subtree_culled(const Affine& composed, double canvas_w, double canvas_h,
                    const Rect& device_rect) {
  const std::optional<Affine> inv = composed.inverse();
  if (!inv.has_value()) {
    return true; // degenerate placement: cull without descending (doc 04)
  }
  const Rect region = inv->map_rect(device_rect).intersect({0.0, 0.0, canvas_w, canvas_h});
  if (region.empty()) {
    return true; // wholly outside the viewport: cull
  }
  const double scale = composed.max_scale();
  if (!(scale > 0.0) || !std::isfinite(scale)) {
    return true; // scale-~0 or non-finite subtree: cull (doc 04:115-117)
  }
  // Sub-pixel: the visible extent of the subtree maps to under one device pixel
  // on either axis (doc 04:70-75). Culled BEFORE descending, so no leaf under it
  // is reached and no request is emitted.
  return region.width() * scale < k_subpixel_cull_extent ||
         region.height() * scale < k_subpixel_cull_extent;
}

// Recursive descent from a composition node: compose each member's per-edge
// transform on demand, prune off-view/sub-pixel nested-composition subtrees, and
// emit leaf layers. Depth-agnostic (Decision 5).
void walk_composition(const DocRoot& state, ObjectId composition, const Affine& to_device,
                      const Rect& device_rect,
                      const std::function<void(const LayerRecord&, const Affine&)>& visit) {
  state.for_each_layer_in(composition, [&](ObjectId member) {
    const LayerRecord* layer = state.find_layer(member);
    if (layer == nullptr) {
      return;
    }
    const Affine composed = compose(to_device, layer->transform);
    if (const CompositionRecord* child = state.find_composition(layer->content); child != nullptr) {
      // A nested composition: honor the group layer's visibility/opacity, then
      // prune the whole subtree if off-view/sub-pixel, else descend into it.
      if (!layer->visible() || layer->opacity <= 0.0) {
        return;
      }
      if (subtree_culled(composed, child->canvas_w, child->canvas_h, device_rect)) {
        return;
      }
      walk_composition(state, layer->content, composed, device_rect, visit);
      return;
    }
    // A leaf layer: emit it; the visitor applies `render_layer`'s own per-leaf
    // predicates (visibility, bounds, zero-pixel cull).
    visit(*layer, composed);
  });
}

} // namespace

RebaseNeed rebase_need(double anchor_to_device_scale) {
  const double s = anchor_to_device_scale;
  if (!(s > 0.0) || !std::isfinite(s)) {
    return RebaseNeed::none; // a degenerate composed scale culls, it does not rebase
  }
  if (s > k_reanchor_scale_threshold) {
    return RebaseNeed::zoom_in;
  }
  if (s < 1.0 / k_reanchor_scale_threshold) {
    return RebaseNeed::zoom_out;
  }
  return RebaseNeed::none;
}

Affine reanchor_camera(const Affine& camera, const Affine& edge) { return compose(camera, edge); }

RebaseResult rebase(const DocRoot& state, const Viewport& viewport) {
  RebaseResult result{viewport, {}, rebase_need(viewport.camera.max_scale())};
  if (result.need != RebaseNeed::zoom_in) {
    // In-band or degenerate: nothing to do. Zoom-out re-anchors to an ancestor,
    // which needs the runtime-held anchor path (persistent L5 state, Decision 2);
    // the reported `need == zoom_out` signals the caller to pop that path and
    // re-anchor upward via `reanchor_camera`.
    return result;
  }
  if (viewport.anchor == k_root_anchor) {
    return result; // the flat global walk has no anchored descendant to pick
  }

  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  // Pick the in-view descendant COMPOSITION with the largest composed scale --
  // the node the user is zooming into (doc 04:62-64). Only nested compositions
  // are re-anchor targets: a leaf layer is not a coordinate frame the camera can
  // pin to, so the reachable structure bounds how deep rebasing can go.
  ObjectId best{};
  Affine best_camera{};
  Affine best_edge{};
  double best_scale = -1.0;
  state.for_each_layer_in(viewport.anchor, [&](ObjectId member) {
    const LayerRecord* layer = state.find_layer(member);
    if (layer == nullptr || !layer->visible() || layer->opacity <= 0.0) {
      return;
    }
    const CompositionRecord* child = state.find_composition(layer->content);
    if (child == nullptr) {
      return;
    }
    const Affine composed = reanchor_camera(viewport.camera, layer->transform);
    if (subtree_culled(composed, child->canvas_w, child->canvas_h, device_rect)) {
      return;
    }
    const double child_scale = composed.max_scale();
    if (child_scale > best_scale) {
      best_scale = child_scale;
      best = layer->content;
      best_camera = composed;
      best_edge = layer->transform; // the descent edge the anchor path stores (Decision 4)
    }
  });

  if (!best.valid()) {
    return result; // structure-bounded: no in-view descendant to re-anchor into
  }
  result.viewport.camera = best_camera; // == reanchor_camera(camera, child edge)
  result.viewport.anchor = best;
  result.edge = best_edge; // NEW-anchor-local -> old-anchor-local; zoom-out inverts it
  result.event = Reanchor{true, viewport.anchor, best};
  return result;
}

void cull_walk(const DocRoot& state, const Viewport& viewport,
               const std::function<void(const LayerRecord& layer, const Affine& composed)>& visit) {
  if (viewport.anchor == k_root_anchor) {
    // Flat walking-skeleton scene: global layer order, byte-identical layer
    // set/order to `render_frame` (doc 04:56-61 backward-compat). No structural
    // pruning -- the visitor's per-leaf predicates own the cull.
    state.for_each_layer(
        [&](const LayerRecord& layer) { visit(layer, compose(viewport.camera, layer.transform)); });
    return;
  }
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));
  walk_composition(state, viewport.anchor, viewport.camera, device_rect, visit);
}

void render_frame_anchored(const DocRoot& state, const ContentResolver& resolve,
                           const Viewport& viewport, Backend& backend, SurfacePool& pool,
                           Surface& target) {
  backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));
  cull_walk(state, viewport, [&](const LayerRecord& layer, const Affine& composed) {
    render_layer(resolve, layer, composed, device_rect, backend, pool, target);
  });
}

} // namespace arbc
