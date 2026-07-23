// Tier-6 TSan lane (issue #13): the two seams where a `HostViewport` driven from a RENDER
// thread meets the document's WRITER thread.
//
// `HostViewport::step()` is render-thread-confined by design (doc 02 § Threading model: frame
// planning "reads the scene under a snapshot ... so planning never races edits and never takes
// a lock"), but step 0 used to run the external-arrival settle -- a structural writer publish
// (`Model::Transaction` + `add_content` + commit) -- unconditionally. On a host that edits from
// its UI thread and renders from another, that made the render thread a SECOND writer identity,
// which doc 15 § Thread rules forbids outright and which no host-side mutex can repair: the
// lock-free growth path (relaxed `high_water`, `SlabDirectory::publish`, the writer-thread
// checkpoint seal) is written against one mutator, and a mutex re-covers accesses, not
// identity. The host could not fix it either -- the call site is the library's.
//
// The second seam is the damage handoff: `DamageAccumulator::flush` runs inside a commit, on
// the writer thread, while `step()` drains it on the render thread.
//
// Assertions are OUTCOMES over deterministic hand-offs, never timings (doc 16): what a step
// installed, what it reported, how many frames each commit produced. Catch2 macros are
// main-thread-only, so the render thread latches into atomics and every CHECK runs after the
// join.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using arbc::Affine;
using arbc::CpuBackend;
using arbc::DocStatePtr;
using arbc::Document;
using arbc::KindBridge;
using arbc::ObjectId;
using arbc::Rect;
using arbc::Registry;
using arbc::Rgba;
using arbc::SolidContent;
using arbc::Surface;
using arbc::SurfacePool;
using arbc::TileCache;
using arbc::Viewport;

// The DEFERRING `AssetSource` double (the shape `tests/async_external_load.t.cpp` established):
// `request()` records the continuation and fires nothing, so ARRIVAL is a thing this test
// schedules. No sleeps, no polling.
class DeferringAssetSource final : public arbc::AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.insert_or_assign(std::move(uri), std::move(bytes));
  }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    d_outstanding.push_back(Request{std::string(resolved_uri), std::move(on_ready)});
  }

  std::size_t fire_all() {
    std::vector<Request> firing;
    firing.swap(d_outstanding);
    for (const Request& r : firing) {
      const auto it = d_files.find(r.uri);
      r.on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
    }
    return firing.size();
  }

private:
  struct Request {
    std::string uri;
    std::function<void(std::string_view)> on_ready;
  };

  std::unordered_map<std::string, std::string> d_files;
  std::vector<Request> d_outstanding;
};

// A one-layer document embedding `ref` through org.arbc.nested, and the leaf it embeds.
std::string nesting_doc(std::string_view ref) {
  std::string layer = R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":")";
  layer += ref;
  layer += R"("}})";
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)" + layer + "]}}";
}

constexpr const char* k_leaf =
    R"({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[)"
    R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[0.0,1.0,0.0,1.0]}}]}})";

ObjectId root_composition_of(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  return root;
}

// A constant clock: the free-run transport advances zero flicks per step, so a still,
// undamaged scene is genuinely still and nothing below reads a wall clock.
arbc::HostViewport::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// The whole host side of a `Document`-bound viewport in one object: the resolver, the
// damage-sink install, the settle hook and the readiness probe are all derived by the
// constructor. The worker pool is the thread-free inline executor (`WorkerPoolConfig{}`), so a
// step issues a frame for exactly one reason -- drained damage -- and never because a worker is
// still in flight.
class DocumentViewport {
public:
  DocumentViewport(Document& doc, KindBridge& bridge, const Registry& registry, int dim)
      : d_cache(64U * 1024 * 1024), d_pool(d_backend),
        d_target(d_backend.make_surface(dim, dim, doc.pin()->working_space())),
        d_viewport(d_renderer, doc, arbc::HostViewport::DocumentBinding{&bridge, &registry},
                   d_backend, d_pool, d_cache, checked(d_target), epoch_clock(), config(doc, dim)) {
  }

  arbc::HostViewport& operator*() noexcept { return d_viewport; }
  arbc::HostViewport* operator->() noexcept { return &d_viewport; }

private:
  using Target = arbc::expected<std::unique_ptr<Surface>, arbc::SurfaceError>;

  static Surface& checked(Target& target) {
    REQUIRE(target.has_value());
    return **target;
  }

  static arbc::HostViewport::Config config(const Document& doc, int dim) {
    arbc::HostViewport::Config cfg;
    cfg.viewport = Viewport{dim, dim, Affine::identity(), root_composition_of(doc)};
    cfg.budget = std::chrono::hours(1); // no deadline pressure: nothing degrades
    return cfg;
  }

  CpuBackend d_backend;
  TileCache d_cache;
  SurfacePool d_pool;
  Target d_target;
  arbc::InteractiveRenderer d_renderer{{}, epoch_clock()};
  arbc::HostViewport d_viewport;
};

// A scene with one solid layer, built (and therefore WRITER-BOUND) on the calling thread.
struct SolidScene {
  static constexpr int k_dim = 16;

  explicit SolidScene(Document& doc)
      : composition(doc.add_composition(k_dim, k_dim)),
        content(doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F},
                                                               Rect{0.0, 0.0, k_dim, k_dim}))),
        layer(doc.add_layer(content, Affine::identity(), 1.0)) {
    doc.attach_layer(composition, layer);
  }

  ObjectId composition;
  ObjectId content;
  ObjectId layer;
};

// Spin until `flag` reaches `value`, yielding. A hand-off, not a timeout: the partner thread
// always arrives, so there is no wall-clock deadline to flake on.
void await(const std::atomic<int>& flag, int value) {
  while (flag.load(std::memory_order_acquire) < value) {
    std::this_thread::yield();
  }
}

} // namespace

// enforces: 02-architecture#frame-step-publishes-only-on-the-writer-thread
TEST_CASE("a step off the writer thread installs no arrival and reports the owed settle") {
  // The issue, reproduced and closed. The document is loaded on THIS thread, so this thread is
  // its writer identity; the frame loop runs on another, as doc 02 says a compositor may. The
  // arrival's bytes have landed. Before the fix, that step opened a transaction and committed
  // -- a second writer identity. Now it publishes nothing and says so.
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));
  REQUIRE(doc.pending_external_loads() == 1);
  REQUIRE(doc.on_writer_thread()); // the load bound the identity here

  arbc::register_builtin_operator_binders();
  DocumentViewport view(doc, bridge, registry, 16);

  // The bytes arrive. `on_ready` only queues them (it touches no `Model`), so nothing is
  // installed yet and the readiness probe -- lock-free, any-thread -- says so.
  REQUIRE(source.fire_all() == 1);
  CHECK(doc.external_loads_ready() == 1);
  const std::uint64_t revision_before = doc.pin()->revision();

  // One step, on a thread that is NOT the writer.
  std::size_t reported = 0;
  bool render_thread_is_writer = true;
  std::thread render([&] {
    render_thread_is_writer = doc.on_writer_thread();
    reported = view->step().external_loads_ready;
  });
  render.join();

  CHECK_FALSE(render_thread_is_writer); // the model knows the difference ...
  CHECK(reported == 1);                 // ... and the step reports rather than publishes
  CHECK(view->external_loads_settled() == 0);
  CHECK(doc.pending_external_loads() == 1);        // nothing installed
  CHECK(doc.external_loads_ready() == 1);          // the arrival is still queued
  CHECK(doc.pin()->revision() == revision_before); // no publish: no revision
  CHECK(view->frames_issued() == 1);               // it still RENDERED (the bootstrap frame)

  // The host now pumps the settle on ITS writer thread -- the one seam #13 asks for. The
  // install publishes a revision and flushes damage naming the embedding content, which
  // reaches the viewport's sink through the ordinary seam.
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(doc.external_loads_ready() == 0);
  CHECK(doc.pin()->revision() > revision_before);

  // ... so the NEXT render-thread step composites the arrival: one frame later than a
  // single-threaded host's, and never from two writers.
  std::thread render_again([&] { view->step(); });
  render_again.join();
  CHECK(view->frames_issued() == 2);
}

// enforces: 02-architecture#frame-step-publishes-only-on-the-writer-thread
TEST_CASE("the same step ON the writer thread settles inline, exactly as it always did") {
  // The control, and the compatibility guarantee: a single-threaded host -- every driver in
  // this tree, and the shipped `examples/host-interactive` -- is byte-for-byte unaffected. The
  // arrival still settles inside the very step that observes it.
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));
  arbc::register_builtin_operator_binders();
  DocumentViewport view(doc, bridge, registry, 16);

  REQUIRE(source.fire_all() == 1);
  REQUIRE(doc.external_loads_ready() == 1);

  const arbc::HostViewport::StepOutcome outcome = view->step();

  CHECK(outcome.external_loads_ready == 0); // nothing OWED: it was installed here
  CHECK(view->external_loads_settled() == 1);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(doc.external_loads_ready() == 0);
  CHECK(doc.pin()->revision() > 0);
}

// enforces: 02-architecture#frame-step-publishes-only-on-the-writer-thread
TEST_CASE("an arrival the render thread declined to install lands at the host's next edit") {
  // The report is a latency optimization, not an obligation. This host ignores
  // `StepOutcome::external_loads_ready` completely -- it never reads the field, never calls
  // `settle_external_loads` -- and simply goes on editing. The arrival still installs, on the
  // writer thread, immediately before the edit that follows it: the `Document`-bound viewport
  // handed the document its settle hook, and `Document::begin()` runs it there.
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));
  arbc::register_builtin_operator_binders();
  DocumentViewport view(doc, bridge, registry, 16);

  REQUIRE(source.fire_all() == 1);
  std::thread render([&] { view->step(); }); // off the writer thread: publishes nothing
  render.join();
  REQUIRE(doc.pending_external_loads() == 1);
  REQUIRE(doc.external_loads_auto_settled() == 0);

  // Any edit at all -- the host's ordinary writer-thread work.
  doc.add_composition(4.0, 4.0);

  CHECK(doc.external_loads_auto_settled() == 1);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(doc.external_loads_ready() == 0);

  // And it is not a per-edit cost: with nothing arrived, the next edit settles nothing.
  doc.add_composition(4.0, 4.0);
  CHECK(doc.external_loads_auto_settled() == 1);
}

// enforces: 15-memory-model#writer-identity-is-queryable
TEST_CASE("the writer identity binds on the first structural write and never rebinds") {
  Document doc;

  // Unbound: nobody has written, so ANY thread may -- it would become the writer.
  bool unbound_elsewhere = false;
  std::thread probe([&] { unbound_elsewhere = doc.on_writer_thread(); });
  probe.join();
  CHECK(unbound_elsewhere);

  // The first transaction binds this thread ...
  const SolidScene scene(doc);
  CHECK(doc.on_writer_thread());

  // ... and every other thread is now, correctly, not the writer -- including after further
  // edits, which never rebind.
  bool bound_elsewhere = true;
  std::thread other([&] { bound_elsewhere = doc.on_writer_thread(); });
  other.join();
  CHECK_FALSE(bound_elsewhere);

  doc.set_layer_transform(scene.layer, Affine::identity());
  bool still_bound_elsewhere = true;
  std::thread again([&] { still_bound_elsewhere = doc.on_writer_thread(); });
  again.join();
  CHECK_FALSE(still_bound_elsewhere);
  CHECK(doc.on_writer_thread());
}

// enforces: 15-memory-model#viewport-damage-handoff-crosses-writer-to-render
TEST_CASE("every writer-thread commit's damage reaches a render-thread frame") {
  // The damage handoff, hand-off-ordered so it is an exact count and not a probability: the
  // writer commits one damaging edit, the render thread takes exactly one step, and that step
  // MUST issue a frame -- the idle gate (Constraint 7) issues zero frames for an undamaged,
  // still scene, so a frame is precisely the witness that the drained batch was not lost
  // across the thread boundary.
  Document doc;
  const SolidScene scene(doc);
  KindBridge bridge;
  const Registry registry;
  arbc::register_builtin_operator_binders();
  DocumentViewport view(doc, bridge, registry, SolidScene::k_dim);

  constexpr int k_rounds = 64;
  std::atomic<int> commits{0};
  std::atomic<int> steps{0};
  std::atomic<int> silent_rounds{0}; // a round whose step issued NO frame: damage was lost

  std::thread render([&] {
    for (int round = 1; round <= k_rounds; ++round) {
      await(commits, round);
      const std::uint64_t before = view->frames_issued();
      view->step();
      if (view->frames_issued() == before) {
        silent_rounds.fetch_add(1, std::memory_order_relaxed);
      }
      steps.store(round, std::memory_order_release);
    }
  });

  for (int round = 1; round <= k_rounds; ++round) {
    // A placement edit auto-damages the object it names, in one commit, flushed once
    // (01-core-concepts#placement-change-auto-damages) -- on THIS thread, the writer's.
    doc.set_layer_transform(scene.layer, Affine::identity());
    commits.store(round, std::memory_order_release);
    await(steps, round);
  }
  render.join();

  CHECK(silent_rounds.load() == 0);
  CHECK(view->frames_issued() == k_rounds);
}

// enforces: 15-memory-model#viewport-damage-handoff-crosses-writer-to-render
TEST_CASE("the writer commits while the render thread steps, unsynchronized") {
  // The TSan lane proper: no hand-off at all. The writer churns damaging commits -- each one
  // flushing into the viewport's accumulator from the writer thread -- while the render thread
  // steps in a tight loop, draining that same accumulator and compositing. Before the fix this
  // was an unguarded `std::vector<Damage>` written by one thread and swapped by another.
  //
  // Outcomes only: every step returns, the frame count is bounded by what actually happened,
  // and the accumulator is still CONSISTENT afterwards -- proven by one ordered round after
  // the race, which must still turn a commit into a frame.
  Document doc;
  const SolidScene scene(doc);
  KindBridge bridge;
  const Registry registry;
  arbc::register_builtin_operator_binders();
  DocumentViewport view(doc, bridge, registry, SolidScene::k_dim);

  constexpr int k_rounds = 2000;
  std::atomic<bool> stop{false};
  std::atomic<int> frames{0};

  std::thread render([&] {
    while (!stop.load(std::memory_order_acquire)) {
      view->step();
      frames.fetch_add(1, std::memory_order_relaxed);
    }
  });

  for (int i = 0; i < k_rounds; ++i) {
    doc.set_layer_transform(scene.layer, Affine::identity());
  }
  stop.store(true, std::memory_order_release);
  render.join();

  CHECK(frames.load() > 0);
  CHECK(doc.pin()->revision() >= static_cast<std::uint64_t>(k_rounds));

  // The accumulator survived the race intact: one more commit, one more step, one more frame.
  const std::uint64_t before = view->frames_issued();
  doc.set_layer_transform(scene.layer, Affine::identity());
  std::thread final_step([&] { view->step(); });
  final_step.join();
  CHECK(view->frames_issued() == before + 1);
}
