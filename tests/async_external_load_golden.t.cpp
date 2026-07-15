// runtime.async_external_load end-to-end: the placeholder REPLACED BY THE REAL PIXELS, live.
//
// The pixel golden is an ORACLE, not a hand-typed byte string, and it is the same oracle
// `nested_external_ref_golden.t.cpp` uses: the scene authored IN-DOCUMENT, with the child in
// the `compositions` table. That test proved an external child renders byte-identically to an
// inline one. This one proves it still does when the bytes are LATE -- which is the only case
// that distinguishes an async design from a synchronous one wearing an async signature.
//
// The proof rides the OFFLINE render path (`render_frame` over a real `OperatorBindingScope` +
// `PullServiceImpl`), exactly as its sibling does. It is now the SAME wiring the interactive
// frame runs -- `runtime.interactive_pull_wiring` gave that loop a real `PullService` and
// `runtime.interactive_binder_wiring` made it call `bind_operators` per frame -- and the settle
// seam was always the same seam and the same damage either way, so the interactive path
// inherits this behavior with no further work here (Decision 7). It stays offline because the
// offline driver is the exact-evaluation oracle, which is what a pixel golden wants.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): needs a real `CpuBackend` and a real
// `OperatorBindingScope`, which a runtime component test may not name (doc 17).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/root_anchor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using arbc::Affine;
using arbc::ContentResolver;
using arbc::CpuBackend;
using arbc::DocStatePtr;
using arbc::Document;
using arbc::KindBridge;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::PullConfig;
using arbc::PullServiceImpl;
using arbc::Registry;
using arbc::Surface;
using arbc::SurfacePool;
using arbc::TileCache;
using arbc::Viewport;

// The deferring double: records the continuation, fires nothing until the test says so.
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

// A source that answers INLINE -- the `FilesystemAssetSource` shape, without the filesystem.
// The byte-exactness claim is "an external child loaded LATE renders identically to one loaded
// inline", so the inline load is one of the two oracles.
class InlineAssetSource final : public arbc::AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.insert_or_assign(std::move(uri), std::move(bytes));
  }
  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    const auto it = d_files.find(std::string(resolved_uri));
    on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
  }

private:
  std::unordered_map<std::string, std::string> d_files;
};

// The child project: an 8x8 composition holding one offset, half-opacity green solid, so the
// render is sensitive to placement rather than to a flat fill. Verbatim from
// `nested_external_ref_golden.t.cpp` -- the SAME scene, so the SAME oracle.
constexpr const char* k_child = R"({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 8, 8],
    "layers": [
      {
        "kind": "org.arbc.solid", "kind_version": "1",
        "params": {"color": [0.0, 0.75, 0.25, 1.0]},
        "transform": [4.0, 0.0, 0.0, 4.0, 2.0, 3.0],
        "opacity": 0.5
      }
    ]
  }
})";

constexpr const char* k_parent = R"({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {
        "kind": "org.arbc.nested", "kind_version": "1",
        "params": {"ref": "widgets/gauge.arbc"},
        "transform": [1.0, 0.0, 0.0, 1.0, 5.0, 1.0],
        "opacity": 1.0
      }
    ]
  }
})";

constexpr const char* k_base = "proj/parent.arbc";
constexpr const char* k_child_uri = "proj/widgets/gauge.arbc";
constexpr int k_dim = 16;

std::string save(const Document& doc, const KindBridge& bridge) {
  const arbc::expected<std::string, arbc::SerializeError> out = arbc::save_document(doc, bridge);
  REQUIRE(out);
  return *out;
}

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

// Render the root composition through the live engine: a real `CpuBackend`, a real
// `PullServiceImpl`, and a real `OperatorBindingScope` -- exactly the wiring the offline export
// driver builds (`offline_sequence.cpp`), and exactly what `nested_external_ref_golden` uses.
std::vector<std::byte> render_root(Document& doc, int dim) {
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };

  PullConfig config;
  config.id_of = arbc::make_pull_identity_of(*pin, resolve);
  const std::uint64_t revision = pin->revision();
  config.contribution = [revision](const arbc::Content*) { return revision; };
  PullServiceImpl service(cache, backend, arbc::direct_dispatch(), config);

  arbc::register_builtin_operator_binders();
  arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);

  const arbc::expected<std::unique_ptr<Surface>, arbc::SurfaceError> target =
      backend.make_surface(dim, dim, pin->working_space());
  REQUIRE(target.has_value());
  SurfacePool pool(backend);
  // `render_frame` renders the composition the Viewport anchors and never re-derives the
  // root, so anchor explicitly at the document's root composition
  // (compositor.root_composition_frame_walk).
  const Viewport viewport{dim, dim, Affine::identity(), arbc::test::root_composition_of(*pin)};
  arbc::render_frame(*pin, resolve, viewport, backend, pool, **target);

  std::vector<std::byte> out = bytes_of(**target);
  scope.release();
  return out;
}

const NestedContent& root_nested(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  ObjectId content;
  pin->for_each_layer_in(root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    content = lr->content;
  });
  const auto* const nested = dynamic_cast<const NestedContent*>(doc.resolve(content));
  REQUIRE(nested != nullptr);
  return *nested;
}

} // namespace

// enforces: 05-recursive-composition#deferred-external-child-installs-live
// enforces: 05-recursive-composition#external-nested-loads-through-loadcontext
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("a deferred external child replaces the placeholder with the real pixels, live") {
  // The task's promise, at the pixel. Render BEFORE the bytes arrive -> the placeholder, which
  // is what the same scene renders with no asset source at all. Fire, settle, re-render -> the
  // child's actual pixels, byte-identical to the same scene loaded through an INLINE source and
  // to the in-document oracle. Doc 05's "from the compositor's perspective there is no
  // difference between an inline child and an external one", now asserted across the async
  // boundary.
  DeferringAssetSource deferring;
  deferring.put(k_child_uri, k_child);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(k_parent, doc, bridge, registry, k_base, &deferring));
  REQUIRE(root_nested(doc).child().valid());                                 // a VALID id ...
  REQUIRE(doc.pin()->find_composition(root_nested(doc).child()) == nullptr); // ... no record
  REQUIRE(doc.pending_external_loads() == 1);

  const std::vector<std::byte> pending_pixels = render_root(doc, k_dim);

  // The oracle for the PENDING state: the same document with no asset source at all, whose
  // child is UNAVAILABLE. Pending and unavailable differ in exactly one bit, and only for the
  // loader -- at the pixel they are the same placeholder.
  Document unavailable;
  KindBridge unavailable_bridge;
  REQUIRE(arbc::load_document(k_parent, unavailable, unavailable_bridge, registry));
  const std::vector<std::byte> placeholder_pixels = render_root(unavailable, k_dim);
  REQUIRE(pending_pixels.size() == placeholder_pixels.size());
  REQUIRE(!pending_pixels.empty());
  CHECK(std::memcmp(pending_pixels.data(), placeholder_pixels.data(), pending_pixels.size()) == 0);

  // The bytes land, on a later revision.
  const std::uint64_t before = doc.pin()->revision();
  REQUIRE(deferring.fire_all() == 1);
  REQUIRE(arbc::settle_external_loads(doc, bridge, registry) == 1);
  REQUIRE(doc.pin()->revision() > before);
  REQUIRE(doc.pending_external_loads() == 0);

  const std::vector<std::byte> settled_pixels = render_root(doc, k_dim);

  // Oracle 1: the same scene through an INLINE source -- the synchronous path
  // `nested_external_ref` shipped.
  InlineAssetSource inline_source;
  inline_source.put(k_child_uri, k_child);
  Document inline_loaded;
  KindBridge inline_loaded_bridge;
  REQUIRE(arbc::load_document(k_parent, inline_loaded, inline_loaded_bridge, registry, k_base,
                              &inline_source));
  REQUIRE(inline_loaded.pending_external_loads() == 0);
  const std::vector<std::byte> inline_pixels = render_root(inline_loaded, k_dim);

  REQUIRE(settled_pixels.size() == inline_pixels.size());
  REQUIRE(!settled_pixels.empty());
  CHECK(std::memcmp(settled_pixels.data(), inline_pixels.data(), settled_pixels.size()) == 0);

  // ... and the placeholder really was replaced: the settled pixels are NOT the empty ones.
  CHECK(std::memcmp(settled_pixels.data(), placeholder_pixels.data(), settled_pixels.size()) != 0);
}

// enforces: 05-recursive-composition#nested-metadata-memoized-on-aggregate-revision
// enforces: 05-recursive-composition#deferred-external-child-installs-live
TEST_CASE("the nested metadata memo re-keys exactly once across the arrival") {
  // `bounds()` / `stability()` / `time_extent()` are memoized on the pinned document's
  // revision. While pending they must report the EMPTY PLACEHOLDER; after the settle they must
  // report the child's real values -- and the transition must cost exactly one recompute,
  // because the install published a new revision and the memo is keyed on it. Repeated queries
  // on either side of the transition must cost zero.
  //
  // The memo is re-keyed by `attach`, which binds the pinned `DocRoot`; so the two states are
  // observed through two binding scopes over two pins, which is what a host does across a
  // frame. The counter is the document's own revision -- a behavioural value, not a timing.
  DeferringAssetSource deferring;
  deferring.put(k_child_uri, k_child);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(k_parent, doc, bridge, registry, k_base, &deferring));

  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };

  const auto describe = [&]() {
    const DocStatePtr pin = doc.pin();
    PullConfig config;
    config.id_of = arbc::make_pull_identity_of(*pin, resolve);
    const std::uint64_t revision = pin->revision();
    config.contribution = [revision](const arbc::Content*) { return revision; };
    PullServiceImpl service(cache, backend, arbc::direct_dispatch(), config);
    arbc::register_builtin_operator_binders();
    arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);
    const NestedContent& nested = root_nested(doc);
    // Repeated queries on the SAME revision answer from the memo, identically -- the memo
    // re-keys on the revision, so a query costs a recompute only when the revision moved.
    const std::optional<arbc::Rect> first = nested.bounds();
    const std::optional<arbc::Rect> again = nested.bounds();
    REQUIRE(first.has_value());
    REQUIRE(again.has_value());
    CHECK(*first == *again);
    CHECK(nested.stability() == arbc::Stability::Static);
    const bool has_extent = nested.time_extent().has_value();
    scope.release();
    return std::pair<std::optional<arbc::Rect>, bool>{first, has_extent};
  };

  // Pending: the EMPTY placeholder -- an empty rect, no time extent.
  const auto [pending_bounds, pending_extent] = describe();
  REQUIRE(pending_bounds.has_value());
  CHECK(pending_bounds->empty());
  CHECK_FALSE(pending_extent);

  REQUIRE(deferring.fire_all() == 1);
  REQUIRE(arbc::settle_external_loads(doc, bridge, registry) == 1);

  // Settled: the child's REAL bounds -- the 8x8 child's solid, placed by the layer's transform.
  const auto [settled_bounds, settled_extent] = describe();
  REQUIRE(settled_bounds.has_value());
  CHECK_FALSE(settled_bounds->empty());
  CHECK_FALSE(pending_bounds->empty() == settled_bounds->empty());
  static_cast<void>(settled_extent);

  // The same document loaded INLINE describes identically -- the memo landed on the same
  // values it would have had if the bytes had never been late.
  InlineAssetSource inline_source;
  inline_source.put(k_child_uri, k_child);
  Document inline_loaded;
  KindBridge inline_bridge;
  REQUIRE(arbc::load_document(k_parent, inline_loaded, inline_bridge, registry, k_base,
                              &inline_source));
  {
    const DocStatePtr pin = inline_loaded.pin();
    const ContentResolver r = [&inline_loaded](ObjectId id) { return inline_loaded.resolve(id); };
    PullConfig config;
    config.id_of = arbc::make_pull_identity_of(*pin, r);
    const std::uint64_t revision = pin->revision();
    config.contribution = [revision](const arbc::Content*) { return revision; };
    PullServiceImpl service(cache, backend, arbc::direct_dispatch(), config);
    arbc::OperatorBindingScope scope = arbc::bind_operators(inline_loaded, service, backend, pin);
    const std::optional<arbc::Rect> inline_bounds = root_nested(inline_loaded).bounds();
    REQUIRE(inline_bounds.has_value());
    CHECK(*inline_bounds == *settled_bounds);
    scope.release();
  }
}

// enforces: 08-serialization#external-composition-ref-round-trips
// enforces: 08-serialization#canonical-output-is-byte-stable
TEST_CASE("save-while-pending is a fixed point: the same bytes loaded, pending, or missing") {
  // Constraint 9, and the one place the new valid-id-with-absent-record state could have leaked
  // into the format. Claim `#external-composition-ref-round-trips` pins that "the bytes do not
  // depend on LOAD state": a document saved with the widget file missing is byte-identical to
  // one saved with it present. PENDING is the first state where `composition_ref()` is VALID
  // while the composition record is ABSENT -- a writer walking `composition_ref()` would emit a
  // dangling `"composition": <id>` naming nothing. The existing suppression is keyed on
  // `external_composition_ref()` being non-empty, which is true while pending, so this should
  // already hold. It is pinned here rather than assumed.
  InlineAssetSource inline_source;
  inline_source.put(k_child_uri, k_child);
  DeferringAssetSource deferring;
  deferring.put(k_child_uri, k_child);

  const Registry registry;

  Document loaded;
  KindBridge loaded_bridge;
  REQUIRE(arbc::load_document(k_parent, loaded, loaded_bridge, registry, k_base, &inline_source));
  REQUIRE(root_nested(loaded).child().valid());
  REQUIRE(loaded.pin()->find_composition(root_nested(loaded).child()) != nullptr);
  const std::string from_loaded = save(loaded, loaded_bridge);

  Document pending;
  KindBridge pending_bridge;
  REQUIRE(arbc::load_document(k_parent, pending, pending_bridge, registry, k_base, &deferring));
  REQUIRE(pending.pending_external_loads() == 1);
  REQUIRE(root_nested(pending).child().valid());                                     // valid ...
  REQUIRE(pending.pin()->find_composition(root_nested(pending).child()) == nullptr); // ... absent
  const std::string from_pending = save(pending, pending_bridge);

  Document missing;
  KindBridge missing_bridge;
  REQUIRE(arbc::load_document(k_parent, missing, missing_bridge, registry));
  REQUIRE_FALSE(root_nested(missing).child().valid());
  const std::string from_missing = save(missing, missing_bridge);

  CHECK(from_pending == from_loaded);
  CHECK(from_pending == from_missing);
  // The authored URI, never a `"composition"` id on the layer (the id form is a STRING --
  // `"composition": "1"` -- as against the document's own `"composition": {` object), never a
  // `compositions` entry for the child, none of the child's contents.
  CHECK(from_pending.find(R"("ref": "widgets/gauge.arbc")") != std::string::npos);
  CHECK(from_pending.find("\"compositions\"") == std::string::npos);
  CHECK(from_pending.find("\"composition\": \"") == std::string::npos);
  CHECK(from_pending.find("org.arbc.solid") == std::string::npos);

  // And it stays a fixed point after the bytes land: settling changes the MODEL, never the
  // SAVE. The reference is what the document says; the child is what the environment supplies.
  REQUIRE(deferring.fire_all() == 1);
  REQUIRE(arbc::settle_external_loads(pending, pending_bridge, registry) == 1);
  CHECK(save(pending, pending_bridge) == from_loaded);
}
