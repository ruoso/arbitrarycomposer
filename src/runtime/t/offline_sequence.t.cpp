#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/backend.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

// Offline sequence renderer (`runtime.offline_sequences`, doc 02:73-85,
// 11:209-213). The scene stubs mirror the runtime interactive-loop test's
// `BufferSurface`/`MarkBackend` (a CPU surface storing raw bytes so a golden reads
// them back byte-for-byte, and a backend whose composite folds a deterministic
// function of opacity + the source's first byte into every destination byte, so an
// identical composite sequence reproduces identical bytes). Goldens are computed
// against an independent in-process render, per the repo's no-frozen-table policy.

namespace {

using arbc::Content;
using arbc::Exactness;
using arbc::RenderCompletion;
using arbc::RenderRequest;
using arbc::RenderResult;
using arbc::Stability;
using arbc::Time;

void fill_solid(arbc::Surface& target) {
  const std::span<std::byte> bytes = target.cpu_bytes();
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::byte>((i * 31u + 7u) & 0xFFu);
  }
}

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
  void clear(arbc::Surface& surface, float, float, float, float) override {
    const std::span<std::byte> bytes = surface.cpu_bytes();
    std::memset(bytes.data(), 0, bytes.size_bytes());
  }
  void composite(arbc::Surface& dst, const arbc::Surface& src, const arbc::Affine&,
                 double opacity) override {
    const std::span<const std::byte> s = src.cpu_bytes();
    const unsigned seed = s.empty() ? 0u : std::to_integer<unsigned>(s[0]);
    const auto mark = (static_cast<unsigned>(opacity * 251.0) + 1u + seed) & 0xFFu;
    for (std::byte& b : dst.cpu_bytes()) {
      b = static_cast<std::byte>((std::to_integer<unsigned>(b) + mark) & 0xFFu);
    }
  }
  void downsample(arbc::Surface&, const arbc::Surface&) override {}
};

// A backend that cannot store any working space: `make_surface` yields the
// capability-honest `SurfaceError` value (doc 10), never an abort.
class RejectingBackend : public arbc::Backend {
public:
  arbc::BackendCaps capabilities() const override { return {}; }
  arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError>
  make_surface(int, int, arbc::SurfaceFormat) override {
    return arbc::unexpected(arbc::SurfaceError::UnsupportedFormat);
  }
  void clear(arbc::Surface&, float, float, float, float) override {}
  void composite(arbc::Surface&, const arbc::Surface&, const arbc::Affine&, double) override {}
  void downsample(arbc::Surface&, const arbc::Surface&) override {}
};

// A synchronous solid fill. Configurable stability; records the request discipline
// (`exactness`/`deadline`) it was driven with, so a test can witness that the
// offline driver stamps `Exact` + `Deadline::none()` (doc 02:73-85, 03:124-127).
class SyncSolid : public Content {
public:
  explicit SyncSolid(Stability stability = Stability::Static) : d_stability(stability) {}
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return d_stability; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return d_stability == Stability::Static ? std::optional<arbc::TimeRange>(std::nullopt)
                                            : std::optional<arbc::TimeRange>(arbc::TimeRange::all());
  }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    d_saw_exact.store(request.exactness == Exactness::Exact, std::memory_order_release);
    d_saw_no_deadline.store(request.deadline.is_none(), std::memory_order_release);
    fill_solid(request.target);
    return RenderResult{request.scale, /*exact=*/true};
  }
  bool saw_exact() const { return d_saw_exact.load(std::memory_order_acquire); }
  bool saw_no_deadline() const { return d_saw_no_deadline.load(std::memory_order_acquire); }

private:
  Stability d_stability;
  std::atomic<bool> d_saw_exact{false};
  std::atomic<bool> d_saw_no_deadline{false};
};

// A 24 fps Timed content that quantizes to native frames, records how many renders
// it was actually driven, and reports the floored `achieved_time` (doc 11:115-129).
class TimedFill : public Content {
public:
  static constexpr std::int64_t k_frame = Time::flicks_per_second / 24;
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override {
    return arbc::TimeRange{Time::zero(), Time{k_frame * 48}};
  }
  std::optional<Time> quantize_time(Time t) const override {
    return Time{(t.flicks / k_frame) * k_frame};
  }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    ++d_renders;
    const std::int64_t frame = request.time.flicks / k_frame;
    fill_solid(request.target);
    RenderResult result{request.scale, /*exact=*/true};
    result.achieved_time = Time{frame * k_frame};
    return result;
  }
  int renders() const { return d_renders; }

private:
  int d_renders{0};
};

// A Timed content honoring any time exactly (identity `quantize_time`), recording
// every content-local instant it was requested at -- so a test can witness the
// per-layer `time_map` retiming the compositor applied.
class TimeRecorder : public Content {
public:
  std::optional<arbc::Rect> bounds() const override { return arbc::Rect{0.0, 0.0, 512.0, 512.0}; }
  Stability stability() const override { return Stability::Timed; }
  std::optional<arbc::TimeRange> time_extent() const override { return arbc::TimeRange::all(); }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    d_times.push_back(request.time.flicks);
    fill_solid(request.target);
    return RenderResult{request.scale, /*exact=*/true};
  }
  const std::vector<std::int64_t>& times() const { return d_times; }

private:
  std::vector<std::int64_t> d_times;
};

std::vector<std::byte> snapshot(const arbc::Surface& surface) {
  const std::span<const std::byte> b = surface.cpu_bytes();
  return {b.begin(), b.end()};
}

bool bytes_identical(std::span<const std::byte> a, std::span<const std::byte> b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size_bytes()) == 0;
}

bool all_zero(std::span<const std::byte> a) {
  for (const std::byte b : a) {
    if (std::to_integer<unsigned>(b) != 0u) {
      return false;
    }
  }
  return true;
}

// A 256x256 identity viewport is exactly one rung-0 (scale 1.0) tile: exact scale,
// no remainder resample.
arbc::Viewport one_tile_viewport() { return arbc::Viewport{256, 256, arbc::Affine::identity()}; }

} // namespace

// enforces: 11-time-and-video#offline-sequence-steps-frame-times-exactly
TEST_CASE("frame_times_over steps the half-open range at the output rate exactly") {
  const std::int64_t frame = Time::flicks_per_second / 24; // 24 fps native step

  SECTION("half-open: start included, end excluded") {
    const std::vector<Time> times =
        arbc::frame_times_over(arbc::TimeRange{Time::zero(), Time{frame * 4}}, arbc::Rational(24, 1));
    REQUIRE(times.size() == 4);
    CHECK(times[0].flicks == 0);
    CHECK(times[1].flicks == frame);
    CHECK(times[2].flicks == frame * 2);
    CHECK(times[3].flicks == frame * 3); // frame*4 == end is excluded
  }

  SECTION("an offset (non-zero) start anchors frame 0") {
    const std::vector<Time> times = arbc::frame_times_over(
        arbc::TimeRange{Time{frame}, Time{frame * 3}}, arbc::Rational(24, 1));
    REQUIRE(times.size() == 2);
    CHECK(times[0].flicks == frame);
    CHECK(times[1].flicks == frame * 2);
  }

  SECTION("empty range and non-positive rate yield an empty series") {
    CHECK(arbc::frame_times_over(arbc::TimeRange{Time{frame}, Time{frame}}, arbc::Rational(24, 1))
              .empty());
    CHECK(arbc::frame_times_over(arbc::TimeRange{Time::zero(), Time{frame}}, arbc::Rational(0, 1))
              .empty());
  }
}

// enforces: 11-time-and-video#offline-sequence-steps-frame-times-exactly
TEST_CASE("an offline sequence export is byte-exact and deterministic") {
  auto content = std::make_shared<SyncSolid>();
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  document.add_layer(cid, arbc::Affine::identity());

  const arbc::TimeRange range{Time::zero(), Time{Time::flicks_per_second / 8}};
  const std::vector<Time> times = arbc::frame_times_over(range, arbc::Rational(24, 1));
  REQUIRE(times.size() == 3);

  auto run_export = [&]() {
    MarkBackend backend;
    arbc::SequenceRenderer renderer(document, one_tile_viewport(), backend);
    std::vector<std::vector<std::byte>> frames;
    renderer.render_sequence(
        times, [&](Time, arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> frame) {
          REQUIRE(frame.has_value());
          frames.push_back(snapshot(**frame));
        });
    return frames;
  };

  const std::vector<std::vector<std::byte>> first = run_export();
  REQUIRE(first.size() == 3);

  // An independent single-frame reference render at the same instant.
  MarkBackend ref_backend;
  arbc::SequenceRenderer reference(document, one_tile_viewport(), ref_backend);
  const auto ref_frame = reference.render_frame_at(times[0]);
  REQUIRE(ref_frame.has_value());
  const std::vector<std::byte> ref = snapshot(**ref_frame);

  for (const std::vector<std::byte>& f : first) {
    CHECK(bytes_identical(f, ref)); // every frame is the byte-exact fresh result
  }

  // Re-running the export produces byte-identical bytes (determinism).
  const std::vector<std::vector<std::byte>> second = run_export();
  REQUIRE(second.size() == first.size());
  for (std::size_t i = 0; i < first.size(); ++i) {
    CHECK(bytes_identical(first[i], second[i]));
  }
}

// enforces: 02-architecture#offline-frame-renders-exactly-no-degrade
TEST_CASE("an offline frame renders exactly with no degradation") {
  auto content = std::make_shared<SyncSolid>();
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  document.add_layer(cid, arbc::Affine::identity());

  MarkBackend backend;
  arbc::SequenceRenderer renderer(document, one_tile_viewport(), backend);

  const auto frame = renderer.render_frame_at(Time::zero());
  REQUIRE(frame.has_value());

  // The exact / no-deadline discipline reached the content: an Exact request
  // carrying Deadline::none() (doc 02:73-85, 03:124-127).
  CHECK(content->saw_exact());
  CHECK(content->saw_no_deadline());

  // Only fresh, exact-scale tiles were composited -- no stale/coarser/placeholder
  // ever appears in an exported frame, no matter how long a render takes.
  CHECK(renderer.counters().degraded_composites() == 0);
  CHECK(renderer.counters().composites() == 1); // the single visible tile, fresh
  CHECK(renderer.counters().requests_issued() == 1);

  // Byte-exact to an independent fresh reference render.
  MarkBackend ref_backend;
  arbc::SequenceRenderer reference(document, one_tile_viewport(), ref_backend);
  const auto ref = reference.render_frame_at(Time::zero());
  REQUIRE(ref.has_value());
  CHECK(bytes_identical(snapshot(**frame), snapshot(**ref)));
}

// enforces: 11-time-and-video#achieved-time-coalescing-issues-zero-renders
TEST_CASE("an offline export coalesces sub-native-grid instants onto one cached tile") {
  auto timed = std::make_shared<TimedFill>();
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(timed);
  document.add_layer(cid, arbc::Affine::identity());

  MarkBackend backend;
  arbc::SequenceRenderer renderer(document, one_tile_viewport(), backend);

  // Output rate 72 fps is 3x the content's 24 fps native grid, so each native frame
  // period holds three output instants: the first renders, the next two coalesce
  // onto the same cached temporal-key tile.
  const arbc::TimeRange range{Time::zero(), Time{TimedFill::k_frame * 2}};
  const std::vector<Time> times = arbc::frame_times_over(range, arbc::Rational(72, 1));
  REQUIRE(times.size() == 6); // 2 native frames x 3 output instants

  int frames = 0;
  renderer.render_sequence(
      times, [&](Time, arbc::expected<std::unique_ptr<arbc::Surface>, arbc::SurfaceError> f) {
        REQUIRE(f.has_value());
        ++frames;
      });
  CHECK(frames == 6);

  // Exactly one render per native frame; the four sub-native-grid instants are
  // fresh cache hits over the warm shared TileCache (misses() is not asserted: a
  // cold tile also issues coarser-fallback probe lookups, whose count is coupled to
  // k_max_fallback_octaves -- the robust witnesses are the driven render count and
  // the fresh-hit count).
  CHECK(timed->renders() == 2);
  CHECK(renderer.cache().hits() == 4);
  CHECK(renderer.counters().requests_issued() == 2);
  CHECK(renderer.counters().degraded_composites() == 0);
}

// enforces: 11-time-and-video#static-tiles-survive-clock
TEST_CASE("an all-Static offline export reuses clock-invariant tiles across frames") {
  auto content = std::make_shared<SyncSolid>(Stability::Static);
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  document.add_layer(cid, arbc::Affine::identity());

  MarkBackend backend;
  arbc::SequenceRenderer renderer(document, one_tile_viewport(), backend);

  const std::vector<Time> times = arbc::frame_times_over(
      arbc::TimeRange{Time::zero(), Time{Time::flicks_per_second}}, arbc::Rational(24, 1));
  REQUIRE(times.size() == 24);
  for (const Time t : times) {
    REQUIRE(renderer.render_frame_at(t).has_value());
  }

  // Static keys omit achieved_time, so a clock advance re-plans to fresh hits: the
  // single tile is rendered once and reused (fresh-hit) for every later frame.
  CHECK(renderer.cache().hits() == 23);
  CHECK(renderer.counters().requests_issued() == 1);
  CHECK(renderer.counters().degraded_composites() == 0);
}

// enforces: 11-time-and-video#span-cull-is-half-open
TEST_CASE("an offline export culls a layer outside its half-open span") {
  auto content = std::make_shared<SyncSolid>();
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  const arbc::ObjectId layer = document.add_layer(cid, arbc::Affine::identity());
  const std::int64_t frame = TimedFill::k_frame;
  // Present for parent time in [frame, 3*frame).
  document.set_layer_span(layer, arbc::TimeRange{Time{frame}, Time{frame * 3}});

  MarkBackend backend;
  arbc::SequenceRenderer renderer(document, one_tile_viewport(), backend);

  // Instants frame*0 (before), frame*1 and frame*2 (in span), frame*3 (== out, excluded).
  const std::vector<Time> times{Time{0}, Time{frame}, Time{frame * 2}, Time{frame * 3}};
  std::vector<bool> present;
  for (const Time t : times) {
    const auto f = renderer.render_frame_at(t);
    REQUIRE(f.has_value());
    present.push_back(!all_zero(snapshot(**f)));
  }
  CHECK_FALSE(present[0]); // before in: culled (empty frame)
  CHECK(present[1]);       // in == present
  CHECK(present[2]);       // inside span
  CHECK_FALSE(present[3]); // out is excluded (half-open): culled
}

// enforces: 11-time-and-video#compositor-retimes-request-through-time-map
TEST_CASE("an offline export requests content at its time-mapped instant") {
  auto content = std::make_shared<TimeRecorder>();
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  const arbc::ObjectId layer = document.add_layer(cid, arbc::Affine::identity());
  // local = (composition_time - 0) * 2 + 100.
  document.set_layer_time_map(layer,
                              arbc::TimeMap{Time::zero(), arbc::Rational(2, 1), Time{100}});

  MarkBackend backend;
  arbc::SequenceRenderer renderer(document, one_tile_viewport(), backend);

  const std::vector<std::int64_t> comp{0, 50, 200};
  for (const std::int64_t c : comp) {
    REQUIRE(renderer.render_frame_at(Time{c}).has_value());
  }
  REQUIRE(content->times().size() == 3);
  CHECK(content->times()[0] == 0 * 2 + 100);
  CHECK(content->times()[1] == 50 * 2 + 100);
  CHECK(content->times()[2] == 200 * 2 + 100);
}

TEST_CASE("render_frame_at surfaces an unstorable working space as an error") {
  auto content = std::make_shared<SyncSolid>();
  arbc::Document document;
  const arbc::ObjectId comp = document.add_composition(8.0, 8.0);
  (void)comp;
  const arbc::ObjectId cid = document.add_content(content);
  document.add_layer(cid, arbc::Affine::identity());

  RejectingBackend backend;
  arbc::SequenceRenderer renderer(document, arbc::Viewport{8, 8, arbc::Affine::identity()}, backend);
  const auto frame = renderer.render_frame_at(Time::zero());
  REQUIRE_FALSE(frame.has_value());
  CHECK(frame.error() == arbc::SurfaceError::UnsupportedFormat);
}

TEST_CASE("parallel-exact rendering is byte-identical to the inline path") {
  auto content = std::make_shared<SyncSolid>();
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  document.add_layer(cid, arbc::Affine::identity());

  MarkBackend inline_backend;
  arbc::SequenceRenderer inline_renderer(document, one_tile_viewport(), inline_backend);
  const auto inline_frame = inline_renderer.render_frame_at(Time::zero());
  REQUIRE(inline_frame.has_value());

  arbc::WorkerPoolConfig parallel;
  parallel.worker_count = 4;
  MarkBackend parallel_backend;
  arbc::SequenceRenderer parallel_renderer(document, one_tile_viewport(), parallel_backend,
                                           parallel);
  const auto parallel_frame = parallel_renderer.render_frame_at(Time::zero());
  REQUIRE(parallel_frame.has_value());

  // Exactness is order-independent, so the fan-out yields identical pixels. (The
  // parallel path composites a transient placeholder into an internal pass before
  // re-compositing the fully-warm cache, so its degraded-composites counter is not
  // a clean zero -- the EXPORTED surface, compared here, carries no placeholder.)
  CHECK(bytes_identical(snapshot(**inline_frame), snapshot(**parallel_frame)));
}

// enforces: 02-architecture#offline-sequence-pins-single-revision
TEST_CASE("an offline export pins one revision while a writer keeps editing") {
  auto content = std::make_shared<SyncSolid>(Stability::Static);
  arbc::Document document;
  const arbc::ObjectId cid = document.add_content(content);
  const arbc::ObjectId layer = document.add_layer(cid, arbc::Affine::identity());

  // A 512x512 viewport is a 2x2 rung-0 grid; an identity layer covers all four
  // tiles. A leaked mid-export edit that translated the layer would change the set
  // of covered tiles (and therefore the composited bytes), so byte-equality to the
  // pinned-revision golden is a genuine witness that no edit leaked in.
  const arbc::Viewport viewport{512, 512, arbc::Affine::identity()};
  MarkBackend backend;
  arbc::SequenceRenderer renderer(document, viewport, backend);

  const std::uint64_t pinned = renderer.revision();
  const std::vector<std::byte> golden = snapshot(**renderer.render_frame_at(Time::zero()));

  // The model is writer-thread-confined (`SlotStore` allocate is writer-only), so
  // the COMMITS stay on this (the owning) thread and the read-only EXPORT runs on a
  // second thread -- the "export while editing" split of doc 02:77-80. The exporter
  // records outcomes into atomics (Catch2 macros are not thread-safe) checked after
  // the join. No timing assertion: the exporter loops a fixed frame count while the
  // writer commits, and both are gated on `go`.
  constexpr int k_frames = 64;
  std::atomic<bool> go{false};
  std::atomic<bool> mismatch{false};
  std::atomic<bool> wrong_revision{false};
  std::atomic<bool> render_failed{false};
  std::atomic<int> frames_done{0};
  std::thread exporter([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < k_frames; ++i) {
      auto frame = renderer.render_frame_at(Time{i});
      if (!frame.has_value()) {
        render_failed.store(true, std::memory_order_release);
      } else if (!bytes_identical(snapshot(**frame), golden)) {
        mismatch.store(true, std::memory_order_release); // a mid-export edit leaked in
      }
      if (renderer.revision() != pinned) {
        wrong_revision.store(true, std::memory_order_release);
      }
      frames_done.fetch_add(1, std::memory_order_release);
    }
  });

  go.store(true, std::memory_order_release);
  int commits = 0;
  // Keep committing new revisions (edits that WOULD change the covered-tile set,
  // hence the bytes, if they leaked) until the exporter has drained every frame.
  while (frames_done.load(std::memory_order_acquire) < k_frames) {
    document.set_layer_transform(layer, (commits % 2 == 0) ? arbc::Affine::translation(256.0, 256.0)
                                                           : arbc::Affine::identity());
    if ((++commits % 16) == 0) {
      std::this_thread::yield(); // widen the race window
    }
  }
  exporter.join();

  CHECK_FALSE(render_failed.load(std::memory_order_acquire));
  CHECK_FALSE(mismatch.load(std::memory_order_acquire));  // no exported frame saw an edit
  CHECK_FALSE(wrong_revision.load(std::memory_order_acquire)); // the pin held throughout
  CHECK(renderer.revision() == pinned);
  // The writer really did advance the model concurrently (the export saw none of it).
  CHECK(document.pin()->revision() > pinned);
}
