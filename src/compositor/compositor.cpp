#include <arbc/compositor/compositor.hpp>

#include <cmath>

namespace arbc {

void render_frame(const DocRoot& state, const ContentResolver& resolve, const Viewport& viewport,
                  Backend& backend, SurfacePool& pool, Surface& target) {
  backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);

  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  // Bottom-to-top over the pinned version's layers (doc 02). `return` culls the
  // current layer (the per-layer body is a callback, not an inline loop).
  state.for_each_layer([&](const LayerRecord& layer) {
    if (!layer.visible() || layer.opacity <= 0.0) {
      return;
    }
    Content* content = resolve(layer.content);
    if (content == nullptr) {
      return;
    }

    // Compose per-edge transforms on demand (doc 04): local -> device.
    const Affine composed = compose(viewport.camera, layer.transform);
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

    const RenderRequest request{region, scale, Time::zero(), temp};
    const RenderResult result = content->render(request);

    // temp pixel (i, j) covers local (region origin + (i, j) / achieved):
    // map temp space through local space to device space.
    const Affine temp_to_dst =
        compose(composed,
                compose(Affine::translation(region.x0, region.y0),
                        Affine::scaling(1.0 / result.achieved_scale, 1.0 / result.achieved_scale)));
    backend.composite(target, temp, temp_to_dst, layer.opacity);
  });
}

} // namespace arbc
