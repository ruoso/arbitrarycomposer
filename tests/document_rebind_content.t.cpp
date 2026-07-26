// The content-repair seam of `runtime.workspace_content_reconstruction` (issue #19):
// `Document::rebind_content` and `Document::recovered_content_state`.
//
// `Document::open` restores the record graph and binds no `Content`, so a reopened
// workspace resolves null for every record. `add_content` cannot repair that -- it
// mints a NEW record with a new id, while the recovered records already carry the ids
// the layers, the journal and the persisted `StateHandle`s all name. `rebind_content`
// binds an object to an id that already exists.
//
// The issue reported this shape as impossible, citing `slot_store.hpp:119` ("it never
// rebinds and there is no rebind API"). That sentence governs the writer-thread
// IDENTITY a `SlotStore` binds on first write, under a "Threading" heading; content
// binding is a separate, copy-on-write `shared_ptr` map (`document.cpp`), and
// replacing a row in it is the same operation as adding one.
//
// Cross-component (a runtime `Document` over a pool workspace file plus a real kind),
// so it lives here rather than in src/runtime/t/. The `TempPath` recipe is
// tests/document_workspace_checkpoint.t.cpp's.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/offline.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
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
using arbc::ObjectId;
using arbc::Rect;
using arbc::Rgba;
using arbc::SolidContent;

// A flat 4x4 image in the working format, the shape `RasterContent` decodes from
// (the recipe is tests/raster_runtime_binding.t.cpp's).
arbc::DecodedImage flat_4x4(float value) {
  arbc::DecodedImage img;
  img.width = 4;
  img.height = 4;
  img.format = arbc::k_working_rgba32f;
  const std::vector<float> f(64, value);
  const auto* src = reinterpret_cast<const std::byte*>(f.data());
  img.bytes.assign(src, src + f.size() * sizeof(float));
  return img;
}

std::shared_ptr<SolidContent> red_solid() {
  return std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0});
}

} // namespace

// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("rebind_content binds an object to a record that already exists") {
  Document doc;
  const ObjectId cid = doc.add_content(red_solid(), /*kind=*/1);
  REQUIRE(doc.resolve(cid) != nullptr);

  // Unbind: the row is dropped and the RECORD stays. This is the state a workspace
  // reopen lands in -- records present, nothing bound -- reached here without a file.
  REQUIRE(doc.rebind_content(cid, nullptr));
  CHECK(doc.resolve(cid) == nullptr);
  CHECK(doc.pin()->find_content(cid) != nullptr);

  // Repair: a fresh object bound to the SAME id. The record is untouched -- same id,
  // same kind -- so every layer naming it resolves again.
  const std::shared_ptr<SolidContent> replacement = red_solid();
  REQUIRE(doc.rebind_content(cid, replacement));
  CHECK(doc.resolve(cid) == replacement.get());
  CHECK(doc.pin()->find_content(cid)->kind == 1U);
}

// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("rebind_content replaces a live row, and publishes no version") {
  Document doc;
  const ObjectId cid = doc.add_content(red_solid(), /*kind=*/1);
  const std::uint64_t revision_before = doc.pin()->revision();
  const std::size_t entries_before = doc.journal().depth();

  const std::shared_ptr<SolidContent> replacement = red_solid();
  REQUIRE(doc.rebind_content(cid, replacement));
  CHECK(doc.resolve(cid) == replacement.get());

  // Nothing VERSIONED happened: the id->Content side-map is runtime state beside the
  // model, not in it (doc 17:66-72), so a rebind mints no version and appends no
  // journal entry. A host repairing a hundred recovered records does not thereby
  // create a hundred undo steps.
  CHECK(doc.pin()->revision() == revision_before);
  CHECK(doc.journal().depth() == entries_before);
}

// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("rebind_content refuses an id that names no content record") {
  Document doc;
  const ObjectId comp = doc.add_composition(8.0, 8.0);
  const ObjectId layer = doc.add_layer(comp, arbc::Affine::identity());

  // A never-minted id, a composition id, and a layer id are all "not a content
  // record". Each is refused as a VALUE (doc 10), because binding an object to an id
  // the document does not have leaves a row nothing can reach and nothing can reclaim.
  CHECK_FALSE(doc.rebind_content(ObjectId{9999}, red_solid()));
  CHECK_FALSE(doc.rebind_content(comp, red_solid()));
  CHECK_FALSE(doc.rebind_content(layer, red_solid()));
  CHECK(doc.resolve(ObjectId{9999}) == nullptr);
}

// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("rebinding an editable content re-registers its state sinks") {
  using arbc::RasterContent;
  using arbc::RasterStore;
  using arbc::StateHandle;

  const auto first = std::make_shared<RasterContent>(flat_4x4(1.0F), /*tile_edge=*/2);
  Document doc;
  const ObjectId cid = doc.add_content(first, /*kind=*/1);
  REQUIRE(doc.pin()->content_state(cid) == first->base_handle());

  // Swap in a different raster under the same id -- the shape a host takes when a
  // reconstruction arrives after the fact.
  const auto second = std::make_shared<RasterContent>(flat_4x4(0.5F), /*tile_edge=*/2);
  const StateHandle second_base = second->base_handle();
  REQUIRE(doc.rebind_content(cid, second));
  REQUIRE(doc.resolve(cid) == second.get());

  // The REBOUND object's edits journal and undo against this record. That is what
  // re-registering the `Editable` routing buys: without it the edit would find no row
  // for `cid`, its state would route nowhere, and the undo below would restore nothing.
  RasterStore& store = second->store();
  {
    auto txn = doc.transact("paint");
    second->paint(txn, cid, Rect{0.0, 0.0, 2.0, 2.0}, arbc::WorkingPixel{1.0F, 0.0F, 0.0F, 1.0F});
    REQUIRE(txn.commit().has_value());
  }
  doc.drain();
  const StateHandle painted = second->base_handle();
  REQUIRE(painted != second_base);
  CHECK(doc.pin()->content_state(cid) == painted);
  CHECK(store.version_refcount(painted) == 1);

  REQUIRE(doc.journal().undo());
  CHECK(second->base_handle() == second_base);
  CHECK(doc.pin()->content_state(cid) == second_base);
}

// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("recovered_content_state is reachable from Document") {
  // The issue's secondary ask: `Model::recovered_content_state()` exists and
  // `replay_recovered_content_state()` consumes it, but `Document` published no
  // accessor for its `Model`, so the whole issue-#5 recovery trio was unreachable from
  // the public host surface. It is now reachable -- and EMPTY here, which is the
  // documented answer for an anonymous document and, today, for every kind: nothing
  // persists workspace-backed state until `kinds.raster_workspace_backing` lands.
  Document doc;
  doc.add_content(red_solid(), /*kind=*/1);
  CHECK(doc.recovered_content_state().empty());
}

#if ARBC_HAS_WORKSPACE_FILES

namespace {

// The `TempPath` recipe is src/runtime/t/housekeeping.t.cpp's, by way of
// tests/document_workspace_checkpoint.t.cpp.
class TempPath {
public:
  TempPath() {
#if defined(_WIN32)
    char dir[MAX_PATH];
    const DWORD n = ::GetTempPathA(MAX_PATH, dir);
    char buf[MAX_PATH];
    if (n != 0 && n < static_cast<DWORD>(MAX_PATH) && ::GetTempFileNameA(dir, "drc", 0, buf) != 0) {
      d_path = buf;
    }
#else
    char tmpl[] = "/tmp/arbc_drc_XXXXXX";
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

arbc::DocumentHousekeepingConfig no_cadence() {
  arbc::DocumentHousekeepingConfig config;
  // A park period long enough that no automatic tick fires, but FINITE: the park
  // computes `now() + tick_period`, so `duration::max()` overflows it and the
  // background loop never wakes to be joined. The recipe is
  // tests/document_workspace_checkpoint.t.cpp's.
  config.thread.tick_period = std::chrono::hours(1);
  config.checkpoint_every_n_transactions = 0;
  return config;
}

} // namespace

// enforces: 14-data-model-and-editing#editable-runtime-bound
TEST_CASE("a reopened workspace's records resolve again once the host rebinds them") {
  // The issue's actual scenario, end to end. A workspace-backed document holding a
  // content and a layer that names it is checkpointed and closed; the reopen restores
  // the record graph and binds NOTHING, which is the reported defect; the host repairs
  // it with `rebind_content` and the layer resolves again.
  TempPath path;
  ObjectId comp{};
  ObjectId cid{};
  ObjectId layer{};
  {
    auto made = Document::create(path.str(), no_cadence());
    REQUIRE(made.has_value());
    const std::unique_ptr<Document>& doc = *made;
    comp = doc->add_composition(8.0, 8.0);
    cid = doc->add_content(red_solid(), /*kind=*/1);
    layer = doc->add_layer(cid, arbc::Affine::identity());
    doc->attach_layer(comp, layer);
    REQUIRE(doc->checkpoint().has_value());
  }

  auto reopened = Document::open(path.str(), no_cadence());
  REQUIRE(reopened.has_value());
  const std::unique_ptr<Document> doc = std::move(*reopened);

  // What survives: the composition, the layer, and the layer's content EDGE. What
  // does not: the `Content` object -- `resolve` is null and `for_each_content` visits
  // nothing, exactly the table in the issue.
  const arbc::DocStatePtr state = doc->pin();
  REQUIRE(state->find_composition(comp) != nullptr);
  REQUIRE(state->find_layer(layer) != nullptr);
  CHECK(state->find_layer(layer)->content == cid);
  REQUIRE(state->find_content(cid) != nullptr);
  CHECK(doc->resolve(cid) == nullptr);
  std::size_t visited = 0;
  doc->for_each_content([&](arbc::Content*) { ++visited; });
  CHECK(visited == 0);

  // The repair, on the id the layer already names -- which `add_content` could not do,
  // because it mints a new record with a new id nothing points at.
  const std::shared_ptr<SolidContent> rebuilt = red_solid();
  REQUIRE(doc->rebind_content(cid, rebuilt));
  CHECK(doc->resolve(cid) == rebuilt.get());
  CHECK(doc->resolve(state->find_layer(layer)->content) == rebuilt.get());
  visited = 0;
  doc->for_each_content([&](arbc::Content*) { ++visited; });
  CHECK(visited == 1);
}

#endif // ARBC_HAS_WORKSPACE_FILES

// enforces: 02-architecture#offline-frame-renders-exactly-no-degrade
TEST_CASE("a caller-pinned batch of offline frames is coherent across one document state") {
  // Issue #27: `render_offline` pinned per call, so an N-frame batch straddled any edit
  // that landed mid-batch -- item 3 reflecting a document state item 1 did not, with no
  // way for the host to ask for the same version twice. The host's only alternatives
  // were blocking the writer for the whole batch (a frozen UI for the minutes a dozen
  // 4K frames take) or a full serialize-and-reload snapshot per export.
  arbc::CpuBackend backend;
  Document doc;
  const ObjectId comp = doc.add_composition(8.0, 8.0);
  const ObjectId cid = doc.add_content(
      std::make_shared<SolidContent>(Rgba{1.0F, 0.0F, 0.0F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0}), 1);
  doc.attach_layer(comp, doc.add_layer(cid, arbc::Affine::identity()));

  // The batch pins ONCE, up front -- the whole of the fix.
  const arbc::DocStatePtr pinned = doc.pin();
  const arbc::Viewport viewport{8, 8, arbc::Affine::identity(), comp};

  const auto first = arbc::render_offline(doc, pinned, viewport, backend);
  REQUIRE(first.has_value());

  // An edit lands MID-BATCH, exactly as a user editing while an export runs would.
  doc.attach_layer(
      comp,
      doc.add_layer(doc.add_content(std::make_shared<SolidContent>(Rgba{0.0F, 1.0F, 0.0F, 1.0F},
                                                                   Rect{0.0, 0.0, 8.0, 8.0}),
                                    1),
                    arbc::Affine::identity()));

  // The rest of the batch still renders the version the batch began with: byte-identical
  // to the first frame, because they are frames of ONE document state.
  const auto second = arbc::render_offline(doc, pinned, viewport, backend);
  REQUIRE(second.has_value());
  const std::span<const std::byte> a = (*first)->cpu_bytes();
  const std::span<const std::byte> b = (*second)->cpu_bytes();
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size()) == 0);

  // And the un-pinned overload sees the edit -- proving the pin is what held the batch
  // still, not that the edit failed to land.
  const auto current = arbc::render_offline(doc, viewport, backend);
  REQUIRE(current.has_value());
  const std::span<const std::byte> c = (*current)->cpu_bytes();
  CHECK(std::memcmp(a.data(), c.data(), a.size()) != 0);

  // A null pin is an error value, never a crash (doc 10).
  CHECK_FALSE(arbc::render_offline(doc, arbc::DocStatePtr{}, viewport, backend).has_value());
}
