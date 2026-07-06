#include <arbc/testing/contract_tests.hpp>

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <vector>

namespace {

using namespace arbc;

// --- Suite-owned render target ---------------------------------------------
//
// A CPU-backed rgba32f surface (the doc-07 working format every contract test
// uses). The suite renders into it and compares the raw float buffer for
// byte-exact equality; a freshly constructed target is all-zero, i.e. fully
// transparent premultiplied output.
class MemSurface final : public Surface {
public:
  MemSurface(int w, int h)
      : d_w(w), d_h(h),
        d_pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4U, 0.0F) {}

  int width() const override { return d_w; }
  int height() const override { return d_h; }
  SurfaceFormat format() const override { return k_working_rgba32f; }

  std::span<std::byte> cpu_bytes() override {
    return {reinterpret_cast<std::byte*>(d_pixels.data()), d_pixels.size() * sizeof(float)};
  }
  std::span<const std::byte> cpu_bytes() const override {
    return {reinterpret_cast<const std::byte*>(d_pixels.data()), d_pixels.size() * sizeof(float)};
  }

  const std::vector<float>& pixels() const { return d_pixels; }

private:
  int d_w;
  int d_h;
  std::vector<float> d_pixels;
};

bool is_transparent(const std::vector<float>& px) {
  return std::all_of(px.begin(), px.end(), [](float v) { return v == 0.0F; });
}

// --- Seeded, suite-owned PRNG (doc 16:39-40, Decision 5) --------------------
//
// splitmix64: deterministic, no ambient randomness, no wall-clock. A fixed
// seed reproduces an exact case set, which is what makes the suite CI-stable
// and lets the quality stress harness perturb the seed deterministically.
class Prng {
public:
  explicit Prng(std::uint64_t seed) : d_state(seed) {}

  std::uint64_t next() {
    d_state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = d_state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  int in_range(int lo, int hi) { // inclusive
    return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1));
  }

  double unit() { // [0, 1)
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }

private:
  std::uint64_t d_state;
};

// --- Generators -------------------------------------------------------------

Rect gen_region(Prng& rng, int w, int h) {
  const double x0 = rng.in_range(0, w - 1);
  const double y0 = rng.in_range(0, h - 1);
  const double x1 = x0 + rng.in_range(1, w);
  const double y1 = y0 + rng.in_range(1, h);
  return {x0, y0, x1, y1};
}

double gen_scale(Prng& rng) { return 0.25 + rng.unit() * 3.75; } // (0.25, 4.0]

Time gen_time(Prng& rng) {
  // Never zero: the operator fixtures reserve Time::zero() as the identity
  // instant, so a generic time draw must not collide with it.
  return Time{static_cast<std::int64_t>(1 + rng.next() % 2'000'000'000ULL)};
}

StateHandle handle(std::uint32_t slot) {
  StateHandle h{};
  h.slot = static_cast<decltype(h.slot)>(slot);
  return h;
}

// --- Render driver ----------------------------------------------------------
//
// Drive one render to its single settlement, whichever path the content takes
// (doc 03:117-121): an inline `RenderResult`, or `nullopt` with a
// `RenderCompletion` the content settled. REQUIREs a successful settlement.
RenderResult drive(Content& content, const RenderRequest& request) {
  auto done = std::make_shared<RenderCompletion>();
  const std::optional<RenderResult> inline_result = content.render(request, done);
  if (inline_result.has_value()) {
    return *inline_result;
  }
  std::optional<expected<RenderResult, RenderError>> settled = done->take();
  REQUIRE(settled.has_value());
  REQUIRE(settled->has_value());
  return **settled;
}

bool contains_center(const Rect& r, int x, int y) {
  const double cx = static_cast<double>(x) + 0.5;
  const double cy = static_cast<double>(y) + 0.5;
  return cx >= r.x0 && cx < r.x1 && cy >= r.y0 && cy < r.y1;
}

} // namespace

namespace arbc::testing {

void check_render_purity(const ContentFactory& factory, const Options& options) {
  Prng rng(options.seed ^ 0x01U);
  for (int c = 0; c < options.cases; ++c) {
    auto content = factory();
    if (content->stability() == Stability::Live) {
      continue; // Live is non-deterministic; it opts out of render purity.
    }
    const Rect region = gen_region(rng, options.width, options.height);
    const double scale = gen_scale(rng);
    const Time time = gen_time(rng);
    const StateHandle snap = handle(static_cast<std::uint32_t>(rng.in_range(1, 64)));

    MemSurface a(options.width, options.height);
    RenderRequest ra{region, scale, time, snap, a, Exactness::Exact, Deadline::none()};
    drive(*content, ra);

    MemSurface b(options.width, options.height);
    RenderRequest rb{region, scale, time, snap, b, Exactness::Exact, Deadline::none()};
    drive(*content, rb);

    // Two calls with an identical RenderRequest yield byte-identical pixels.
    // (claim 03-layer-plugin-interface#render-pure-over-pinned-state)
    REQUIRE(a.pixels() == b.pixels());

    if (options.snapshot_sensitive) {
      const StateHandle other = handle(snap.slot + 1U);
      MemSurface d(options.width, options.height);
      RenderRequest rd{region, scale, time, other, d, Exactness::Exact, Deadline::none()};
      drive(*content, rd);
      // The snapshot handle is a genuine input: differing only in snapshot
      // yields different pixels.
      REQUIRE(a.pixels() != d.pixels());
    }
  }
}

void check_scale_honesty(const ContentFactory& factory, const Options& options) {
  Prng rng(options.seed ^ 0x02U);
  for (int c = 0; c < options.cases; ++c) {
    auto content = factory();
    const Rect region = gen_region(rng, options.width, options.height);
    const double scale = gen_scale(rng);
    const bool exact_req = (rng.next() & 1U) != 0U;
    MemSurface s(options.width, options.height);
    RenderRequest r{region,
                    scale,
                    gen_time(rng),
                    StateHandle{},
                    s,
                    exact_req ? Exactness::Exact : Exactness::BestEffort,
                    exact_req ? Deadline::none() : Deadline{}};
    const RenderResult result = drive(*content, r);

    // achieved_scale never exceeds the requested scale, and a degraded render
    // is never reported `exact`. (claim #render-scale-honest)
    REQUIRE(result.achieved_scale > 0.0);
    REQUIRE(result.achieved_scale <= scale);
    if (result.achieved_scale < scale) {
      REQUIRE_FALSE(result.exact);
    }
    if (exact_req) {
      // An Exact request must be rendered faithfully (content.hpp:205-207).
      REQUIRE(result.exact);
      REQUIRE(result.achieved_scale == scale);
    }
  }
}

void check_time_honesty(const ContentFactory& factory, const Options& options) {
  Prng rng(options.seed ^ 0x03U);
  const Stability stab = factory()->stability();
  if (stab == Stability::Live) {
    return; // Live content makes no time-honesty promise.
  }

  for (int c = 0; c < options.cases; ++c) {
    auto content = factory();
    const Rect region = gen_region(rng, options.width, options.height);
    const double scale = gen_scale(rng);
    const Time t1 = gen_time(rng);

    MemSurface a(options.width, options.height);
    RenderRequest ra{region, scale, t1, StateHandle{}, a, Exactness::Exact, Deadline::none()};
    const RenderResult r1 = drive(*content, ra);

    if (stab == Stability::Static) {
      // Static ignores time: a different time paints identically and reports
      // nullopt achieved_time / time_extent. (claim #static-time-invariant)
      const Time t2 = gen_time(rng);
      MemSurface b(options.width, options.height);
      RenderRequest rb{region, scale, t2, StateHandle{}, b, Exactness::Exact, Deadline::none()};
      const RenderResult r2 = drive(*content, rb);
      REQUIRE(content->time_extent() == std::nullopt);
      REQUIRE(r1.achieved_time == std::nullopt);
      REQUIRE(r2.achieved_time == std::nullopt);
      REQUIRE(a.pixels() == b.pixels());
    } else { // Timed
      // Timed is a deterministic function of time and reports the achieved
      // time it rendered. (claim #render-time-honest)
      REQUIRE(r1.achieved_time.has_value());
      MemSurface b(options.width, options.height);
      RenderRequest rb{region, scale, t1, StateHandle{}, b, Exactness::Exact, Deadline::none()};
      const RenderResult r2 = drive(*content, rb);
      REQUIRE(r2.achieved_time == r1.achieved_time);
      REQUIRE(a.pixels() == b.pixels());
    }
  }
}

void check_bounds_honesty(const ContentFactory& factory, const Options& options) {
  Prng rng(options.seed ^ 0x04U);
  for (int c = 0; c < options.cases; ++c) {
    auto content = factory();
    const std::optional<Rect> b = content->bounds();
    const double scale = gen_scale(rng);

    if (b.has_value() && !b->empty()) {
      // A region placed entirely to the right of the declared bounds must
      // render nothing. (claim #render-within-declared-bounds)
      const double w = std::max(1.0, b->width());
      const Rect outside{b->x1 + 1.0, b->y0, b->x1 + 1.0 + w, b->y1};
      MemSurface s(options.width, options.height);
      RenderRequest r{outside, scale, gen_time(rng), StateHandle{},
                      s,       Exactness::Exact, Deadline::none()};
      drive(*content, r);
      REQUIRE(is_transparent(s.pixels()));
    }

    if (content->stability() == Stability::Timed) {
      const std::optional<TimeRange> te = content->time_extent();
      if (te.has_value() && !te->empty()) {
        // A time past the declared extent likewise renders nothing.
        const Time outside_t{te->end.flicks + 1};
        const Rect region = b.value_or(Rect::from_size(options.width, options.height));
        MemSurface s(options.width, options.height);
        RenderRequest r{region, scale, outside_t, StateHandle{},
                        s,      Exactness::Exact, Deadline::none()};
        drive(*content, r);
        REQUIRE(is_transparent(s.pixels()));
      }
    }
  }
}

void check_capture_restore_roundtrip(const ContentFactory& factory, const Options& options) {
  if (factory()->stability() == Stability::Live) {
    return; // Live content opts out of the round-trip (doc 14:173-174).
  }
  Prng rng(options.seed ^ 0x05U);
  const Rect region = gen_region(rng, options.width, options.height);
  const double scale = gen_scale(rng);
  const Time time = gen_time(rng);

  const int k = std::max(2, options.cases / 2);
  std::vector<StateHandle> handles;
  std::vector<std::vector<float>> captured;
  auto content = factory();
  for (int i = 0; i < k; ++i) {
    const StateHandle h = handle(static_cast<std::uint32_t>(1 + i));
    handles.push_back(h);
    MemSurface s(options.width, options.height);
    RenderRequest r{region, scale, time, h, s, Exactness::Exact, Deadline::none()};
    drive(*content, r);
    captured.push_back(s.pixels());
  }
  // Re-render each handle in a permuted order: restoring a previously captured
  // snapshot reproduces its render byte-for-byte, even after intervening
  // renders under other handles. (claim #capture-restore-roundtrip)
  for (int i = 0; i < k; ++i) {
    const int j = (i * 7 + 3) % k;
    MemSurface s(options.width, options.height);
    RenderRequest r{region, scale, time, handles[static_cast<std::size_t>(j)], s, Exactness::Exact,
                    Deadline::none()};
    drive(*content, r);
    REQUIRE(s.pixels() == captured[static_cast<std::size_t>(j)]);
  }
}

void check_async_cancellation(const ContentFactory& factory, const Options& options) {
  Prng rng(options.seed ^ 0x06U);

  // (1) Render answers along one settle path, and both paths yield a single
  // settlement drained by take(). (claim #render-inline-or-async)
  for (int c = 0; c < options.cases; ++c) {
    auto content = factory();
    MemSurface s(options.width, options.height);
    RenderRequest r{gen_region(rng, options.width, options.height),
                    gen_scale(rng),
                    gen_time(rng),
                    StateHandle{},
                    s,
                    Exactness::BestEffort,
                    Deadline{}};
    auto done = std::make_shared<RenderCompletion>();
    const std::optional<RenderResult> inline_result = content->render(r, done);
    if (inline_result.has_value()) {
      done->complete(*inline_result); // fold the inline value through complete()
    }
    std::optional<expected<RenderResult, RenderError>> settled = done->take();
    REQUIRE(settled.has_value());
    REQUIRE(settled->has_value());
    REQUIRE(done->take() == std::nullopt); // yields at most once
  }

  // (2) A RenderCompletion settles exactly once under concurrent
  // complete / cancel / take from different threads, and cancel is advisory.
  // This is the TSan-covered family (Constraint 6). (claim
  // #render-completion-settles-once)
  for (int c = 0; c < options.cases; ++c) {
    auto done = std::make_shared<RenderCompletion>();
    std::atomic<bool> go{false};
    std::optional<expected<RenderResult, RenderError>> taken;

    std::thread completer([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      done->complete(RenderResult{2.0, true});
    });
    std::thread canceller([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      done->cancel();
    });
    std::thread taker([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < 64; ++i) {
        auto t = done->take();
        if (t.has_value()) {
          taken = t;
          break;
        }
      }
    });

    go.store(true, std::memory_order_release);
    completer.join();
    canceller.join();
    taker.join();

    // cancel() is advisory: it does not prevent the settlement.
    REQUIRE(done->settled());
    // Exactly one settlement is observable across the racing taker and the
    // final drain.
    const auto remainder = done->take();
    REQUIRE(taken.has_value() != remainder.has_value());
    // A second settle attempt after settlement is ignored.
    done->fail(RenderError::ContentFailed);
    REQUIRE(done->take() == std::nullopt);

    const expected<RenderResult, RenderError> settlement =
        taken.has_value() ? *taken : *remainder;
    REQUIRE(settlement.has_value());
    REQUIRE(settlement->achieved_scale == 2.0);
  }
}

void check_facet_consistency(const ContentFactory& factory, const Options& options) {
  for (int c = 0; c < options.cases; ++c) {
    auto content = factory();
    // The description methods are idempotent -- repeated queries agree.
    REQUIRE(content->bounds() == content->bounds());
    REQUIRE(content->stability() == content->stability());
    REQUIRE(content->time_extent() == content->time_extent());
    // ... and mutually coherent: Static content declares no temporal extent
    // (content.hpp:174-183). (claim #facet-consistency)
    if (content->stability() == Stability::Static) {
      REQUIRE(content->time_extent() == std::nullopt);
    }
  }
}

void check_leaf_no_operator_graph(const ContentFactory& factory, const Options& options) {
  Prng rng(options.seed ^ 0x07U);
  auto content = factory();
  // A leaf overrides no operator-graph member: empty inputs, nullopt identity
  // for every request, and the identity damage map. (claim
  // #leaf-content-has-no-operator-graph)
  REQUIRE(content->inputs().empty());
  for (int c = 0; c < options.cases; ++c) {
    MemSurface s(options.width, options.height);
    RenderRequest r{gen_region(rng, options.width, options.height),
                    gen_scale(rng),
                    gen_time(rng),
                    StateHandle{},
                    s,
                    Exactness::BestEffort,
                    Deadline{}};
    REQUIRE(content->identity(r) == std::nullopt);
    const Rect dmg = gen_region(rng, options.width, options.height);
    REQUIRE(content->map_input_damage(0, dmg) == dmg);
  }
}

void check_operator_damage_covers(const ContentFactory& factory, const OperatorDamageCase& edit,
                                  const Options& options) {
  Prng rng(options.seed ^ 0x08U);
  auto content = factory();
  REQUIRE(!content->inputs().empty());
  const Rect region = Rect::from_size(options.width, options.height);
  const Time time = gen_time(rng); // never Time::zero(), so never the identity instant

  MemSurface before(options.width, options.height);
  RenderRequest rb{region, 1.0, time, edit.before, before, Exactness::Exact, Deadline::none()};
  drive(*content, rb);
  MemSurface after(options.width, options.height);
  RenderRequest ra{region, 1.0, time, edit.after, after, Exactness::Exact, Deadline::none()};
  drive(*content, ra);

  const Rect mapped = content->map_input_damage(edit.input, edit.input_damage);
  const std::vector<float>& pb = before.pixels();
  const std::vector<float>& pa = after.pixels();
  REQUIRE(pb.size() == pa.size());

  for (int y = 0; y < options.height; ++y) {
    for (int x = 0; x < options.width; ++x) {
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(options.width) +
           static_cast<std::size_t>(x)) *
          4U;
      bool changed = false;
      for (int ch = 0; ch < 4; ++ch) {
        if (pb[base + static_cast<std::size_t>(ch)] != pa[base + static_cast<std::size_t>(ch)]) {
          changed = true;
          break;
        }
      }
      if (changed) {
        // Every output pixel that actually changed must lie within the
        // reported mapped rect: map_input_damage over-approximates and never
        // under-reports. (claim #operator-damage-covers)
        REQUIRE(contains_center(mapped, x, y));
      }
    }
  }
}

void check_operator_identity_faithful(const ContentFactory& factory, Time identity_time,
                                      const Options& options) {
  auto content = factory();
  const std::span<const ContentRef> ins = content->inputs();
  REQUIRE(!ins.empty());
  const Rect region = Rect::from_size(options.width, options.height);

  MemSurface op_target(options.width, options.height);
  RenderRequest r{region,     1.0, identity_time, StateHandle{}, op_target, Exactness::Exact,
                  Deadline::none()};
  const std::optional<std::size_t> id = content->identity(r);
  REQUIRE(id.has_value());
  const std::size_t n = *id;
  REQUIRE(n < ins.size());

  drive(*content, r);

  // Render input N for the same request as ground truth.
  MemSurface in_target(options.width, options.height);
  RenderRequest ir{region,     1.0, identity_time, StateHandle{}, in_target, Exactness::Exact,
                   Deadline::none()};
  drive(*ins[n], ir);

  // At an identity request the operator's output is byte-identical to input
  // N's output. (claim #operator-identity-faithful)
  REQUIRE(op_target.pixels() == in_target.pixels());
}

void check_damage_soundness(const ContentFactory& factory, const OperatorDamageCase& edit,
                            const Options& options) {
  Prng rng(options.seed ^ 0x09U);
  auto content = factory();
  const Rect region = Rect::from_size(options.width, options.height);
  const Time time = gen_time(rng);

  MemSurface before(options.width, options.height);
  RenderRequest rb{region, 1.0, time, edit.before, before, Exactness::Exact, Deadline::none()};
  drive(*content, rb);
  MemSurface after(options.width, options.height);
  RenderRequest ra{region, 1.0, time, edit.after, after, Exactness::Exact, Deadline::none()};
  drive(*content, ra);

  const std::vector<float>& pb = before.pixels();
  const std::vector<float>& pa = after.pixels();
  REQUIRE(pb.size() == pa.size());

  for (int y = 0; y < options.height; ++y) {
    for (int x = 0; x < options.width; ++x) {
      if (contains_center(edit.input_damage, x, y)) {
        continue; // inside the reported damage -- may change
      }
      const std::size_t base =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(options.width) +
           static_cast<std::size_t>(x)) *
          4U;
      for (int ch = 0; ch < 4; ++ch) {
        // A region disjoint from the edit's damage is bit-identical across the
        // edit. (claim #undamaged-regions-stable)
        REQUIRE(pb[base + static_cast<std::size_t>(ch)] == pa[base + static_cast<std::size_t>(ch)]);
      }
    }
  }
}

} // namespace arbc::testing

namespace arbc {

void contract_tests(const testing::ContentFactory& factory, const testing::Options& options) {
  using namespace testing;
  if (options.render_purity) {
    check_render_purity(factory, options);
  }
  if (options.scale_honesty) {
    check_scale_honesty(factory, options);
  }
  if (options.time_honesty) {
    check_time_honesty(factory, options);
  }
  if (options.bounds_honesty) {
    check_bounds_honesty(factory, options);
  }
  if (options.capture_restore) {
    check_capture_restore_roundtrip(factory, options);
  }
  if (options.async_cancellation) {
    check_async_cancellation(factory, options);
  }
  if (options.facet_consistency) {
    check_facet_consistency(factory, options);
  }
  if (options.operator_graph && factory()->inputs().empty()) {
    check_leaf_no_operator_graph(factory, options);
  }
}

} // namespace arbc
