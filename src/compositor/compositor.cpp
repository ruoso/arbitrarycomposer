#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/provided_surface.hpp>

#include <cmath>

namespace arbc {

void render_layer(const ContentResolver& resolve, const LayerRecord& layer, const Affine& composed,
                  const Rect& device_rect, Backend& backend, SurfacePool& pool, Surface& target) {
  if (!layer.visible() || layer.opacity <= 0.0) {
    return;
  }
  Content* content = resolve(layer.content);
  if (content == nullptr) {
    return;
  }

  // Compose per-edge transforms on demand (doc 04): the caller already composed
  // local -> device; invert it to map the device region back into local space.
  const std::optional<Affine> inv = composed.inverse();
  if (!inv.has_value()) {
    return; // degenerate placement: cull (doc 04)
  }

  // The pull contract (doc 03): map the visible device region into
  // content-local space, intersect with declared bounds, and request
  // exactly that region at the composed scale.
  Rect region = inv->map_rect(device_rect);
  if (const std::optional<Rect> bounds = content->bounds(); bounds.has_value()) {
    region = region.intersect(*bounds);
  }
  if (region.empty()) {
    return;
  }

  const double scale = composed.max_scale();
  if (!(scale > 0.0) || !std::isfinite(scale)) {
    return;
  }
  const int temp_width = static_cast<int>(std::ceil(region.width() * scale));
  const int temp_height = static_cast<int>(std::ceil(region.height() * scale));
  if (temp_width <= 0 || temp_height <= 0) {
    return; // sub-pixel: cull (doc 04)
  }

  // Acquire the per-layer temp from the pool (doc 09): a free-list hit
  // recycles a released same-key surface, a miss allocates through the
  // backend. The handle's lambda-body scope releases it back to the pool.
  expected<PooledSurface, SurfaceError> temp_result =
      pool.acquire(temp_width, temp_height, target.format());
  if (!temp_result.has_value()) {
    return; // backend cannot store the target's working format: cull (doc 09)
  }
  // Recycled surfaces carry undefined contents (doc 09): clear before use so
  // output stays byte-identical regardless of a prior frame's residue.
  Surface& temp = temp_result->get();
  backend.clear(temp, 0.0F, 0.0F, 0.0F, 0.0F);

  // No pinned DocState is threaded through `compose()` in the walking
  // skeleton (Time::zero() is hard-coded the same way): supply the inert
  // default snapshot explicitly. Resolving `content_state(id)` from the
  // frame's pinned version rides `model.content_binding` + the runtime
  // renderers (refinement Decision 2).
  const RenderRequest request{region, scale, Time::zero(), StateHandle{}, temp};

  // One code path (doc 03:117-121): drive the render through a
  // `RenderCompletion`. A returned-inline result is folded in via
  // `complete` and read back through `take` exactly as a deferred settlement
  // would be, so composite always reads the single settle path.
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> inline_result = content->render(request, done);
  if (!inline_result.has_value()) {
    // Async content answered `nullopt`: the synchronous single pass cannot
    // service a pending completion (holding `temp` alive across a frame loop
    // is runtime.interactive, doc 17:39-41). Release `temp` and skip this
    // layer this pass.
    return;
  }
  done->complete(*inline_result);
  const std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return; // inline content failed to produce pixels: cull this layer
  }
  RenderResult result = **settled;

  // temp pixel (i, j) covers local (region origin + (i, j) / achieved):
  // map temp space through local space to device space. A content-provided
  // surface covers the same region at the same achieved scale (doc 09:122-124),
  // so it rides the identical mapping.
  const Affine temp_to_dst = compose(
      composed, compose(Affine::translation(region.x0, region.y0),
                        Affine::scaling(1.0 / result.achieved_scale, 1.0 / result.achieved_scale)));
  // Honor a content-provided surface (doc 09:87-100): composite directly FROM
  // it -- zero copy, the exact inline-path win doc 09 targets (09:122-124) --
  // instead of the pooled temp, and release it right after; the temp is returned
  // to the pool untouched (09:80). Absent, this composites the filled temp
  // exactly as before.
  consume_render_result(result, temp, [&](const Surface& src) {
    backend.composite(target, src, temp_to_dst, layer.opacity);
  });
}

void render_frame(const DocRoot& state, const ContentResolver& resolve, const Viewport& viewport,
                  Backend& backend, SurfacePool& pool, Surface& target) {
  backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);

  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  // Bottom-to-top over the pinned version's layers (doc 02). Compose the camera
  // with each layer's per-edge transform on demand (doc 04) and hand the layer
  // to the shared per-layer body; `render_layer` culls the current layer by
  // returning early.
  state.for_each_layer([&](const LayerRecord& layer) {
    const Affine composed = compose(viewport.camera, layer.transform);
    render_layer(resolve, layer, composed, device_rect, backend, pool, target);
  });
}

} // namespace arbc
