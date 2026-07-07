#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/tile_planning.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

// Temporal-placement consumption in the interactive compositor (doc 11:60-73,
// 122-124, 185-191): the `render_frame_interactive` per-layer walk culls a layer
// whose half-open `span` does not contain the frame's composition time (before
// any content resolve / render) and evaluates the layer's `time_map` at the
// composition time to the content-local time it requests. Span culling is pinned
// behaviorally through `CompositorCounters` (never wall-clock); the retiming is
// pinned by a `Timed` test content that records the request `time` it received.

namespace {

using arbc::CompositorCounters;
using arbc::Rational;
using arbc::RenderResult;
using arbc::Stability;
using arbc::Time;
using arbc::TimeMap;
using arbc::TimeRange;
using arbc::TileCache;

// A `Timed` test content that records the request `time` the compositor issued.
// `quantize_time` is identity (returns the time unchanged, not nullopt) so the
// tile key's `achieved_time` and the recorded time both equal the content-local
// time the compositor computed -- the retiming is observable directly.
class RecordingTimedContent : public arbc::Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<Time> quantize_time(Time t) const override { return t; }
  std::optional<RenderResult> render(const arbc::RenderRequest& request,
                                     std::shared_ptr<arbc::RenderCompletion> /*done*/) override {
    last_time = request.time;
    render_count += 1;
    RenderResult result{request.scale, /*exact=*/true};
    result.achieved_time = request.time; // identity quantize: key == achieved_time
    return result;
  }

  Time last_time{std::numeric_limits<std::int64_t>::min()}; // sentinel: never rendered
  int render_count{0};
};

// A CPU-buffer surface + a byte-touching backend so the driver's composite path
// is real; the counts, not the bytes, are under test here.
class BufferSurface : public arbc::Surface {
public:
  BufferSurface(int width, int height)
      : d_width(width), d_height(height),
        d_bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 16,
                std::byte{0}) {}
  int width() const override { return d_width; }
  int height() const override { return d_height; }
  arbc::SurfaceFormat format() const override { return arbc::k_working_rgba32f; }
  std::span<std::byte> cpu_bytes() override { return d_bytes; }
  std::span<const std::byte> cpu_bytes() const override { return d_bytes; }

private:
  int d_width;
  int d_height;
  std::vector<std::byte> d_bytes;
};

class MarkBackend : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int width, int height, arbc::SurfaceFormat /*format*/) override {
    return std::unique_ptr<arbc::Surface>(std::make_unique<BufferSurface>(width, height));
  }
  void clear(arbc::Surface& /*surface*/, float /*r*/, float /*g*/, float /*b*/,
             float /*a*/) override {}
  void composite(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/, const arbc::Affine& /*m*/,
                 double /*opacity*/) override {}
  void downsample(arbc::Surface& /*dst*/, const arbc::Surface& /*src*/) override {}
};

// A single identity-transform layer bound to a fresh content id, with the given
// temporal placement (`span` / `time_map`) set through the transactional setters.
arbc::ObjectId add_placed_layer(arbc::Model& model, const TimeRange& span, const TimeMap& time_map) {
  auto txn = model.transact();
  const arbc::ObjectId content_id = txn.add_content(0);
  const arbc::ObjectId layer = txn.add_layer(content_id, arbc::Affine::identity());
  txn.set_span(layer, span);
  txn.set_time_map(layer, time_map);
  REQUIRE(txn.commit().has_value());
  return content_id;
}

// Drive one interactive frame at `composition_time` against a fresh cache into
// the caller's `counters`, read as absolute values (cold cache -> a present layer
// renders exactly once).
void drive_frame(const arbc::DocRoot& state, const arbc::ContentResolver& resolver,
                 Time composition_time, CompositorCounters& counters) {
  MarkBackend backend;
  // A 256x256 viewport over the 512x512 content -> exactly one rung-0 tile.
  const arbc::Viewport viewport{256, 256, arbc::Affine::identity()};
  arbc::SurfacePool pool(backend);
  TileCache cache(64u * 1024 * 1024);
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> target =
      backend.make_surface(256, 256, arbc::k_working_rgba32f);
  REQUIRE(target.has_value());
  arbc::render_frame_interactive(state, resolver, viewport, cache, backend, pool, **target,
                                 arbc::Deadline::none(), std::nullopt, nullptr, &counters, nullptr,
                                 composition_time);
}

} // namespace

// enforces: 11-time-and-video#span-cull-is-half-open
TEST_CASE("compositor culls a layer half-open on its span, issuing zero renders when absent") {
  // Span [in, out) = [100, 200) in composition time.
  const Time in{100};
  const Time out{200};
  RecordingTimedContent content;
  arbc::Model model;
  const arbc::ObjectId content_id = add_placed_layer(model, TimeRange{in, out}, TimeMap{});
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };

  // in-1: before the span -> absent. A culled layer resolves no content, issues
  // no render, and emits no composite (the performance face of "outside span ->
  // skip", doc 11:21,72).
  {
    CompositorCounters c;
    drive_frame(*state, resolver, Time{in.flicks - 1}, c);
    CHECK(c.requests_issued() == 0);
    CHECK(c.composites() == 0);
    CHECK(c.operator_renders() == 0);
  }
  // in: the lower bound is INCLUDED -> present, renders and composites.
  {
    CompositorCounters c;
    drive_frame(*state, resolver, in, c);
    CHECK(c.requests_issued() == 1);
    CHECK(c.composites() == 1);
  }
  // out-1: the last present instant -> present.
  {
    CompositorCounters c;
    drive_frame(*state, resolver, Time{out.flicks - 1}, c);
    CHECK(c.requests_issued() == 1);
    CHECK(c.composites() == 1);
  }
  // out: the upper bound is EXCLUDED (half-open) -> absent, zero renders.
  {
    CompositorCounters c;
    drive_frame(*state, resolver, out, c);
    CHECK(c.requests_issued() == 0);
    CHECK(c.composites() == 0);
    CHECK(c.operator_renders() == 0);
  }
}

// enforces: 11-time-and-video#span-cull-is-half-open
TEST_CASE("compositor: default all() span is always present (a still is never culled)") {
  RecordingTimedContent content;
  arbc::Model model;
  // Default temporal placement: all()-span, identity time_map -- a still.
  const arbc::ObjectId content_id = add_placed_layer(model, TimeRange::all(), TimeMap{});
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  // Present at a wildly negative and a large positive instant alike.
  for (const Time t : {Time{-1'000'000}, Time{0}, Time{999'000'000}}) {
    CompositorCounters c;
    drive_frame(*state, resolver, t, c);
    CHECK(c.requests_issued() == 1);
    CHECK(c.composites() == 1);
  }
}

// enforces: 11-time-and-video#compositor-retimes-request-through-time-map
TEST_CASE("compositor requests content at the time_map-evaluated content-local time") {
  // Forward rate != 1 with non-zero in/offset: local = (t - in) * rate + offset.
  SECTION("forward rate 2, non-zero in and offset") {
    RecordingTimedContent content;
    arbc::Model model;
    // in=10, rate=2/1, offset=3, driven at t=20 -> local = (20-10)*2+3 = 23.
    const TimeMap map{Time{10}, Rational(2, 1), Time{3}};
    const arbc::ObjectId content_id = add_placed_layer(model, TimeRange::all(), map);
    const arbc::DocStatePtr state = model.current();
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == content_id ? &content : nullptr;
    };
    CompositorCounters c;
    drive_frame(*state, resolver, Time{20}, c);
    REQUIRE(content.render_count == 1);
    CHECK(content.last_time == Time{23});
  }

  // Negative rate: reverse playback -- advancing composition time requests an
  // EARLIER content-local time. in=0, rate=-1/1, offset=100.
  SECTION("negative rate reverses (advancing parent time -> decreasing local time)") {
    RecordingTimedContent content;
    arbc::Model model;
    const TimeMap map{Time{0}, Rational(-1, 1), Time{100}};
    const arbc::ObjectId content_id = add_placed_layer(model, TimeRange::all(), map);
    const arbc::DocStatePtr state = model.current();
    const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
      return id == content_id ? &content : nullptr;
    };
    // t=30 -> local = (30-0)*-1+100 = 70; t=40 -> local = 60 (earlier: reversed).
    CompositorCounters c30;
    drive_frame(*state, resolver, Time{30}, c30);
    REQUIRE(content.render_count == 1);
    CHECK(content.last_time == Time{70});

    CompositorCounters c40;
    drive_frame(*state, resolver, Time{40}, c40);
    CHECK(content.last_time == Time{60}); // advanced parent -> earlier local time
  }
}

// enforces: 11-time-and-video#compositor-retimes-request-through-time-map
TEST_CASE("compositor culls a layer whose time_map overflows at the driven instant") {
  RecordingTimedContent content;
  arbc::Model model;
  // A rate at INT64_MAX overflows the int64 flick width for any non-trivial
  // parent time (Decision D3: a layer that cannot be temporally placed is culled,
  // never rendered at a clamped/wrong instant). Default all()-span, so the span
  // gate passes and the cull is the time_map-evaluation error path.
  const TimeMap overflowing{Time{0}, Rational(std::numeric_limits<std::int64_t>::max(), 1),
                            Time{0}};
  const arbc::ObjectId content_id = add_placed_layer(model, TimeRange::all(), overflowing);
  const arbc::DocStatePtr state = model.current();
  const auto resolver = [&](arbc::ObjectId id) -> arbc::Content* {
    return id == content_id ? &content : nullptr;
  };
  CompositorCounters c;
  drive_frame(*state, resolver, Time{100}, c);
  CHECK(content.render_count == 0); // culled: never rendered
  CHECK(c.requests_issued() == 0);
  CHECK(c.composites() == 0);
}
