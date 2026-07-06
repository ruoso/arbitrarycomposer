// Concurrency stress for compositor.pull_service (doc 16 tier-6, tsan lane;
// refinement Acceptance "TSan / stress"): many concurrent async pulls of
// thread-safe leaf content dispatched onto a real multi-worker WorkerPool. The
// pull engine probes and inserts the cache ONLY on the draining frame thread
// (single-writer, Decision 4); workers only run a leaf `render` into a
// thread-confined target and settle the thread-safe `RenderCompletion`. TSan must
// report no data race on the completion plumbing or the cache, and the resident
// bytes / eviction counts must be consistent after the drain (behavioral, never
// wall-clock). Cross-component (CpuBackend + WorkerPool + compositor), so it
// links the umbrella `arbc` and lives here (doc 17:153).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/compositor/refinement.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/worker_pool.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace {

using namespace arbc;

// A thread-safe leaf that renders inline-in-worker: it fills its (thread-confined)
// target and returns a `RenderResult`, so the worker settles `done` on the worker
// thread and the frame thread drains it -- exercising the completion handoff.
class SolidLeaf : public Content {
public:
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> /*done*/) override {
    std::span<std::byte> bytes = request.target.cpu_bytes();
    std::memset(bytes.data(), 0x5A, bytes.size_bytes());
    return RenderResult{request.scale, /*exact=*/true};
  }
  bool render_thread_safe() const override { return true; }
};

RenderRequest one_tile_request(Surface& target) {
  return RenderRequest{Rect::from_size(256.0, 256.0),
                       1.0,
                       Time::zero(),
                       StateHandle{},
                       target,
                       Exactness::BestEffort,
                       Deadline::none()};
}

} // namespace

// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("pull_service async: many concurrent worker pulls settle race-free into the cache") {
  constexpr int k_pulls = 64;
  constexpr std::size_t k_tile_bytes = 256u * 256u * 16u; // rgba32f 256^2

  CpuBackend backend;
  WorkerPoolConfig pool_config;
  pool_config.worker_count = 4;
  WorkerPool pool(pool_config);
  TileCache cache(256u * 1024 * 1024); // budget well above k_pulls tiles -> no eviction

  // Distinct leaf inputs -> distinct cache keys, one resident tile each.
  std::vector<std::unique_ptr<SolidLeaf>> leaves;
  std::unordered_map<const Content*, ObjectId> ids;
  for (int i = 0; i < k_pulls; ++i) {
    leaves.push_back(std::make_unique<SolidLeaf>());
    ids.emplace(leaves.back().get(), ObjectId{static_cast<std::uint32_t>(i + 1)});
  }

  RefinementQueue queue;
  PullConfig config;
  config.pending = &queue;
  config.id_of = [&ids](const Content* c) {
    const auto it = ids.find(c);
    return it != ids.end() ? it->second : ObjectId{};
  };
  config.contribution = [](const Content*) { return std::uint64_t{1}; };

  // Dispatch = submit to the real worker pool (the runtime binding shape,
  // `worker_pool.hpp:41`): over `contract` types only, so the compositor names no
  // runtime type at its call site.
  RenderDispatch dispatch = [&pool](Content* content, const RenderRequest& request,
                                    std::shared_ptr<RenderCompletion> done) {
    pool.submit(RenderTask{content, request, std::move(done)});
  };
  PullServiceImpl service(cache, backend, std::move(dispatch), config);

  // Issue every pull from the frame thread: each misses and dispatches to a
  // worker. A pull whose worker has not settled by the time it checks records the
  // render pending (occupying no worker); a pull whose worker already settled
  // inserts inline on THIS (frame) thread. Which of the two a given pull takes is
  // a timing detail -- both keep the cache single-writer on the frame thread and
  // insert the tile exactly once, so the test asserts only the timing-independent
  // final state, never the intermediate split (doc 16: behavioral, not wall-clock).
  // The caller's request target is only read for its format (the engine allocates
  // its own cache-destined surface), so a scratch surface scoped to the call
  // suffices; it outlives the `pull` and is never retained.
  for (int i = 0; i < k_pulls; ++i) {
    expected<std::unique_ptr<Surface>, SurfaceError> scratch =
        backend.make_surface(1, 1, k_working_rgba32f);
    REQUIRE(scratch.has_value());
    auto done = std::make_shared<RenderCompletion>();
    service.pull(leaves[static_cast<std::size_t>(i)].get(), one_tile_request(**scratch), done);
  }

  // Drain on the frame thread: poll settled arrivals into the cache, parking for a
  // worker completion only while pending renders remain. The park is bounded so a
  // coalesced wake cannot hang the drain; the loop exits on queue emptiness, never
  // on a timing assertion.
  while (!queue.tiles.empty()) {
    poll_refinements(queue, cache);
    if (queue.tiles.empty()) {
      break;
    }
    pool.wait_completions(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
  }

  // Every pull settled its tile into the cache exactly once -- whether inline or
  // via the pending drain -- so all `k_pulls` distinct keys are resident with no
  // eviction (budget was ample). This holds independently of the inline/pending
  // split above, and a missing or duplicated insert would fail it.
  CHECK(queue.tiles.empty());
  CHECK(cache.resident_bytes() == static_cast<std::size_t>(k_pulls) * k_tile_bytes);
  CHECK(cache.evictions() == 0);
}
