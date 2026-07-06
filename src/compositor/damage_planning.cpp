#include <arbc/compositor/damage_planning.hpp>

#include <arbc/cache/invalidation.hpp>
#include <arbc/compositor/anchored_viewports.hpp>
#include <arbc/compositor/tile_planning.hpp>

#include <cmath>

namespace arbc {

namespace {

// A rect is finite iff every corner is finite. Structural damage encodes
// "whole object" as `Rect::infinite()`; any non-finite coordinate is that
// over-approximation and routes to the whole-content path here.
bool rect_is_finite(const Rect& r) {
  return std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) && std::isfinite(r.y1);
}

} // namespace

std::vector<Rect> map_damage_to_device(const DocRoot& state, const Viewport& viewport,
                                       std::span<const Damage> damage, Time now) {
  std::vector<Rect> device_rects;
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  for (const Damage& d : damage) {
    // Temporal gate (Decision 3): an empty/degenerate range (the refinement
    // arrival `TimeRange{when, when}`) is present-frame damage; a real range
    // that excludes the displayed instant contributes nothing.
    if (!(d.range.empty() || d.range.contains(now))) {
      continue;
    }
    // Locate every visible layer that shows this content and project the damage
    // rect through its composed local->device transform. `cull_walk` composes
    // the anchor->device transform outward and already handles the anchored /
    // flat cases; the visitor applies the per-leaf visibility predicates.
    cull_walk(state, viewport, [&](const LayerRecord& layer, const Affine& composed) {
      if (!(layer.content == d.object) || !layer.visible() || layer.opacity <= 0.0) {
        return;
      }
      Rect dev;
      if (!rect_is_finite(d.rect)) {
        // Structural infinite damage: the object's device footprint clipped to
        // the viewport -- conservatively the whole viewport rect (refinement 1a;
        // this signature carries no resolver to tighten to `bounds()`).
        dev = device_rect;
      } else {
        dev = composed.map_rect(d.rect).intersect(device_rect);
      }
      if (!dev.empty()) {
        device_rects.push_back(dev);
      }
    });
  }
  return device_rects;
}

std::vector<Damage> clock_advance_damage(const DocRoot& state, const ContentResolver& resolve,
                                         const Viewport& viewport, const TimeRange& advanced) {
  std::vector<Damage> damage;
  if (advanced.empty()) {
    return damage; // an empty advance damages nothing -- no work (doc 02:51)
  }
  cull_walk(state, viewport, [&](const LayerRecord& layer, const Affine& /*composed*/) {
    if (!layer.visible() || layer.opacity <= 0.0) {
      return;
    }
    Content* content = resolve(layer.content);
    if (content == nullptr) {
      return;
    }
    // Static layers' cached tiles remain valid across a clock advance (doc
    // 11:133-137; 11-time-and-video#static-tiles-survive-clock), so they emit
    // no temporal damage; Timed/Live layers dirty their whole visible footprint.
    if (content->stability() == Stability::Static) {
      return;
    }
    damage_add(damage, Damage{layer.content, Rect::infinite(), advanced});
  });
  return damage;
}

std::size_t invalidate_damage(TileCache& cache, std::span<const Damage> damage) {
  std::size_t dropped = 0;
  for (const Damage& d : damage) {
    if (!rect_is_finite(d.rect)) {
      // Structural infinite region intersects every tile: route explicitly to
      // the wholesale drop rather than an all-pairs geometry test (Decision 6).
      dropped += cache::invalidate_content(cache, d.object);
    } else {
      // Bind the caller-injected `Geom` to the tile grid: drop every tile of
      // `object` whose content-space footprint intersects `rect`, across all
      // rungs/revisions/achieved-times (doc 02:94-95).
      dropped += cache::invalidate_region(cache, d.object, d.rect, tile_local_rect);
    }
  }
  return dropped;
}

} // namespace arbc
