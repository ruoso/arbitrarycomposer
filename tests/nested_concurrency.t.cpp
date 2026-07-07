#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/worker_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Concurrency / TSan stress for org.arbc.nested (doc 16 tier-6, tsan lane). A
// nested scene is rendered many times CONCURRENTLY through a real multi-worker
// WorkerPool: each worker runs nested's synchronous per-layer descent on its own
// (worker) thread, the child leaf renders happening across workers. Nested
// declares render_thread_safe() == true, so all tasks run concurrently on the
// shared queue. Asserts no data race (under TSan) and deterministic output --
// every concurrent render equals the single-threaded reference. Reuses the
// pull_service async harness shape (WorkerPool + drain loop).

namespace {

using namespace arbc;

// Stateless inline honoring of the abstract PullService contract: renders each
// child into the request's target and settles `done`. No mutable state, so it is
// itself safe under concurrent descent from many workers.
class InlinePull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }
};

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

} // namespace

TEST_CASE("nested renders race-free and deterministically across worker threads") {
  Model model;
  SolidContent solid_a{Rgba{0.6F, 0.2F, 0.1F, 0.8F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent solid_b{Rgba{0.1F, 0.4F, 0.3F, 0.5F}, Rect{0.0, 0.0, 8.0, 8.0}};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  {
    auto tx = model.transact("scene");
    comp = tx.add_composition(8.0, 8.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::translation(1.0, 1.0));
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &solid_a;
    binding[cb] = &solid_b;
  }
  const DocStatePtr doc = model.current();

  CpuBackend backend;
  InlinePull pull;
  NestedContent nested(comp);
  nested.attach(
      pull, backend,
      [&binding](ObjectId id) -> Content* {
        const auto it = binding.find(id);
        return it != binding.end() ? it->second : nullptr;
      },
      *doc);

  const Rect region{0.0, 0.0, 8.0, 8.0};

  // Single-threaded reference render.
  auto ref_target = backend.make_surface(8, 8, k_working_rgba32f);
  REQUIRE(ref_target.has_value());
  const RenderRequest ref_req{region,     1.0,           Time::zero(), StateHandle{},
                              **ref_target, Exactness::Exact, Deadline::none()};
  auto ref_done = std::make_shared<RenderCompletion>();
  REQUIRE(nested.render(ref_req, ref_done).has_value());
  const std::vector<std::byte> ref_bytes = bytes_of(**ref_target);

  // Many concurrent renders of the SAME nested content on a real worker pool.
  constexpr int k_tasks = 64;
  std::vector<std::unique_ptr<Surface>> targets;
  std::vector<std::shared_ptr<RenderCompletion>> dones;
  targets.reserve(k_tasks);
  dones.reserve(k_tasks);
  for (int i = 0; i < k_tasks; ++i) {
    auto t = backend.make_surface(8, 8, k_working_rgba32f);
    REQUIRE(t.has_value());
    targets.push_back(std::move(*t));
    dones.push_back(std::make_shared<RenderCompletion>());
  }

  WorkerPoolConfig config;
  config.worker_count = 4;
  WorkerPool pool(config);
  for (int i = 0; i < k_tasks; ++i) {
    // The Surface objects live on the heap behind the unique_ptrs, so binding a
    // reference into the RenderRequest is stable regardless of vector growth.
    RenderTask task{&nested,
                    RenderRequest{region, 1.0, Time::zero(), StateHandle{}, *targets[static_cast<std::size_t>(i)],
                                  Exactness::Exact, Deadline::none()},
                    dones[static_cast<std::size_t>(i)]};
    pool.submit(std::move(task));
  }

  // Drain the pool (bounded park loop; never a fixed sleep or wall-clock gate).
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (pool.tasks_completed() < static_cast<std::uint64_t>(k_tasks)) {
    pool.wait_completions(std::chrono::steady_clock::now() + std::chrono::milliseconds(50));
    if (std::chrono::steady_clock::now() > deadline) {
      break;
    }
  }

  // Every concurrent render settled and produced byte-identical output to the
  // reference: no data race corrupted a shared read, and the descent is
  // deterministic.
  REQUIRE(pool.tasks_completed() == static_cast<std::uint64_t>(k_tasks));
  for (int i = 0; i < k_tasks; ++i) {
    REQUIRE(dones[static_cast<std::size_t>(i)]->settled());
    REQUIRE(bytes_of(*targets[static_cast<std::size_t>(i)]) == ref_bytes);
  }
}
