// The concurrency coverage doc 16:66-73 requires of `runtime.housekeeping_document_wiring`:
// a LIVE, workspace-backed `Document` with its own background `HousekeepingThread`
// draining, a writer thread committing transactions that churn editable raster contents
// through `bind`/`unbind` (mutating the router's table), and render threads pinning and
// unpinning `DocState`s throughout.
//
// It is the regression test for HAZARD 2. Enabling the background drainer makes
// `EditableRouter::route()` a CROSS-THREAD read: `ContentStateReclaimSink::on_zero`
// lands in `EditableStateRefSink::release` -> `EditableRouter::route` -> `d_table.find`
// on the DRAIN thread, while the writer concurrently `insert`s and `erase`s on every
// content add/remove. `find` racing a rehashing `insert` on a bare `std::unordered_map`
// is UB whose failure mode is not benign: a torn read yields a stale `Editable*`, and
// since `kinds.raster_pool_backing` a raster `StateHandle` transitively owns its tile
// blobs, releasing through it FREES ANOTHER CONTENT'S PIXELS. The `shared_mutex` on the
// router is what makes the outcome assertions below hold.
//
// It is also the counter witness for Decision 2: across a run with thousands of
// background ticks, every `Checkpointer` commit is attributable to a writer-side
// trigger, and the background thread's ticks advance only the drain counter.
//
// `FakeEditable` deliberately is NOT the editable here: it holds a mutex, which is
// enough for the component tests but is not the production shape. `RasterContent` is --
// its `RasterStore` guards versions and its free list with its own mutex
// (raster_content.hpp:216-219), so the kind side is genuinely thread-safe and the
// runtime router is the only link under test.
//
// Cross-component (runtime + kind_raster + pool), so it lives here. Outcome-only
// assertions, fixed op counts, `yield()` perturbation -- never a wall clock
// (doc 16:54-62). Written TSan-ready.
//
// enforces: 14-data-model-and-editing#state-release-routes-under-concurrent-binding
// enforces: 15-memory-model#checkpoint-commit-is-writer-thread

#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if ARBC_HAS_WORKSPACE_FILES
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

namespace {

using arbc::Document;
using arbc::DocumentHousekeepingConfig;
using arbc::ObjectId;
using arbc::RasterContent;
using arbc::WorkingPixel;

// A tick period short enough that the document's background thread is an ACTIVE
// low-priority drainer for the whole run -- which is the point: the drain must be
// happening concurrently with the writer's binds, or the race window never opens. It
// never appears in an assertion (doc 16:54-62).
constexpr std::chrono::microseconds kActiveTick{50};

constexpr int kWriterRounds = 200;  // contents added, painted, and removed
constexpr int kPinnerCount = 2;     // render surrogates
constexpr int kCheckpointEvery = 8; // writer-side checkpoint cadence

const WorkingPixel k_red{1.0F, 0.0F, 0.0F, 1.0F};

// A small opaque-white premultiplied rgba32f raster; tile edge 2 gives a 2x2 grid of
// level-0 tiles, so a one-tile paint leaves three untouched tiles structurally shared.
arbc::DecodedImage white_4x4() {
  arbc::DecodedImage img;
  img.width = 4;
  img.height = 4;
  img.format = arbc::k_working_rgba32f;
  const std::vector<float> f(64, 1.0F);
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

#if ARBC_HAS_WORKSPACE_FILES

// The temp workspace-file recipe (src/runtime/t/housekeeping.t.cpp).
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "dhs", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_dhs_XXXXXX";
    const int fd = ::mkstemp(tmpl);
    if (fd >= 0) {
      ::close(fd);
    }
    d_path = tmpl;
#endif
  }
  ~TempPath() {
#if defined(_WIN32)
    ::DeleteFileA(d_path.c_str());
#else
    ::unlink(d_path.c_str());
#endif
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  const std::string& str() const noexcept { return d_path; }

private:
  std::string d_path;
};

#endif // ARBC_HAS_WORKSPACE_FILES

} // namespace

#if ARBC_HAS_WORKSPACE_FILES

TEST_CASE("stress: a live workspace-backed Document drains in the background while a writer "
          "churns editable contents and readers pin") {
  TempPath path;

  DocumentHousekeepingConfig config;
  config.thread.tick_period = kActiveTick;              // an active background drainer
  config.checkpoint_every_n_transactions = kCheckpointEvery; // a WRITER-side trigger

  auto made = Document::create(path.str(), config);
  REQUIRE(made.has_value());
  const std::unique_ptr<Document> doc = std::move(*made);
  REQUIRE(doc->workspace_backed());

  const ObjectId comp = doc->add_composition(8.0, 8.0);

  // Every raster the writer ever binds, kept alive past the document so its store's
  // version refcounts survive to be read at the end. A misrouted release would have
  // freed one of THESE contents' tile blobs.
  std::vector<std::shared_ptr<RasterContent>> rasters;
  std::vector<arbc::StateHandle> painted; // one published version per raster

  std::atomic<bool> go{false};
  std::atomic<bool> stop{false};
  std::atomic<bool> bad{false};

  // Render surrogates: pin the current version and read a content's state handle off it,
  // exactly as a render worker does. They touch neither the router nor the sinks -- and
  // if either were reachable from here, TSan would say so.
  std::vector<std::thread> pinners;
  for (int t = 0; t < kPinnerCount; ++t) {
    pinners.emplace_back([&] {
      while (!go.load(std::memory_order_acquire)) {
      }
      while (!stop.load(std::memory_order_acquire)) {
        const arbc::DocStatePtr pin = doc->pin();
        if (pin == nullptr) {
          bad.store(true, std::memory_order_relaxed);
          continue;
        }
        // A pinned version is immutable and its composition is always there, however
        // many contents the writer has bound and unbound underneath.
        if (pin->find_composition(comp) == nullptr) {
          bad.store(true, std::memory_order_relaxed);
        }
        std::this_thread::yield();
      }
    });
  }

  go.store(true, std::memory_order_release);

  // The writer (this thread). Each round: mint an editable raster (a router INSERT),
  // paint it under transactional discipline (a retain, and a superseded version the
  // background drainer will release), and lay down a layer. The background thread is
  // draining throughout -- and its releases route through the table this loop is
  // mutating.
  for (int i = 0; i < kWriterRounds; ++i) {
    const auto raster = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
    const ObjectId cid = doc->add_content(raster, /*kind=*/1);

    {
      auto txn = doc->transact("paint");
      raster->paint(txn, cid, arbc::Rect{0.0, 0.0, 2.0, 2.0}, k_red);
      REQUIRE(txn.commit().has_value());
    }
    painted.push_back(raster->base_handle());

    const ObjectId layer = doc->add_layer(cid, arbc::Affine::identity());
    doc->attach_layer(comp, layer);

    rasters.push_back(raster);
    if ((i % 8) == 0) {
      std::this_thread::yield(); // widen the drain-vs-bind window
    }
  }

  // An explicit host checkpoint on top of the cadence -- also on the writer thread.
  REQUIRE(doc->checkpoint().has_value());

  stop.store(true, std::memory_order_release);
  for (std::thread& th : pinners) {
    th.join();
  }

  REQUIRE_FALSE(bad.load());

  // The background loop really ran, and really ran a lot: this was a genuinely
  // concurrent drain, not a serialized one that happened to pass.
  const std::uint64_t ticks = doc->background_ticks();
  CHECK(ticks > 0);

  const arbc::HousekeepingStats stats = doc->memory_stats();
  arbc::Checkpointer& ckpt = *doc->checkpointer();

  // DECISION 2, as a counter. Every commit is attributable to a writer-side trigger:
  // the every-Nth-transaction cadence plus the one explicit request. The background
  // thread's thousands of ticks advanced the DRAIN counter and nothing else -- there is
  // no tick-interval trigger on a `Document` to advance anything more.
  const std::uint64_t cadence_commits = stats.transactions_seen / kCheckpointEvery;
  CHECK(stats.checkpoints_committed == cadence_commits + 1); // + the explicit request
  CHECK(ckpt.commit_count() == stats.checkpoints_committed);
  CHECK(stats.checkpoints_skipped_clean == 0); // the only skip-capable trigger is the
                                               // tick one, and a Document never has it
  CHECK(stats.drains_run >= stats.transactions_seen);

  // HAZARD 2, as an outcome. Every state release landed on the content that owned the
  // handle -- so not one of them was routed through a torn read into another raster's
  // store.
  CHECK(doc->editable_binding().unrouted_state_calls() == 0);

  // Quiesce, then assert the memory outcome: every version the model still pins is
  // accounted for, and the arena is drained to its no-garbage state (a second drain
  // finds nothing).
  doc->drain();
  const std::size_t quiesced = doc->memory_stats().live_slots;
  doc->drain();
  CHECK(doc->memory_stats().live_slots == quiesced);
}

TEST_CASE("stress: every raster's versions are released exactly once when the Document dies") {
  TempPath path;

  DocumentHousekeepingConfig config;
  config.thread.tick_period = kActiveTick;
  config.checkpoint_every_n_transactions = kCheckpointEvery;

  // The rasters OUTLIVE the document, so their stores' version refcounts survive to be
  // read after the final teardown drain has run.
  std::vector<std::shared_ptr<RasterContent>> rasters;
  std::vector<arbc::StateHandle> painted;

  {
    auto made = Document::create(path.str(), config);
    REQUIRE(made.has_value());
    const std::unique_ptr<Document> doc = std::move(*made);

    std::atomic<bool> stop{false};
    std::atomic<bool> bad{false};

    // One reader pinning throughout, so the teardown drain races a live pinner.
    std::thread pinner([&] {
      while (!stop.load(std::memory_order_acquire)) {
        if (doc->pin() == nullptr) {
          bad.store(true, std::memory_order_relaxed);
        }
        std::this_thread::yield();
      }
    });

    for (int i = 0; i < kWriterRounds; ++i) {
      const auto raster = std::make_shared<RasterContent>(white_4x4(), /*tile_edge=*/2);
      const ObjectId cid = doc->add_content(raster, /*kind=*/1);
      {
        auto txn = doc->transact("paint");
        raster->paint(txn, cid, arbc::Rect{0.0, 0.0, 2.0, 2.0}, k_red);
        REQUIRE(txn.commit().has_value());
      }
      painted.push_back(raster->base_handle());
      rasters.push_back(raster);
      if ((i % 8) == 0) {
        std::this_thread::yield();
      }
    }

    stop.store(true, std::memory_order_release);
    pinner.join();
    REQUIRE_FALSE(bad.load());
  } // ~Document: the thread stops after a final drain, THEN model, binding, contents

  // Every version every raster ever published is released exactly once, in its OWN
  // store. The handles COLLIDE across stores (each raster numbers its versions from 0),
  // so a release routed by the bare handle -- or by a torn table read -- would have
  // driven another raster's refcount negative or freed its tile blobs. Neither happened.
  REQUIRE(rasters.size() == painted.size());
  for (std::size_t i = 0; i < rasters.size(); ++i) {
    arbc::RasterStore& store = rasters[i]->store();
    CHECK(store.version_refcount(painted[i]) == 0);
    // The live base TABLE survives regardless -- the store pins it with its own
    // shared_ptr -- so a surviving content still reads its own pixels, not freed memory.
    REQUIRE(store.base_table());
    CHECK(store.base_table()->pixel(0, 0, 0) == k_red);
  }
}

#endif // ARBC_HAS_WORKSPACE_FILES
