#pragma once

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
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
// space, at a scale (device pixels per local unit), at a local time.
struct RenderRequest {
  Rect region;
  double scale{1.0};
  Time time;
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
  virtual RenderResult render(const RenderRequest& request) = 0;

protected:
  Content() = default;
};

} // namespace arbc
