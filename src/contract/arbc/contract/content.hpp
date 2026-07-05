#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/model/records.hpp> // StateHandle (L3->model edge, doc 17:53,68-72)
#include <arbc/surface/surface.hpp>

#include <optional>

namespace arbc {

// Cacheability of content output (docs 01/11).
enum class Stability {
  Static, // time-invariant; same request -> same pixels until damage
  Timed,  // deterministic function of request time
  Live,   // non-deterministic; cache only within a frame
};

// What the compositor wants rendered (doc 03): a region of content-local
// space, at a scale (device pixels per local unit), at a local time, over a
// pinned content-state snapshot. `snapshot` is the "revision fence" of the
// doc-03 sketch (03:50), resolved by doc 14's purity refinement to the
// content's captured `StateHandle` (14:181-187): the frozen state the request
// renders. It is an index-only, trivially-copyable slab handle (no refcount
// touch, no allocation, no store identity), so the request stays a cheap
// by-value descriptor built per render call. Defaults to `k_state_none`
// (`has_state() == false`): unpinned content renders live. Runtime binding
// (`model.content_binding`) populates non-none handles; the walking-skeleton
// compositor supplies the default.
struct RenderRequest {
  Rect region;
  double scale{1.0};
  Time time;
  StateHandle snapshot{};
  Surface& target;
};

struct RenderResult {
  double achieved_scale{1.0};
  bool exact{true};
};

// The layer contract (doc 03). Walking-skeleton subset: synchronous
// rendering only — the async completion path, the audio and editable
// facets, and the operator-graph members land with their systems.
class Content {
public:
  Content(const Content&) = delete;
  Content& operator=(const Content&) = delete;
  virtual ~Content();

  virtual std::optional<Rect> bounds() const = 0;
  virtual Stability stability() const = 0;
  // Render `request.region` into `request.target`. For content with editable
  // state, `render` must be a PURE function of `(request.snapshot, region,
  // scale, time)` (docs 03:138-140, 14:181-187): two calls with an identical
  // `RenderRequest` yield byte-identical target pixels, and `snapshot` is a
  // genuine input -- requests differing only in `snapshot` may yield different
  // pixels, since the handle names the frozen state to interpret. This purity
  // is what lets a cache key of (revision, region, scale, time) honestly
  // identify the pixels and lets render workers read frozen state while the
  // writer keeps editing (doc 14:159-162).
  virtual RenderResult render(const RenderRequest& request) = 0;

protected:
  Content() = default;
};

} // namespace arbc
