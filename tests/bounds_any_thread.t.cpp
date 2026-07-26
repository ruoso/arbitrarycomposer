// `Content::bounds()` is ANY THREAD (issue #23, doc 03 § the contract's threading
// clauses).
//
// The guarantee was never stated, and the shipped code already depends on it: the
// compositor calls `bounds()` on the frame thread once per visible layer per frame --
// to cull the layer and, since `compositor.tile_apron`, to enforce its extent in the
// tile -- while the writer thread may be committing an edit. It has been safe only
// because every shipped kind fixes its extent at construction, which is an accident of
// the current kind set rather than a contract. A host hit-testing off the writer thread
// (the prescribed architecture: walk the document through the lock-free `pin()` seam
// and ask each resolved content for its extent) had no way to know whether that was
// legal.
//
// So the rule is stated in the direction the system relies on, and this is its
// enforcing test: a kind whose extent CHANGES under an edit must publish the change
// atomically, and a concurrent reader must observe one whole extent or the other,
// never a torn one. Registered in the TSan lanes, where a non-atomic publication is a
// reported race rather than a value this assertion happens to miss.

#include <arbc/base/geometry.hpp>
#include <arbc/contract/content.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using arbc::Content;
using arbc::Rect;

constexpr Rect k_small{0.0, 0.0, 10.0, 10.0};
constexpr Rect k_large{0.0, 0.0, 1000.0, 1000.0};

// A kind with a MUTABLE extent -- the case no shipped kind exercises yet, and the one
// the contract clause exists for. It publishes the whole rect behind an atomically
// swapped `shared_ptr`, which is the same shape the document's own version pointer and
// binding table use (issue #10) and the one a real kind should reach for: a `Rect` is
// four doubles, wider than any lock-free atomic, so "publish it atomically" means
// swapping a pointer to an immutable value, not hoping a 32-byte store is indivisible.
// A kind that stored the four doubles as plain members and wrote them one at a time
// would fail this under TSan, which is exactly the bug the clause forbids.
class GrowingContent final : public Content {
public:
  std::optional<Rect> bounds() const override { return *d_extent.load(); }
  arbc::Stability stability() const override { return arbc::Stability::Static; }
  std::optional<arbc::TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<arbc::RenderResult> render(const arbc::RenderRequest& request,
                                           std::shared_ptr<arbc::RenderCompletion>) override {
    return arbc::RenderResult{request.scale, true};
  }

  // The "edit": one atomic swap to a new immutable rect.
  void grow(const Rect& to) { d_extent.store(std::make_shared<const Rect>(to)); }

private:
  std::atomic<std::shared_ptr<const Rect>> d_extent{std::make_shared<const Rect>(k_small)};
};

} // namespace

// enforces: 03-layer-plugin-interface#bounds-is-any-thread
TEST_CASE("bounds() read off the writer thread observes whole extents, never torn ones") {
  GrowingContent content;
  std::atomic<bool> stop{false};
  std::atomic<std::size_t> torn{0};
  std::atomic<std::size_t> reads{0};

  // Two readers standing in for the two threads that really do this: the compositor's
  // frame thread and a host's UI hit-test.
  constexpr int k_readers = 2;
  std::vector<std::thread> readers;
  readers.reserve(k_readers);
  for (int i = 0; i < k_readers; ++i) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        const std::optional<Rect> seen = content.bounds();
        reads.fetch_add(1, std::memory_order_relaxed);
        if (!seen.has_value() || !(*seen == k_small || *seen == k_large)) {
          torn.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // The writer edits the extent repeatedly while they read.
  for (int i = 0; i < 2000; ++i) {
    content.grow((i % 2 == 0) ? k_large : k_small);
  }
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& t : readers) {
    t.join();
  }

  CHECK(torn.load() == 0);
  CHECK(reads.load() > 0); // the readers really ran; a vacuous pass would be worse
}
