// org.arbc.image concurrency (doc 16 tier-6, tsan lane). The mirror image of
// `imageseq_concurrency_stress.t.cpp`, and deliberately so: imageseq declares
// `render_thread_safe() == false` because its decoder and LRU are stateful, so the core
// SERIALIZES its renders through the per-content queue. `org.arbc.image` declares TRUE --
// its pyramid is immutable after construction, so a render is a pure read -- and therefore it
// genuinely runs CONCURRENTLY on workers (doc 00:203's leaf-only worker dispatch). That is
// the win Decision 4 is claiming, so it is the thing this test has to hold to.
//
// Five races, all real:
//
//   * Concurrent RENDERS of one content across the worker pool. Every render allocates from
//     (and releases back to) the plugin-owned tile free list, so the free list is genuinely
//     shared mutable state on the hot path even though the pyramid is not.
//   * Concurrent CONSTRUCTION of several contents resolving to ONE URI. The pyramid cache is
//     the state at risk here; in production construction is writer-thread-only, but the counter
//     claim -- one decode per resolved identity -- must hold under a race, not merely under a
//     convention. It survives kinds.image_master_budget UNCHANGED, and that is deliberate: the
//     decode stays under the cache mutex (that task's Decision 9) precisely so this test does
//     not have to move.
//   * A LATE INSTALL racing in-flight worker renders (kinds.image_async_pending).
//   * CONTINUOUS EVICTION racing worker renders (kinds.image_master_budget). The budget turns a
//     construction-time-only cache into one TOUCHED FROM EVERY RENDER THREAD, so a render is no
//     longer a read of a pointer nobody writes -- it takes a pin, may decode, and may drop the
//     last reference to a pyramid on the way out.
//   * An EVICTOR racing a live pin. The precise memory-safety risk the budget introduces:
//     EVICT-WHILE-PINNED MUST NOT FREE. The pin is an owning `shared_ptr`, so an
//     evicted-but-pinned pyramid outlives its cache entry.
//
// The kind keeps `render_thread_safe() == true` across all of it, and the legs it now stands on
// are narrow and named (kinds.image_master_budget Decision 3): a `Pyramid` object is immutable
// for its whole life, a pin OWNS the one a render reads, and the cache is mutex-guarded. What it
// no longer rests on is monotonicity of a content-owned pointer -- eviction retires that, and
// what publishes once instead is the content's EXTENT.
//
// Outcome-only assertions, no wall-clock. Runs green under dev, ASan/UBSan and
// `ctest --preset tsan`.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/serialize/codec.hpp> // CodecTable (names nlohmann::json)
#include <arbc/serialize/load_context.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

// A DEFERRING source whose `on_ready` the test fires FROM WHATEVER THREAD IT LIKES -- which is
// what a real network source does, and the reason `PendingExternalLoads::complete` is the task's
// one cross-thread channel. `request()` may itself be called concurrently in these drivers, so
// the outstanding list is mutex-guarded (a production source owns this problem too).
class ThreadedDeferringSource final : public AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.insert_or_assign(std::move(uri), std::move(bytes));
  }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    const std::lock_guard<std::mutex> lock(d_mutex);
    ++d_requests;
    d_outstanding.push_back(Request{std::string(resolved_uri), std::move(on_ready)});
  }

  std::size_t fire_all() {
    std::vector<Request> firing;
    {
      const std::lock_guard<std::mutex> lock(d_mutex);
      firing.swap(d_outstanding);
    }
    for (const Request& r : firing) {
      const auto it = d_files.find(r.uri);
      r.on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
    }
    return firing.size();
  }

  std::size_t requests() const {
    const std::lock_guard<std::mutex> lock(d_mutex);
    return d_requests;
  }

private:
  struct Request {
    std::string uri;
    std::function<void(std::string_view)> on_ready;
  };

  mutable std::mutex d_mutex;
  std::unordered_map<std::string, std::string> d_files; // written before the threads start
  std::vector<Request> d_outstanding;
  std::size_t d_requests{0};
};

Registry image_registry() {
  Registry registry;
  REQUIRE(registry
              .add(
                  arbc::image::ImageContent::kind_id,
                  [](ContentConfig config) { return arbc::image::make_image_content(config); },
                  KindMetadata{"Image", "1"})
              .has_value());
  return registry;
}

std::string image_doc(const std::string& source) {
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,384,320],"layers":[)"
         R"({"kind":"org.arbc.image","kind_version":"1","params":{"source":")" +
         source + R"("}}]}})";
}

// Render one fixed tile of `content`. Deliberately CATCH2-FREE: the racing driver below calls it
// from worker threads, and Catch2's assertion macros are not thread-safe -- an outcome smuggled
// out through a return value and asserted on the main thread is the only honest way to do this.
//
//   * nullopt      -- the HARNESS failed (a surface allocation), never the content;
//   * empty vector -- the content answered NOTHING: the pending/unavailable state, which the
//                     compositor culls on empty bounds;
//   * pixels       -- the decoded pyramid.
//
// The last two are the only outcomes a racing worker may legally observe. A third would be a torn
// read.
std::optional<std::vector<float>> render_tile(arbc::image::ImageContent& content,
                                              Backend& backend) {
  constexpr int k_edge = 64;
  auto target = backend.make_surface(k_edge, k_edge, k_working_rgba32f);
  if (!target.has_value()) {
    return std::nullopt;
  }
  auto done = std::make_shared<RenderCompletion>();
  const RenderRequest request{Rect{32.0, 32.0, 96.0, 96.0},
                              1.0,
                              Time::zero(),
                              StateHandle{},
                              **target,
                              Exactness::Exact,
                              Deadline::none()};
  const std::optional<RenderResult> r = content.render(request, done);
  if (!r.has_value()) {
    return std::vector<float>{}; // culled: no pixels to answer with
  }
  if (!r->provided.has_value()) {
    return std::nullopt; // this kind ALWAYS provides its own surface (Decision 1)
  }
  const std::span<const float> px = r->provided->surface().span<PixelFormat::Rgba32fLinearPremul>();
  return std::vector<float>(px.begin(), px.end());
}

} // namespace

TEST_CASE("concurrent renders of one org.arbc.image are race-free across the worker pool") {
  CpuBackend backend;
  auto content = fix::make_content();
  REQUIRE(content->render_thread_safe()); // the immutable pyramid opts IN to worker dispatch

  constexpr int k_tasks = 200;
  constexpr int k_edge = 32;

  std::vector<std::unique_ptr<Surface>> targets;
  std::vector<std::shared_ptr<RenderCompletion>> dones;
  targets.reserve(k_tasks);
  dones.reserve(k_tasks);
  for (int i = 0; i < k_tasks; ++i) {
    auto target = backend.make_surface(k_edge, k_edge, k_working_rgba32f);
    REQUIRE(target.has_value());
    targets.push_back(std::move(*target));
    dones.push_back(std::make_shared<RenderCompletion>());
  }

  WorkerPool pool(WorkerPoolConfig{/*worker_count=*/4});
  CompletionCursor cursor;

  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  constexpr int k_producers = 4;
  for (int p = 0; p < k_producers; ++p) {
    producers.emplace_back([&, p] {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = p; i < k_tasks; i += k_producers) {
        // Walk the image in tile-sized strides so different workers hit different regions --
        // and, at 0.5 scale, different pyramid rungs -- all reading the one immutable pyramid.
        const auto idx = static_cast<std::size_t>(i);
        const double x0 = static_cast<double>((i % 8) * 32);
        const double y0 = static_cast<double>((i % 5) * 32);
        const double scale = ((i % 2) == 0) ? 1.0 : 0.5;
        const double extent = static_cast<double>(k_edge) / scale;
        RenderTask task{content.get(),
                        RenderRequest{Rect{x0, y0, x0 + extent, y0 + extent}, scale, Time::zero(),
                                      StateHandle{}, *targets[idx], Exactness::Exact,
                                      Deadline::none()},
                        dones[idx]};
        pool.submit(std::move(task));
      }
    });
  }

  go.store(true, std::memory_order_release);
  for (std::thread& t : producers) {
    t.join();
  }

  int settled = 0;
  while (settled < k_tasks) {
    pool.wait_completions(cursor, std::nullopt);
    settled = 0;
    for (int i = 0; i < k_tasks; ++i) {
      settled += dones[static_cast<std::size_t>(i)]->settled() ? 1 : 0;
    }
  }

  // Every submission settled exactly once, with pixels -- and, because the content is
  // thread-safe, several were genuinely in flight at once (the pool does NOT throttle it to
  // one, which is exactly what distinguishes this kind from imageseq).
  for (int i = 0; i < k_tasks; ++i) {
    const auto idx = static_cast<std::size_t>(i);
    const std::optional<expected<RenderResult, RenderError>> taken = dones[idx]->take();
    REQUIRE(taken.has_value());
    REQUIRE(taken->has_value());
    REQUIRE((*taken)->provided.has_value());
    // Static content: no achieved_time, so it adds no time dimension to the cache key.
    CHECK((*taken)->achieved_time == std::nullopt);
    CHECK(dones[idx]->take() == std::nullopt); // settles at most once
  }
  CHECK(pool.tasks_completed() == static_cast<std::uint64_t>(k_tasks));
}

TEST_CASE("concurrent construction against one resolved URI issues exactly one decode") {
  const std::string bytes = fix::fixture_bytes();
  REQUIRE_FALSE(bytes.empty());

  // A cache LOCAL to this test, so the counter is isolated from every other decode in the
  // binary and the assertion is an exact equality rather than a delta.
  arbc::image::PyramidCache cache;

  constexpr int k_threads = 8;
  std::atomic<bool> go{false};
  std::vector<std::thread> racers;
  std::vector<arbc::image::PyramidPtr> got(k_threads);
  racers.reserve(k_threads);
  for (int i = 0; i < k_threads; ++i) {
    racers.emplace_back([&, i] {
      while (!go.load(std::memory_order_acquire)) {
      }
      // Every thread resolves the SAME URI. Whichever wins the race decodes; the rest must
      // observe its pyramid, not decode a second one.
      got[static_cast<std::size_t>(i)] =
          cache.resolve("project/assets/bg.ppm",
                        std::span<const unsigned char>(
                            reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& t : racers) {
    t.join();
  }

  // Exactly ONE decode, and every racer holds the very same pyramid -- no data race on the
  // cache, and dedup by resolved identity (doc 08:116-122) survives concurrency.
  CHECK(cache.decodes_issued() == 1);
  for (const arbc::image::PyramidPtr& p : got) {
    REQUIRE(p != nullptr);
    CHECK(p.get() == got[0].get());
  }
}

// enforces: 03-layer-plugin-interface#image-pyramid-publishes-once
TEST_CASE("a late install racing in-flight worker renders is atomic, monotonic, and once") {
  // THE one cross-thread edge kinds.image_async_pending introduces: a worker rendering a pinned
  // EARLIER revision reads the content's pixels while the writer thread publishes the arrival.
  //
  // What makes that race benign without a lock is that a reader can observe exactly two values
  // and both are self-consistent -- the empty state (which the compositor culls, because bounds
  // are empty) and the FINISHED pyramid. There is no intermediate. So the assertion is not merely
  // "TSan is quiet": every non-empty render is compared BYTE-FOR-BYTE against the fully-decoded
  // reference, so a torn pointer or a half-built pyramid would surface as a pixel mismatch rather
  // than only as a race report.
  //
  // What publishes ONCE and MONOTONICALLY is the EXTENT (kinds.image_master_budget Decision 3).
  // The pixels are now budgeted derived data, so the property "the pyramid pointer never changes
  // once set" is GONE -- deliberately, and the two cases below are where its replacement is held
  // to. The property that had to survive is the one doc 03's obligation actually cares about: a
  // worker never observes a partial state, and the state the compositor CULLS on never reverts.
  CpuBackend backend;
  const std::string bytes = fix::fixture_bytes();
  REQUIRE_FALSE(bytes.empty());

  // The reference frame, from a content that was NEVER pending. `fix::make_content` decodes
  // through `Pyramid::decode` directly, so it does not touch the process-wide cache and the
  // decode counter below stays an exact measure of what the INSTALL cost.
  auto reference = fix::make_content();
  const std::optional<std::vector<float>> reference_tile = render_tile(*reference, backend);
  REQUIRE(reference_tile.has_value());
  REQUIRE_FALSE(reference_tile->empty());
  const std::vector<float>& expected = *reference_tile;

  // A URI no other case in this binary names, so the cache entry is genuinely fresh and the
  // delta is exactly the install's own decode.
  const auto pending = std::make_unique<arbc::image::ImageContent>(
      std::string("assets/photo.ppm"), std::string("stress/publish_once/photo.ppm"),
      arbc::image::PyramidPtr{});
  REQUIRE(pending->render_thread_safe()); // the flag survives the late install -- that IS the claim
  CHECK_FALSE(pending->available());
  CHECK(pending->bounds()->empty());

  constexpr int k_readers = 6;
  constexpr int k_rounds = 400;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> empty_reads{0};
  std::atomic<std::uint64_t> full_reads{0};
  std::atomic<std::uint64_t> torn_reads{0};
  std::atomic<std::uint64_t> harness_failures{0};

  std::vector<std::thread> readers;
  readers.reserve(k_readers);
  for (int r = 0; r < k_readers; ++r) {
    readers.emplace_back([&] {
      CpuBackend local; // each reader owns its own backend: the race under test is the pyramid's
      while (!go.load(std::memory_order_acquire)) {
      }
      while (!stop.load(std::memory_order_acquire)) {
        const std::optional<std::vector<float>> got = render_tile(*pending, local);
        if (!got.has_value()) {
          harness_failures.fetch_add(1, std::memory_order_relaxed);
        } else if (got->empty()) {
          empty_reads.fetch_add(1, std::memory_order_relaxed); // culled: the pre-arrival state
        } else if (*got == expected) {
          full_reads.fetch_add(1, std::memory_order_relaxed); // the complete decoded pyramid
        } else {
          torn_reads.fetch_add(1, std::memory_order_relaxed); // a THIRD value: the bug
        }
      }
    });
  }

  const std::uint64_t before = arbc::image::default_pyramid_cache().decodes_issued();
  go.store(true, std::memory_order_release);

  // NO Catch2 macro runs while the readers are live: a failing `REQUIRE` would unwind past their
  // `join()`, and destroying a joinable `std::thread` calls `std::terminate`. Outcomes are
  // collected here and asserted after the join.
  const bool first_install = pending->install_asset(bytes);
  // ... and a burst of redundant installs behind it: publish-once must hold against a duplicate
  // arrival too. What must not move is the EXTENT: once it names a rectangle it names that
  // rectangle forever, because `bounds()` reads it on the compositor's CULL path and an image
  // whose bounds could revert would flicker out of the composition.
  const Rect installed{0.0, 0.0, fix::k_width, fix::k_height};
  bool reinstalls_ok = pending->available() && pending->bounds() == installed;
  for (int i = 0; i < k_rounds; ++i) {
    reinstalls_ok = reinstalls_ok && pending->install_asset(bytes) &&
                    pending->bounds() == installed && pending->pyramid() != nullptr;
  }
  stop.store(true, std::memory_order_release);
  for (std::thread& t : readers) {
    t.join();
  }

  CHECK(first_install);
  CHECK(reinstalls_ok); // the extent published once, and no redundant arrival moved it
  CHECK(harness_failures.load() == 0);
  CHECK(torn_reads.load() == 0); // never a third value
  CHECK(full_reads.load() > 0);  // the install genuinely became visible to the readers
  CHECK(pending->available());
  CHECK(pending->bounds() == installed);
  // EXACTLY ONE decode, however many installs were attempted: the arrival is admitted once, and
  // the default cache's budget is nowhere near tight enough to evict it again.
  CHECK(arbc::image::default_pyramid_cache().decodes_issued() == before + 1);
}

// enforces: 03-layer-plugin-interface#image-pyramid-publishes-once
// enforces: 03-layer-plugin-interface#image-re-decode-is-byte-identical-and-model-invisible
TEST_CASE("N workers rendering one image under continuous eviction never read a torn pyramid") {
  // kinds.image_master_budget puts the pyramid cache on EVERY RENDER THREAD, and this is the
  // race that buys: a one-byte budget means the pyramid is evicted the moment the last pin drops,
  // so the readers below are continuously decoding, publishing, reading and freeing pyramids
  // against each other -- the pointer they resolve through changes constantly, which is exactly
  // the history the retired monotonicity invariant forbade.
  //
  // The assertion is not "TSan is quiet". Every non-empty read is compared BYTE-FOR-BYTE against
  // the resident reference, so a half-built pyramid, a torn pin, or a pyramid freed out from
  // under a reader surfaces as a PIXEL MISMATCH -- the same discipline the late-install race
  // established, applied to the harder case.
  CpuBackend backend;
  auto reference = fix::make_content(); // un-keyed: outside the cache, outside the budget
  const std::optional<std::vector<float>> reference_tile = render_tile(*reference, backend);
  REQUIRE(reference_tile.has_value());
  REQUIRE_FALSE(reference_tile->empty());
  const std::vector<float>& expected = *reference_tile;

  arbc::image::PyramidCache cache(1); // no pyramid ever fits: every unpinned one is dropped
  auto content = fix::make_cached_content(cache, "stress/evicting/photo.ppm");
  REQUIRE(content->available());
  REQUIRE(content->render_thread_safe()); // the flag survives eviction -- that IS the claim
  REQUIRE(cache.resident_bytes() == 0);   // already evicted: nothing pins it

  constexpr int k_readers = 6;
  constexpr int k_renders = 40;
  std::atomic<bool> go{false};
  std::atomic<std::uint64_t> full_reads{0};
  std::atomic<std::uint64_t> torn_reads{0};
  std::atomic<std::uint64_t> empty_reads{0};
  std::atomic<std::uint64_t> harness_failures{0};

  std::vector<std::thread> readers;
  readers.reserve(k_readers);
  for (int r = 0; r < k_readers; ++r) {
    readers.emplace_back([&] {
      CpuBackend local; // each reader owns its backend: the race under test is the cache's
      while (!go.load(std::memory_order_acquire)) {
      }
      for (int i = 0; i < k_renders; ++i) {
        const std::optional<std::vector<float>> got = render_tile(*content, local);
        if (!got.has_value()) {
          harness_failures.fetch_add(1, std::memory_order_relaxed);
        } else if (got->empty()) {
          empty_reads.fetch_add(1, std::memory_order_relaxed);
        } else if (*got == expected) {
          full_reads.fetch_add(1, std::memory_order_relaxed);
        } else {
          torn_reads.fetch_add(1, std::memory_order_relaxed); // the bug
        }
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& t : readers) {
    t.join();
  }

  CHECK(harness_failures.load() == 0);
  CHECK(torn_reads.load() == 0);
  // An EVICTED image is not an UNAVAILABLE one: it always has pixels to answer with, because the
  // pull re-decodes. So NO render was ever culled -- which is the vanishing-layer regression, in
  // its concurrent form.
  CHECK(empty_reads.load() == 0);
  CHECK(full_reads.load() == static_cast<std::uint64_t>(k_readers) * k_renders);
  // ...and it genuinely thrashed rather than quietly staying resident: the budget really is being
  // enforced, so the byte-for-byte comparison above was not vacuous.
  CHECK(cache.evictions() > 1);
  CHECK(cache.decodes_issued() > 1);
  CHECK(content->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height}); // never reverted
}

// enforces: 03-layer-plugin-interface#image-pyramid-publishes-once
TEST_CASE("an evictor racing a live pin never frees pixels a render is reading") {
  // EVICT-WHILE-PINNED MUST NOT FREE -- the precise memory-safety risk the byte budget
  // introduces, and the reason the pin is an OWNING `shared_ptr` rather than a bare index into
  // the store (which is what makes it strictly stronger than `KeyedStore`'s `CacheHold`).
  //
  // One thread holds pins and reads pixels through them; another admits a stream of fresh decodes
  // that force `evict_to_fit` to run on every insert. ASan says no use-after-free; TSan says no
  // data race; and the pixel comparison says the pin was reading a COHERENT pyramid the whole
  // time, not merely a live allocation.
  const std::string bytes = fix::fixture_bytes();
  const std::size_t one_pyramid = fix::decode_fixture()->resident_bytes();

  // A budget of one pyramid: every admit must evict something, and the reader's pinned entry is
  // the one thing it may not choose (doc 02:268-277).
  arbc::image::PyramidCache cache(one_pyramid);
  const std::span<const unsigned char> span(reinterpret_cast<const unsigned char*>(bytes.data()),
                                            bytes.size());
  REQUIRE(cache.resolve("stress/pinned/photo.ppm", span) != nullptr);

  constexpr int k_rounds = 200;
  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> good_reads{0};
  std::atomic<std::uint64_t> bad_reads{0};

  std::thread reader([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    while (!stop.load(std::memory_order_acquire)) {
      const arbc::image::PyramidPin pin = cache.pin("stress/pinned/photo.ppm");
      if (!pin) {
        bad_reads.fetch_add(1, std::memory_order_relaxed); // a pull must ALWAYS get pixels
        continue;
      }
      // Read THROUGH the pin while the evictor races. If eviction could free a pinned pyramid,
      // this is where ASan would fire; if it could hand out a half-built one, the corners would
      // disagree with the extent.
      const bool coherent = pin->width() == fix::k_width && pin->height() == fix::k_height &&
                            pin->level_count() == 10 &&
                            pin->pixel(0, -4, -4) == pin->pixel(0, 0, 0);
      (coherent ? good_reads : bad_reads).fetch_add(1, std::memory_order_relaxed);
    }
  });

  std::thread evictor([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    for (int i = 0; i < k_rounds; ++i) {
      // A fresh URI every round, so every admit is a genuine decode that must evict to fit --
      // and the reader's pinned entry is never a legal victim while its pin is live.
      static_cast<void>(cache.resolve("stress/pinned/fill_" + std::to_string(i) + ".ppm", span));
    }
    stop.store(true, std::memory_order_release);
  });

  go.store(true, std::memory_order_release);
  evictor.join();
  reader.join();

  CHECK(bad_reads.load() == 0);
  CHECK(good_reads.load() > 0);
  CHECK(cache.evictions() > 0); // the evictor really was evicting under the reader
}

// enforces: 03-layer-plugin-interface#image-pyramid-publishes-once
TEST_CASE("on_ready firing from N threads while the writer settles, renders and saves") {
  // The production shape of the race: the arrival lands through `PendingExternalLoads::complete`
  // on whatever thread the source chose, while the writer thread is settling, rendering and
  // saving in a loop. `complete` copies the bytes under its mutex and touches no `Model`, no
  // `Document` and no `Content` -- so the only thing that ever mutates the content is the
  // writer-thread install, which is what "loading a file is async, mutating the document is not"
  // means operationally (doc 05:83).
  ThreadedDeferringSource source;
  source.put("stress/threads/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc("photo.ppm"), doc, bridge, registry,
                        "stress/threads/project.arbc", &source)
              .has_value());
  CHECK(source.requests() == 1);
  CHECK(doc.pending_external_loads() == 1);

  arbc::image::ImageContent* image = nullptr;
  doc.for_each_content([&image](ObjectId, Content* c) {
    if (auto* const found = dynamic_cast<arbc::image::ImageContent*>(c); found != nullptr) {
      image = found;
    }
  });
  REQUIRE(image != nullptr);

  // N threads all firing the SAME outstanding callback set. Exactly one of them wins the swap
  // inside `fire_all`; the rest fire nothing -- which is itself the point, because a source is
  // allowed to answer at most once per request and the queue must not care which thread does.
  constexpr int k_firers = 4;
  std::atomic<bool> go{false};
  std::vector<std::thread> firers;
  firers.reserve(k_firers);
  for (int i = 0; i < k_firers; ++i) {
    firers.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      static_cast<void>(source.fire_all());
    });
  }

  go.store(true, std::memory_order_release);
  // The writer thread, meanwhile, does exactly what a host frame loop does: settle, render, save.
  // The save goes through the codec table built FROM THE REGISTRY -- the image codec is gated on
  // the plugin being present, so the registry-free `builtin_codecs()` has no codec for the kind
  // and could not emit the layer at all.
  //
  // As above, no Catch2 macro runs while the firer threads are live.
  CpuBackend backend;
  const CodecTable codecs = builtin_codecs(registry);
  std::size_t settled = 0;
  bool renders_ok = true;
  bool saves_ok = true;
  for (int round = 0; round < 200 && settled == 0; ++round) {
    settled += settle_external_loads(doc, bridge, registry);
    renders_ok = renders_ok && render_tile(*image, backend).has_value();
    saves_ok = saves_ok && save_document(doc, bridge, codecs).has_value();
  }
  for (std::thread& t : firers) {
    t.join();
  }
  settled += settle_external_loads(doc, bridge, registry); // drain anything that landed at the end

  CHECK(renders_ok);   // a render never faulted, whichever side of the install it landed on
  CHECK(saves_ok);     // and the save never depended on the load state
  CHECK(settled == 1); // installed EXACTLY once, however many threads raced to deliver it
  CHECK(doc.pending_external_loads() == 0);
  CHECK(image->available());
  CHECK(source.requests() == 1); // and fetched exactly once
  CHECK(doc.pin()->revision() == 1);
}

// enforces: 08-serialization#pending-asset-is-not-unavailable
TEST_CASE("an arrival racing Document teardown installs nothing and faults nothing") {
  // The lifetime edge (Constraint 3): a fetch can outlive the document that started it, and a
  // user can close a project mid-download. Every `on_ready` captures a `weak_ptr` into the queue
  // the `Document` owns, so a callback whose queue has expired drops its bytes and returns.
  // ASan + TSan clean; no install after teardown.
  ThreadedDeferringSource source;
  source.put("stress/teardown/photo.ppm", fix::fixture_bytes());

  const Registry registry = image_registry();
  std::atomic<bool> go{false};
  std::thread firer([&] {
    while (!go.load(std::memory_order_acquire)) {
    }
    static_cast<void>(source.fire_all());
  });

  // No Catch2 macro inside the scope: it holds a joinable thread that is spinning on `go`, so an
  // early unwind would deadlock it and then `std::terminate` on its destructor.
  bool loaded = false;
  std::size_t pending_at_load = 0;
  {
    Document doc;
    KindBridge bridge;
    loaded = load_document(image_doc("photo.ppm"), doc, bridge, registry,
                           "stress/teardown/project.arbc", &source)
                 .has_value();
    pending_at_load = doc.pending_external_loads();
    go.store(true, std::memory_order_release);
  } // the Document is destroyed while the arrival may be in flight on the other thread

  firer.join();
  CHECK(loaded);
  CHECK(pending_at_load == 1);
  // The bytes landed on a queue that either still existed (and is now gone with its document) or
  // had already expired. Either way nothing was installed into a dead content, and the process is
  // still standing -- which is the whole assertion.
  CHECK(source.requests() == 1);
}
