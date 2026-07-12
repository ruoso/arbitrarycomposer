#include <arbc/cache/invalidation.hpp>
#include <arbc/compositor/anchored_viewports.hpp>
#include <arbc/compositor/damage_planning.hpp>
#include <arbc/compositor/tile_planning.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace arbc {

namespace {

// A rect is finite iff every corner is finite. Structural damage encodes
// "whole object" as `Rect::infinite()`; any non-finite coordinate is that
// over-approximation and routes to the whole-content path here.
bool rect_is_finite(const Rect& r) {
  return std::isfinite(r.x0) && std::isfinite(r.y0) && std::isfinite(r.x1) && std::isfinite(r.y1);
}

// The half-open x-interval of a rect within one y-band: the sweep's working unit.
using XRun = std::pair<double, double>;

// Normalize the raw dirty rects into the sweep's input: viewport-clipped, rounded
// OUT to whole device pixels, empties dropped. Both steps are inherited from
// `repaint_region` and both are load-bearing (`disjoint_dirty_repaint` Constraint
// 1). Clipping FIRST is what keeps a structural `Rect::infinite()` from taking the
// round-out to a non-representable integer -- it saturates to the viewport instead.
// Rounding out BEFORE the decomposition is what makes the sweep's outputs integer
// by construction: every edge it emits is an edge of some input.
std::vector<Rect> integer_inputs(const DirtyRegion& dirty, const Rect& device_rect) {
  std::vector<Rect> inputs;
  inputs.reserve(dirty.device_rects.size());
  for (const Rect& device_dirty : dirty.device_rects) {
    const Rect clipped = device_dirty.intersect(device_rect);
    if (clipped.empty()) {
      continue;
    }
    const Rect rounded{std::floor(clipped.x0), std::floor(clipped.y0), std::ceil(clipped.x1),
                       std::ceil(clipped.y1)};
    // Re-intersect: the round-out of a rect already inside an integer viewport stays
    // inside it, so this only re-asserts the bound (and pins it against a viewport
    // with non-integer extent), exactly as `repaint_region` does.
    const Rect bounded = rounded.intersect(device_rect);
    if (!bounded.empty()) {
      inputs.push_back(bounded);
    }
  }
  return inputs;
}

// The x-runs of one y-band: the x-intervals of every input rect SPANNING the band,
// sorted and merged into disjoint runs. A rect either spans a band wholly or misses
// it entirely -- every band boundary is one of the rects' own y-edges, so no rect
// starts or ends inside a band. That is the property the whole sweep rests on: the
// band's coverage is exactly the union of those intervals, and merging them makes it
// a disjoint one.
void band_runs(const std::vector<Rect>& inputs, double y0, double y1, std::vector<XRun>& runs) {
  runs.clear();
  for (const Rect& r : inputs) {
    if (r.y0 <= y0 && r.y1 >= y1) {
      runs.emplace_back(r.x0, r.x1);
    }
  }
  if (runs.empty()) {
    return;
  }
  std::sort(runs.begin(), runs.end());
  std::size_t kept = 0;
  for (std::size_t at = 1; at < runs.size(); ++at) {
    if (runs[at].first <= runs[kept].second) { // overlapping or abutting: absorb
      runs[kept].second = std::max(runs[kept].second, runs[at].second);
    } else {
      runs[++kept] = runs[at];
    }
  }
  runs.resize(kept + 1);
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

Rect repaint_region(const DirtyRegion& dirty, const Viewport& viewport) {
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));
  Rect box{}; // empty accumulator (empty = identity under rect_union)
  for (const Rect& device_dirty : dirty.device_rects) {
    // Intersect BEFORE unioning: `Rect::infinite()` is absorbing under
    // `rect_union`, so an un-clipped structural rect would take the box to
    // infinity and the round-out below to a non-representable int.
    const Rect clipped = device_dirty.intersect(device_rect);
    if (clipped.empty()) {
      continue;
    }
    box = rect_union(box, clipped);
  }
  if (box.empty()) {
    return Rect{};
  }
  // Round out to whole device pixels, then re-intersect: the round-out of a rect
  // already inside an integer viewport stays inside it, so this only re-asserts
  // the bound (and pins it against a viewport with non-integer extent).
  const Rect rounded{std::floor(box.x0), std::floor(box.y0), std::ceil(box.x1), std::ceil(box.y1)};
  return rounded.intersect(device_rect);
}

std::vector<Rect> repaint_regions(const DirtyRegion& dirty, const Viewport& viewport) {
  const Rect device_rect =
      Rect::from_size(static_cast<double>(viewport.width), static_cast<double>(viewport.height));

  const std::vector<Rect> inputs = integer_inputs(dirty, device_rect);
  if (inputs.empty()) {
    return {}; // no damage -> no work (doc 02:51): every per-rect loop runs zero times
  }

  // The y-bands: the distinct y-edges of the inputs, sorted, taken pairwise as
  // half-open bands. Every rect's vertical extent is a whole number of bands.
  std::vector<double> edges;
  edges.reserve(2 * inputs.size());
  for (const Rect& r : inputs) {
    edges.push_back(r.y0);
    edges.push_back(r.y1);
  }
  std::sort(edges.begin(), edges.end());
  edges.erase(std::unique(edges.begin(), edges.end()), edges.end());

  std::vector<Rect> regions;
  std::vector<XRun> runs;
  std::vector<XRun> prev_runs; // the band immediately below's runs, for the coalesce
  std::size_t prev_at = 0;     // where that band's rects start in `regions`
  for (std::size_t band = 0; band + 1 < edges.size(); ++band) {
    const double y0 = edges[band];
    const double y1 = edges[band + 1];
    band_runs(inputs, y0, y1, runs);
    if (runs.empty()) {
      // A gap band -- two vertically separated damages. Nothing to emit, and the
      // coalesce must not reach ACROSS it: forget the band below.
      prev_runs.clear();
      continue;
    }

    // Vertical coalesce: a band whose runs are identical to the band directly below
    // extends those rects rather than emitting new ones. This is a pure rect-count
    // reduction -- it is what makes two plainly non-overlapping input rects come back
    // out as two rects instead of three bands, so the common case does not pay for the
    // machinery.
    if (runs == prev_runs) {
      for (std::size_t at = 0; at < runs.size(); ++at) {
        regions[prev_at + at].y1 = y1;
      }
      continue;
    }

    prev_at = regions.size();
    for (const XRun& run : runs) {
      regions.push_back(Rect{run.first, y0, run.second, y1});
    }
    prev_runs = runs;

    // The cap (Decision 3): a diagonal staircase of n rects decomposes into O(n^2)
    // bands x runs, which is worse than the bbox it replaces -- and the frame path has
    // a deadline. Abandon the sweep for `repaint_region`, which is not an approximation
    // but the shipped, byte-exact behavior whose union is a SUPERSET of this one's, so
    // no damage is ever missed. Checked as the set grows, so a pathological input costs
    // O(cap) emitted rects rather than the full O(n^2).
    if (regions.size() > k_max_repaint_rects) {
      return {repaint_region(dirty, viewport)};
    }
  }
  return regions;
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
