#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>

#include <algorithm>
#include <vector>

namespace arbc {

// The minimal damage value (doc 14:92-94): an object id, the invalidated
// spatial region, and the invalidated time range. All fields are `arbc::base`
// value types, so `Damage` is model-owned but level-clean. `model.transactions`
// owns this value + the per-transaction accumulator + the `DamageSink`;
// `model.damage` reuses it and adds auto-damage-on-placement and up-nesting
// propagation on top (doc 14 Decisions). This is a TRANSIENT handle type
// (holds STL), never an in-arena record.
struct Damage {
  ObjectId object{};
  Rect rect{};
  TimeRange range{};

  friend bool operator==(const Damage&, const Damage&) = default;
};

// Bounding-box union of two rects. An **empty** rect contributes nothing
// (identity), so a coarse object-keyed damage with a default rect leaves a real
// rect intact; an **infinite** rect (`Rect::infinite()`) is absorbing (min/max
// with the infinities), so a whole-object structural marker soundly widens the
// union to everything. Model emits infinite rects for structural damage and
// finite ones never for the same object id, so the two never clobber.
inline Rect rect_union(const Rect& a, const Rect& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// Half-open union of two time ranges under the same convention as `rect_union`:
// an **empty** range (`end <= start`) is identity -- so a degenerate default no
// longer folds a real range toward instant 0 -- and `TimeRange::all()` is
// absorbing (min-start / max-end reach the ends). Structural damage carries
// `all()`; content damage carries the caller's finite range.
inline TimeRange range_union(const TimeRange& a, const TimeRange& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  return {Time{std::min(a.start.flicks, b.start.flicks)},
          Time{std::max(a.end.flicks, b.end.flicks)}};
}

// Accumulate `d` into `set`, unioning by object id (dedup): a matching object's
// rect becomes the bounding-box union and its range the half-open union, both
// under empty=identity / whole=absorbing. This is the union/dedup the
// flush-once-per-commit contract rides on
// (14-data-model-and-editing#damage-flushes-once-per-commit).
inline void damage_add(std::vector<Damage>& set, const Damage& d) {
  for (Damage& e : set) {
    if (e.object == d.object) {
      e.rect = rect_union(e.rect, d.rect);
      e.range = range_union(e.range, d.range);
      return;
    }
  }
  set.push_back(d);
}

// Abstract, model-defined damage seam (pure change-notification, doc 02): a
// commit flushes the union of its per-mutation damage here exactly once; abort
// flushes nothing. The concrete consumer (model.damage propagation, a viewport
// invalidator) is wired from above at level L3 (doc 17:66-72).
class ARBC_API DamageSink {
public:
  virtual ~DamageSink() = default;
  virtual void flush(const std::vector<Damage>& damage) = 0;
};

} // namespace arbc
