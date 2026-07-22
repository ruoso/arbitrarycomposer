// Tier-6 TSan lane (issue #10): the interactive render path walks the document's
// `id -> Content*` binding table (`d_contents`) LOCK-FREE while the single writer thread
// mutates it via `add_content`. Before the copy-on-write publish this was a data race on
// the map CONTAINER -- a concurrent `emplace` rehashes the bucket array out from under a
// reader iterating it. The table is now published copy-on-write behind an atomic, so
// `resolve()`/`for_each_content()` are pinned reads like `pin()` and never tear.
//
// This lane reproduces exactly the render-thread read chain the issue names
// (`for_each_content` -> walk each content plus its `inputs()`, `operator_binding.cpp:166`;
// `resolve` for a per-layer lookup) racing the writer's `add_content`. Assertions are
// OUTCOMES only -- the reader never observes a torn table (every visited pointer is live,
// the visited count is monotonic within a pass and reaches the writer's final count) and
// TSan is clean -- never a wall-clock assertion (doc 16). Catch2 macros are
// main-thread-only, so the reader latches its verdict into atomics.

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>

TEST_CASE("the content binding table is walked lock-free while the writer emplaces") {
  using arbc::Affine;
  using arbc::Content;
  using arbc::Document;
  using arbc::ObjectId;
  using arbc::Rgba;
  using arbc::SolidContent;

  Document doc;
  const ObjectId comp = doc.add_composition(1920.0, 1080.0);

  // Seed one content so the reader has a stable id to resolve from the very first pass.
  // The id is captured by value into the reader -- the two threads share only `doc`, whose
  // binding table is the thing under test, never a test-side container.
  const ObjectId seed = doc.add_content(std::make_shared<SolidContent>(Rgba{1.0F, 0.5F, 0.25F, 1.0F}));
  doc.attach_layer(comp, doc.add_layer(seed, Affine::identity(), 1.0));

  constexpr int k_iterations = 4000;
  std::atomic<bool> torn{false};        // a visited Content* was null / unwalkable
  std::atomic<bool> shrank{false};      // a single pass saw the table lose rows (impossible)
  std::atomic<bool> reader_done{false};
  std::atomic<int> last_seen{0};

  // The render-thread read chain: `for_each_content` walks every top-level content plus
  // each content's `inputs()` -- the exact `bind_operators` walk -- and `resolve` does the
  // per-id lookup a layer binding performs. Both must be lock-free against `add_content`.
  std::thread reader([&] {
    while (!reader_done.load(std::memory_order_acquire)) {
      std::size_t count = 0;
      doc.for_each_content([&](Content* c) {
        if (c == nullptr) {
          torn.store(true);
          return;
        }
        // Touch the vtable and the inputs span exactly as the operator binder does; a
        // torn read of the container (rehash mid-walk) would surface as a bad pointer
        // here under TSan/ASan rather than as a clean visit.
        static_cast<void>(c->stability());
        static_cast<void>(c->inputs().size());
        ++count;
      });
      // The table only ever grows (rows are never erased live), so within one pinned pass
      // the count can never regress below what a prior pass saw.
      const int seen = static_cast<int>(count);
      if (seen < last_seen.load(std::memory_order_relaxed)) {
        shrank.store(true);
      }
      last_seen.store(seen, std::memory_order_relaxed);

      // A lock-free lookup of a known id must always resolve to a live pointer.
      if (doc.resolve(seed) == nullptr) {
        torn.store(true);
      }
    }
  });

  // The main thread is the single writer: append a fresh content + layer each round.
  for (int i = 0; i < k_iterations; ++i) {
    const ObjectId c =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 0.0F, 0.0F, 1.0F}));
    doc.attach_layer(comp, doc.add_layer(c, Affine::identity(), 1.0));
  }

  reader_done.store(true, std::memory_order_release);
  reader.join();

  CHECK_FALSE(torn.load());
  CHECK_FALSE(shrank.load());
  // The writer added k_iterations contents on top of the seed; the table is the union.
  CHECK(static_cast<std::size_t>(k_iterations + 1) == [&] {
    std::size_t n = 0;
    doc.for_each_content([&](Content*) { ++n; });
    return n;
  }());
}
