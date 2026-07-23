// Tier-6 TSan lane (issue #15): a host whose UI thread is not its writer thread asks
// `journal().can_undo()` / `can_redo()` EVERY FRAME -- that is what enables and disables the
// undo/redo affordances -- while the writer thread commits, undoes and redoes. Before the
// cursor and the entry count were published, those accessors were plain reads of `d_cursor`
// and `d_entries` that the writer mutated concurrently: `can_redo()` read `d_entries.size()`
// across a `push_back` that may reallocate. Formally UB, TSan-observable, and unavoidable
// for a consumer following doc 15's "funnel writes onto one dedicated writer thread" -- which
// is exactly what moves the UI thread off the writer.
//
// The two values are now relaxed atomics the writer publishes after it has finished mutating
// the entry vector. Assertions are OUTCOMES only (doc 16): the reader never observes a value
// the writer never held, never observes an affordance the history does not offer (the
// published pair is conservative -- it can be a frame late offering an undo/redo, never a
// frame early), and the run is TSan-clean. Never a wall-clock assertion. Catch2 macros are
// main-thread-only, so the reader latches its verdict into atomics.

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace {

// One layer over one solid content: the smallest document whose transform edits are
// ordinary undoable commits (no kind state, so the journal's cost seam stays out of it).
struct Scene {
  arbc::Document doc;
  arbc::ObjectId composition;
  arbc::ObjectId layer;

  Scene() {
    composition = doc.add_composition(1920.0, 1080.0);
    const arbc::ObjectId content =
        doc.add_content(std::make_shared<arbc::SolidContent>(arbc::Rgba{0.2F, 0.4F, 0.8F, 1.0F}));
    layer = doc.add_layer(content, arbc::Affine::identity(), 1.0);
    doc.attach_layer(composition, layer);
  }

  // One undoable edit: a transform commit through the host-facing transaction seam.
  void edit(double scale) {
    auto txn = doc.transact("scale");
    txn.set_transform(layer, arbc::Affine::scaling(scale, scale));
    REQUIRE(txn.commit().has_value());
  }
};

} // namespace

// enforces: 14-data-model-and-editing#history-enable-state-is-any-thread
TEST_CASE("the undo/redo enable state is read from a non-writer thread while the writer commits") {
  Scene scene;
  const arbc::Journal& journal = scene.doc.journal();

  // The document's setup already committed; start the reader from a known base so the
  // counts below are the writer's own.
  const std::size_t base_depth = journal.depth();
  REQUIRE(journal.cursor() == base_depth);

  constexpr int k_commits = 4000;
  const std::size_t max_depth = base_depth + k_commits;

  std::atomic<bool> stop{false};
  std::atomic<bool> impossible_value{false};   // a count the writer never held
  std::atomic<bool> depth_regressed{false};    // this lane only appends: depth never shrinks
  std::atomic<bool> cursor_regressed{false};   // ... nor does the cursor
  std::atomic<bool> invented_redo{false};      // a redo offered while the writer never undid
  std::atomic<bool> undo_without_entry{false}; // can_undo() true with an empty history
  std::atomic<std::size_t> polls{0};

  // The UI-frame read: exactly the four accessors a host samples to paint its undo/redo
  // affordances, sampled as fast as the thread can loop against a churning writer.
  std::thread reader([&] {
    std::size_t last_depth = 0;
    std::size_t last_cursor = 0;
    // At least one poll always happens, so `polls > 0` below witnesses the reader rather
    // than a lucky interleaving with the writer's `stop`.
    do {
      const bool can_undo = journal.can_undo();
      const bool can_redo = journal.can_redo();
      const std::size_t cursor = journal.cursor();
      const std::size_t depth = journal.depth();

      if (depth > max_depth || cursor > max_depth) {
        impossible_value.store(true, std::memory_order_relaxed);
      }
      // Each atomic is published by one thread and only ever grows in this lane, so a
      // regression in either would be a torn or stale-forever read, not a race artifact.
      if (depth < last_depth) {
        depth_regressed.store(true, std::memory_order_relaxed);
      }
      if (cursor < last_cursor) {
        cursor_regressed.store(true, std::memory_order_relaxed);
      }
      last_depth = depth;
      last_cursor = cursor;

      // The writer never navigates in this lane, so there is never anything to redo. A
      // `true` here would be the published pair inventing an affordance out of the gap
      // between its two stores -- which the writer's store order rules out.
      if (can_redo) {
        invented_redo.store(true, std::memory_order_relaxed);
      }
      // The cursor is published only after the entry is in the vector, so an offered undo
      // always has an entry behind it. `depth` is read after `can_undo` and only grows.
      if (can_undo && depth == 0) {
        undo_without_entry.store(true, std::memory_order_relaxed);
      }
      polls.fetch_add(1, std::memory_order_relaxed);
    } while (!stop.load(std::memory_order_acquire));
  });

  // The main thread is the single writer.
  for (int i = 0; i < k_commits; ++i) {
    scene.edit(1.0 + static_cast<double>(i % 8) * 0.125);
  }

  stop.store(true, std::memory_order_release);
  reader.join();

  CHECK_FALSE(impossible_value.load());
  CHECK_FALSE(depth_regressed.load());
  CHECK_FALSE(cursor_regressed.load());
  CHECK_FALSE(invented_redo.load());
  CHECK_FALSE(undo_without_entry.load());
  CHECK(polls.load() > 0); // the reader really ran against the writer

  // The published pair agrees with the writer once the two threads are joined: every commit
  // is an entry, the cursor is at the tip, and the affordances say so.
  CHECK(journal.depth() == max_depth);
  CHECK(journal.cursor() == max_depth);
  CHECK(journal.can_undo());
  CHECK_FALSE(journal.can_redo());
}

// enforces: 14-data-model-and-editing#history-enable-state-is-any-thread
TEST_CASE("the enable state stays consistent while the writer navigates undo/redo cycles") {
  Scene scene;
  const arbc::Journal& journal = scene.doc.journal();

  // Two edits above the document's setup commits: the writer toggles the top one, so an
  // undo is available for the whole run and the cursor never reaches the base.
  scene.edit(2.0);
  scene.edit(3.0);
  const std::size_t depth = journal.depth();
  REQUIRE(depth >= 2);
  REQUIRE(journal.cursor() == depth);

  constexpr int k_cycles = 3000;
  std::atomic<bool> stop{false};
  std::atomic<bool> impossible_value{false};
  std::atomic<bool> undo_lost{false}; // an undo blinked off while one always existed
  std::atomic<std::size_t> polls{0};

  std::thread reader([&] {
    do {
      const bool can_undo = journal.can_undo();
      const std::size_t cursor = journal.cursor();
      const std::size_t d = journal.depth();
      // The writer only ever moves the cursor between depth-1 and depth here.
      if (d != depth || cursor > depth || cursor + 1 < depth) {
        impossible_value.store(true, std::memory_order_relaxed);
      }
      if (!can_undo) {
        undo_lost.store(true, std::memory_order_relaxed);
      }
      // `can_redo()` genuinely oscillates in this lane, so it is exercised (both stores are
      // read every pass) but not asserted -- there is no frame at which its value is fixed.
      static_cast<void>(journal.can_redo());
      polls.fetch_add(1, std::memory_order_relaxed);
    } while (!stop.load(std::memory_order_acquire));
  });

  for (int i = 0; i < k_cycles; ++i) {
    REQUIRE(scene.doc.journal().undo());
    scene.doc.drain();
    REQUIRE(scene.doc.journal().redo());
    scene.doc.drain();
  }

  stop.store(true, std::memory_order_release);
  reader.join();

  CHECK_FALSE(impossible_value.load());
  CHECK_FALSE(undo_lost.load());
  CHECK(polls.load() > 0);
  CHECK(journal.depth() == depth);
  CHECK(journal.cursor() == depth);
  CHECK_FALSE(journal.can_redo());
}

// enforces: 14-data-model-and-editing#history-enable-state-is-any-thread
TEST_CASE("a stale enable read costs a refused no-op, never a wrong mutation") {
  Scene scene;
  arbc::Journal& journal = scene.doc.journal();

  scene.edit(2.0);
  const std::size_t depth = journal.depth();

  // Walk the cursor to the base: the last undo is the one a stale `can_undo() == true`
  // would have dispatched a frame later.
  while (journal.can_undo()) {
    REQUIRE(journal.undo());
  }
  CHECK(journal.cursor() == 0);
  CHECK_FALSE(journal.can_undo());
  CHECK(journal.can_redo());

  const std::uint64_t revision = scene.doc.pin()->revision();
  // The writer re-checks on its own thread: the dispatched undo is refused, publishing
  // nothing -- no revision, no cursor move, no entry lost.
  CHECK_FALSE(journal.undo());
  CHECK(scene.doc.pin()->revision() == revision);
  CHECK(journal.cursor() == 0);
  CHECK(journal.depth() == depth);

  // Symmetrically at the tip.
  while (journal.can_redo()) {
    REQUIRE(journal.redo());
  }
  const std::uint64_t tip_revision = scene.doc.pin()->revision();
  CHECK_FALSE(journal.redo());
  CHECK(scene.doc.pin()->revision() == tip_revision);
  CHECK(journal.cursor() == depth);
}
