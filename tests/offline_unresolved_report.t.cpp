// Issue #35: an offline render says what it could not resolve, so a deferring `AssetSource`
// can no longer make an export silently transparent.
//
// `render_offline` renders a pinned snapshot and settles nothing -- deliberately, because the
// byte-exact-reference role depends on rendering exactly the version it was handed. What was
// missing is the REPORT. A host exporting cameras to PNG through a network or content-store
// source got a hole in the picture with no error, no warning and no count; it now gets a
// number, and the frame, and decides for itself.
//
// Every arrival here is SCHEDULED by the test (the deferring source records the continuation
// and fires when told), so not one assertion depends on wall-clock timing (doc 16:54-62). Both
// external-reference kinds are driven -- an `org.arbc.image` asset and an `org.arbc.nested`
// project reference -- because the count is over what a content NAMES and does not have, and
// the two name their targets through different seams.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc` plus the image plugin's impl archive):
// it drives the full L5 load facade, a real `CpuBackend`, and the real offline drivers.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_image/image_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/offline.hpp>
#include <arbc/runtime/offline_sequence.hpp>
#include <arbc/runtime/unresolved_contents.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/surface/surface.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

using arbc::image::ImageContent;

// The DEFERRING `AssetSource` double: `request()` records `(uri, on_ready)` and fires nothing
// until the test says so. The same shape `async_external_load.t.cpp` and
// `image_async_pending.t.cpp` drive; duplicated rather than hoisted because each of the three
// asserts a different thing about it and a shared double would have to grow all three.
class DeferringAssetSource final : public AssetSource {
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
      // Absent == empty bytes, exactly as the `AssetSource` contract spells absence.
      r.on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
    }
    return firing.size();
  }

  std::size_t outstanding() const noexcept { return d_outstanding.size(); }

private:
  struct Request {
    std::string uri;
    std::function<void(std::string_view)> on_ready;
  };

  std::unordered_map<std::string, std::string> d_files;
  std::vector<Request> d_outstanding;
};

// A source that ANSWERS inline with empty bytes: UNAVAILABLE, not pending. It renders the same
// hole, and the report counts it the same -- which is the point of driving it here.
class AbsentAssetSource final : public AssetSource {
public:
  void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
    on_ready(std::string_view{});
  }
};

Registry image_registry() {
  Registry registry;
  REQUIRE(registry
              .add(
                  ImageContent::kind_id,
                  [](ContentConfig config) { return arbc::image::make_image_content(config); },
                  KindMetadata{"Image", "1"})
              .has_value());
  return registry;
}

// One `org.arbc.image` layer per authored reference.
std::string image_doc(const std::vector<std::string>& sources) {
  std::string layers;
  for (const std::string& source : sources) {
    if (!layers.empty()) {
      layers += ",";
    }
    layers +=
        R"({"kind":"org.arbc.image","kind_version":"1","params":{"source":")" + source + R"("}})";
  }
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,64,64],"layers":[)" + layers + "]}}";
}

// A document nesting `ref` through `org.arbc.nested`, beside one opaque solid so the frame is
// never empty and the render has something real to do.
std::string nesting_doc(std::string_view ref) {
  std::string layer = R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":")";
  layer += ref;
  layer += R"("}})";
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,64,64],"layers":[)"
         R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[0.0,1.0,0.0,1.0]}},)" +
         layer + "]}}";
}

// A leaf project: one solid, no external reference of its own.
constexpr const char* k_leaf =
    R"({"arbc":{"format":1},"composition":{"canvas":[0,0,32,32],"layers":[)"
    R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[1.0,0.0,0.0,1.0]}}]}})";

Viewport frame_viewport() { return Viewport{64, 64, Affine::identity()}; }

// Render one offline frame and hand back the report. The surface itself is required to exist:
// the whole posture under test is that an unresolved reference is REPORTED, not refused.
OfflineRenderReport rendered_report(const Document& doc, Backend& backend) {
  OfflineRenderReport report;
  const auto surface = render_offline(doc, frame_viewport(), backend, &report);
  REQUIRE(surface.has_value());
  return report;
}

OfflineRenderReport rendered_report(const Document& doc, const DocStatePtr& pin, Backend& backend) {
  OfflineRenderReport report;
  const auto surface = render_offline(doc, pin, frame_viewport(), backend, &report);
  REQUIRE(surface.has_value());
  return report;
}

} // namespace

// enforces: 02-architecture#offline-render-reports-unresolved-contents
TEST_CASE("an offline render reports an image whose bytes have not arrived") {
  DeferringAssetSource source;
  source.put("off0/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry, "off0/project.arbc",
                        &source)
              .has_value());
  // The load did not wait: the fetch is in flight and the layer is extent-less.
  REQUIRE(source.outstanding() == 1);
  REQUIRE(doc.pending_external_loads() == 1);

  CpuBackend backend;

  // The frame renders -- successfully, with the hole in it -- and SAYS there is a hole. That
  // pairing is the entire fix: before it, the caller got the surface and nothing else.
  CHECK(rendered_report(doc, backend).unresolved_contents == 1);

  // The bytes land and are installed, and the same render now reports a clean frame.
  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(rendered_report(doc, backend).unresolved_contents == 0);
}

// enforces: 02-architecture#offline-render-reports-unresolved-contents
TEST_CASE("the offline report counts blank regions, not references") {
  // PENDING and UNAVAILABLE are one thing from the exporter's side: both composite nothing.
  // A report that counted only the pending half would still let a missing file export silently.
  Document absent_doc;
  KindBridge absent_bridge;
  const Registry registry = image_registry();
  AbsentAssetSource absent;
  REQUIRE(load_document(image_doc({"assets/missing.ppm"}), absent_doc, absent_bridge, registry,
                        "off1/project.arbc", &absent)
              .has_value());
  CHECK(absent_doc.pending_external_loads() == 0); // it ANSWERED; nothing is in flight

  CpuBackend backend;
  CHECK(rendered_report(absent_doc, backend).unresolved_contents == 1);

  // Two layers spelling ONE file two ways share one fetch (doc 08 Principle 3) -- and are two
  // holes. The count is of contents, because it is of blank regions.
  DeferringAssetSource source;
  Document shared_doc;
  KindBridge shared_bridge;
  REQUIRE(load_document(image_doc({"assets/photo.ppm", "./assets/photo.ppm"}), shared_doc,
                        shared_bridge, registry, "off2/project.arbc", &source)
              .has_value());
  CHECK(source.outstanding() == 1);                // ONE fetch ...
  CHECK(shared_doc.pending_external_loads() == 1); // ... one reference in flight ...
  CHECK(rendered_report(shared_doc, backend).unresolved_contents == 2); // ... two holes
}

// enforces: 02-architecture#offline-render-reports-unresolved-contents
TEST_CASE("the offline report is answered against the rendered pin, not the document") {
  // The reason this is not `Document::pending_external_loads()`. A COMPOSITION arrival installs
  // its `CompositionRecord` on a NEW revision, so a pin taken before the settle still draws the
  // doc-05 placeholder after it -- while the document's own pending count has already fallen to
  // zero. Asking the document would under-report exactly the frame that has the hole, which is
  // the dangerous direction; asking the pin cannot.
  DeferringAssetSource source;
  source.put("off3/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  Registry registry;
  REQUIRE(
      load_document(nesting_doc("child.arbc"), doc, bridge, registry, "off3/project.arbc", &source)
          .has_value());
  REQUIRE(doc.pending_external_loads() == 1);

  CpuBackend backend;
  const DocStatePtr stale = doc.pin();
  CHECK(rendered_report(doc, stale, backend).unresolved_contents == 1);

  // The child lands on a later revision.
  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 0);

  // The document is clean; the OLD pin is not, and still renders the placeholder. The report
  // follows the pin.
  CHECK(rendered_report(doc, stale, backend).unresolved_contents == 1);
  CHECK(rendered_report(doc, backend).unresolved_contents == 0);
}

// enforces: 02-architecture#offline-render-reports-unresolved-contents
TEST_CASE("the offline report is opt-in and defined on every return path") {
  DeferringAssetSource source;
  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry, "off4/project.arbc",
                        &source)
              .has_value());

  CpuBackend backend;
  // A caller that does not want the count passes nothing and pays nothing -- the pre-issue
  // signature, still the default.
  CHECK(render_offline(doc, frame_viewport(), backend).has_value());

  // A null pin renders nothing and reports nothing -- the report is ZEROED rather than left
  // holding whatever the caller's struct carried in from a previous frame.
  OfflineRenderReport report;
  report.unresolved_contents = 99;
  CHECK_FALSE(render_offline(doc, DocStatePtr{}, frame_viewport(), backend, &report).has_value());
  CHECK(report.unresolved_contents == 0);
}

// enforces: 02-architecture#offline-render-reports-unresolved-contents
TEST_CASE("a sequence export reports its unresolved contents once, before it renders") {
  // The sequence twin. The pin is taken once for the whole export, so no frame can resolve what
  // another could not -- which is why this is one number asked before spending the minutes an
  // N-frame export costs, rather than a per-frame out-param.
  DeferringAssetSource source;
  source.put("off5/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry, "off5/project.arbc",
                        &source)
              .has_value());

  CpuBackend backend;
  {
    SequenceRenderer renderer(doc, frame_viewport(), backend);
    CHECK(renderer.unresolved_contents() == 1);
    CHECK(renderer.render_frame_at(Time::zero()).has_value()); // and it still renders
  }

  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  {
    SequenceRenderer settled(doc, frame_viewport(), backend);
    CHECK(settled.unresolved_contents() == 0);
  }
}

// enforces: 02-architecture#offline-render-reports-unresolved-contents
TEST_CASE("the unresolved walk is anchored, terminating, and blames nothing else") {
  // The walk's own edges, driven directly: a document with no external reference at all counts
  // zero however many layers it has, and an invalid anchor (a document with no composition)
  // reaches nothing rather than faulting.
  Document doc;
  KindBridge bridge;
  Registry registry;
  REQUIRE(load_document(k_leaf, doc, bridge, registry, "off6/project.arbc").has_value());

  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  ObjectId root;
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));

  CHECK(count_unresolved_contents(*pin, resolve, root) == 0);
  CHECK(count_unresolved_contents(*pin, resolve, ObjectId{}) == 0);
}
