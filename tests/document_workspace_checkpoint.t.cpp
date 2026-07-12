// The checkpoint half of `runtime.housekeeping_document_wiring`: a WORKSPACE-BACKED
// `Document` (docs 14/15). `model.workspace_backing` made a `Model` file-backed and
// gave it `checkpoint()`; nothing above it was file-backed, so no document ever
// committed one. This drives the cadence policy over `Model::checkpoint` end to end --
// the every-Nth-transaction writer trigger, the explicit host request, the
// still-document data-msync skip, and the `open()` round trip that resumes the last
// durable root.
//
// Cross-component (a runtime `Document` over a pool workspace file), so it lives here
// rather than in src/runtime/t/. The `TempPath` recipe is src/runtime/t/housekeeping.t.cpp's.

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/pool/checkpoint.hpp>
#include <arbc/pool/workspace_file.hpp>
#include <arbc/runtime/document.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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
using arbc::HousekeepingStats;
using arbc::ObjectId;

// A park period long enough that no automatic timeout tick fires: the background loop
// stays parked, so every drain and every commit below is attributable to the writer.
// It never appears in an assertion (doc 16:54-62). It also sharpens Decision 2 --
// a `Document` exposes no tick-interval checkpoint trigger at all, so even a BUSY loop
// could not be hiding a commit here; parking it merely removes the noise.
constexpr std::chrono::steady_clock::duration kNoTimeout = std::chrono::hours(1);

#if ARBC_HAS_WORKSPACE_FILES

// A temp workspace-file path, cleaned up on teardown (the housekeeping.t.cpp recipe):
// GetTempFileNameA/DeleteFileA on Windows, mkstemp/unlink on POSIX.
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    // GetTempFileNameA creates the file; create() reopens it with CREATE_ALWAYS.
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "dwc", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_dwc_XXXXXX";
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

DocumentHousekeepingConfig cadence(std::uint64_t every_n) {
  DocumentHousekeepingConfig config;
  config.thread.tick_period = kNoTimeout;
  config.checkpoint_every_n_transactions = every_n;
  return config;
}

std::unique_ptr<Document> create_at(const TempPath& path, std::uint64_t every_n) {
  auto made = Document::create(path.str(), cadence(every_n));
  REQUIRE(made.has_value());
  return std::move(*made);
}

#endif // ARBC_HAS_WORKSPACE_FILES

} // namespace

#if ARBC_HAS_WORKSPACE_FILES

// enforces: 15-memory-model#document-checkpoint-cadence
TEST_CASE("a workspace-backed Document commits on the every-Nth-transaction trigger") {
  TempPath path;
  const std::unique_ptr<Document> doc = create_at(path, /*every_n=*/3);

  REQUIRE(doc->workspace_backed());
  REQUIRE(doc->checkpointer() != nullptr);
  REQUIRE(doc->memory_stats().checkpoints_committed == 0);

  const ObjectId comp = doc->add_composition(4.0, 4.0); // transaction 1
  doc->add_layer(comp, arbc::Affine::identity());       // transaction 2
  CHECK(doc->memory_stats().checkpoints_committed == 0);

  doc->add_layer(comp, arbc::Affine::translation(1.0, 0.0)); // transaction 3 -> commit
  CHECK(doc->memory_stats().checkpoints_committed == 1);

  // The window RESETS on each commit, so the next two transactions do not fire.
  doc->add_layer(comp, arbc::Affine::translation(2.0, 0.0)); // 4
  doc->add_layer(comp, arbc::Affine::translation(3.0, 0.0)); // 5
  CHECK(doc->memory_stats().checkpoints_committed == 1);
  doc->add_layer(comp, arbc::Affine::translation(4.0, 0.0)); // 6 -> commit
  CHECK(doc->memory_stats().checkpoints_committed == 2);

  const HousekeepingStats stats = doc->memory_stats();
  CHECK(stats.transactions_seen == 6);
  CHECK(doc->checkpointer()->commit_count() == 2);
  CHECK(stats.durable_epoch > 0); // the file really carries a durable generation now
  CHECK_FALSE(doc->last_checkpoint_error().has_value());
}

// enforces: 15-memory-model#document-checkpoint-cadence
TEST_CASE("an explicit Document::checkpoint commits, and a still document skips the data msync") {
  TempPath path;
  // No automatic cadence: `Document::checkpoint()` is the ONLY trigger, so every commit
  // below is attributable to the explicit host call (doc 15:213-216's third trigger --
  // autosave / export / quit).
  const std::unique_ptr<Document> doc = create_at(path, /*every_n=*/0);
  arbc::Checkpointer& ckpt = *doc->checkpointer();

  const ObjectId comp = doc->add_composition(4.0, 4.0);
  doc->add_layer(comp, arbc::Affine::identity());
  CHECK(doc->memory_stats().checkpoints_committed == 0); // no cadence trigger exists

  REQUIRE(doc->checkpoint().has_value());
  CHECK(doc->memory_stats().checkpoints_committed == 1);
  CHECK(ckpt.commit_count() == 1);
  const std::uint64_t data_msyncs_after_first = ckpt.data_msyncs();
  CHECK(data_msyncs_after_first > 0); // a dirty scene really wrote its data through

  // THE STILL DOCUMENT. Nothing has been edited since, so a second request has nothing
  // new to make durable: it still commits (the host asked, and the header flip is
  // cheap), but it issues NO redundant data msync. This is what makes an idle-autosave
  // cadence free rather than a periodic disk storm.
  REQUIRE(doc->checkpoint().has_value());
  CHECK(ckpt.commit_count() == 2);
  CHECK(ckpt.data_msyncs() == data_msyncs_after_first);

  // ...and one more edit re-dirties it, so the next request does write data again --
  // the skip is a dirtiness decision, not a one-shot.
  doc->add_layer(comp, arbc::Affine::translation(1.0, 0.0));
  REQUIRE(doc->checkpoint().has_value());
  CHECK(ckpt.data_msyncs() > data_msyncs_after_first);
}

// enforces: 15-memory-model#document-checkpoint-cadence
TEST_CASE("a checkpointed Document reopens at its last durable root") {
  TempPath path;

  ObjectId comp{};
  std::vector<ObjectId> layers;
  std::uint32_t durable_epoch = 0;
  {
    const std::unique_ptr<Document> doc = create_at(path, /*every_n=*/0);
    comp = doc->add_composition(640.0, 480.0);
    for (int i = 0; i < 3; ++i) {
      const ObjectId layer =
          doc->add_layer(comp, arbc::Affine::translation(static_cast<double>(i), 0.0));
      doc->attach_layer(comp, layer);
      layers.push_back(layer);
    }
    REQUIRE(doc->checkpoint().has_value());
    durable_epoch = doc->memory_stats().durable_epoch;
    REQUIRE(durable_epoch > 0);

    // An edit AFTER the checkpoint is deliberately left un-checkpointed: the reopen
    // below must land on the durable root, not on this. "An editor crash costs
    // at-most-since-last-checkpoint, not the document" (doc 15:179-187).
    const ObjectId lost = doc->add_layer(comp, arbc::Affine::translation(99.0, 0.0));
    doc->attach_layer(comp, lost);
  } // ~Document: the housekeeping thread joins, the model drains, the file closes

  auto reopened = Document::open(path.str(), cadence(0));
  REQUIRE(reopened.has_value());
  const std::unique_ptr<Document> doc = std::move(*reopened);

  REQUIRE(doc->workspace_backed());
  const arbc::DocStatePtr state = doc->pin();
  REQUIRE(state != nullptr);

  // The durable root IS the document: the composition and its three checkpointed
  // layers are back, with their identities intact.
  const arbc::CompositionRecord* const recovered = state->find_composition(comp);
  REQUIRE(recovered != nullptr);
  for (const ObjectId layer : layers) {
    CHECK(state->find_layer(layer) != nullptr);
  }

  // The post-checkpoint fourth layer is NOT: it was never made durable, so the
  // composition comes back with exactly the three members the durable root held.
  std::size_t recovered_layers = 0;
  state->for_each_layer_in(comp, [&](ObjectId) { ++recovered_layers; });
  CHECK(recovered_layers == layers.size());

  // The recovered document is a live, checkpointable document, not a read-only husk.
  CHECK(doc->memory_stats().durable_epoch >= durable_epoch);
  doc->add_layer(comp, arbc::Affine::identity());
  REQUIRE(doc->checkpoint().has_value());
  CHECK(doc->checkpointer()->commit_count() == 1); // this document's first commit
}

#endif // ARBC_HAS_WORKSPACE_FILES
