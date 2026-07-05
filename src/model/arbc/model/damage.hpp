#pragma once

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
  Time start{};
  Time end{};

  friend bool operator==(const Damage&, const Damage&) = default;
};

// Bounding-box union of two rects; an empty rect contributes nothing (so a
// coarse object-keyed damage with a default rect leaves a real rect intact).
inline Rect rect_union(const Rect& a, const Rect& b) {
  if (a.empty()) {
    return b;
  }
  if (b.empty()) {
    return a;
  }
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

// Accumulate `d` into `set`, unioning by object id (dedup): a matching object's
// rect becomes the bounding-box union and its time range the [min start, max
// end]. This is the union/dedup the flush-once-per-commit contract rides on
// (14-data-model-and-editing#damage-flushes-once-per-commit).
inline void damage_add(std::vector<Damage>& set, const Damage& d) {
  for (Damage& e : set) {
    if (e.object == d.object) {
      e.rect = rect_union(e.rect, d.rect);
      e.start = std::min(e.start, d.start);
      e.end = std::max(e.end, d.end);
      return;
    }
  }
  set.push_back(d);
}

// Abstract, model-defined damage seam (pure change-notification, doc 02): a
// commit flushes the union of its per-mutation damage here exactly once; abort
// flushes nothing. The concrete consumer (model.damage propagation, a viewport
// invalidator) is wired from above at level L3 (doc 17:66-72).
class DamageSink {
public:
  virtual ~DamageSink() = default;
  virtual void flush(const std::vector<Damage>& damage) = 0;
};

} // namespace arbc
