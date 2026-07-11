// runtime.nested_external_ref end-to-end: M8's promise, "external nested projects load",
// proved the only way it can honestly be proved -- a REAL parent `.arbc` on disk beside a
// REAL child `.arbc`, loaded through the REAL `FilesystemAssetSource`, rendering the child's
// actual pixels.
//
// The pixel golden is an ORACLE, not a hand-typed byte string: the external scene must render
// byte-identically to the same scene authored IN-DOCUMENT. That is doc 05:47-52 stated as a
// test -- "from the compositor's perspective there is no difference between an inline child
// and an external one" -- and it is the strongest form the claim can take, because it pins
// the external path to the in-document path rather than to a constant that could drift.
//
// The save-side half is the data-loss hazard this task exists to close (doc 08 Principle 7's
// third corollary): the moment an external child RENDERS, its `composition_ref()` starts
// resolving, and a writer that did not know better would walk the other document's contents
// into this one's `contents` table and emit `"composition": "1"` in place of the URI. Open,
// save, and the reference is gone -- replaced by a frozen copy. So the golden asserts the
// negative space too: no `compositions` key, no `contents` key, the child's contents absent,
// and the `ref` intact.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): the render and attach-invariance
// proofs need a real `CpuBackend` and a real `OperatorBindingScope`, which a runtime component
// test may not name (doc 17 / check_levels.py). The deterministic loader behaviours -- dedup,
// cycles, the depth cap -- live in tests/nested_external_ref.t.cpp over an in-memory source.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
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
#include <arbc/runtime/filesystem_asset_source.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace {

using arbc::Affine;
using arbc::ContentResolver;
using arbc::CpuBackend;
using arbc::DocStatePtr;
using arbc::Document;
using arbc::FilesystemAssetSource;
using arbc::KindBridge;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::PullConfig;
using arbc::PullServiceImpl;
using arbc::Registry;
using arbc::SerializeError;
using arbc::Surface;
using arbc::SurfacePool;
using arbc::TileCache;
using arbc::Viewport;

// The child project: an 8x8 composition holding one green solid, offset so the render is
// sensitive to placement rather than to a flat fill.
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

// The parent project, naming the child by a RELATIVE `params.ref` -- the doc 08:38-42
// spelling, verbatim from the format's own document-shape example.
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

// The SAME scene, authored in-document: the child rides the `compositions` table by a
// core-owned id instead of a URI. Every pixel must come out identical.
constexpr const char* k_inline = R"({
  "arbc": {"format": 1},
  "composition": {
    "canvas": [0, 0, 16, 16],
    "layers": [
      {
        "kind": "org.arbc.nested", "kind_version": "1",
        "composition": "1",
        "params": {},
        "transform": [1.0, 0.0, 0.0, 1.0, 5.0, 1.0],
        "opacity": 1.0
      }
    ]
  },
  "compositions": {
    "1": {
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
  }
})";

// A project directory under the OS temp dir, removed on scope exit. Named after the test so
// two lanes of the ctest matrix never collide.
class ProjectDir {
public:
  explicit ProjectDir(const std::string& name)
      : d_root(std::filesystem::temp_directory_path() / ("arbc_extref_" + name)) {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
    REQUIRE(std::filesystem::create_directories(d_root / "widgets", ec));
  }
  ~ProjectDir() {
    std::error_code ec;
    std::filesystem::remove_all(d_root, ec);
  }
  ProjectDir(const ProjectDir&) = delete;
  ProjectDir& operator=(const ProjectDir&) = delete;

  // Write `bytes` at `relative` and return the path, as a string, for use as a base URI.
  std::string write(const std::string& relative, std::string_view bytes) const {
    const std::filesystem::path path = d_root / relative;
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    REQUIRE(out);
    return path.string();
  }

private:
  std::filesystem::path d_root;
};

std::string save(const Document& doc, const KindBridge& bridge) {
  const arbc::expected<std::string, SerializeError> out = arbc::save_document(doc, bridge);
  REQUIRE(out);
  return *out;
}

std::vector<std::byte> bytes_of(const Surface& s) {
  const std::span<const std::byte> b = s.cpu_bytes();
  return {b.begin(), b.end()};
}

// Render `doc`'s root composition through the live engine: a real `CpuBackend`, a real
// `PullServiceImpl`, and a real `OperatorBindingScope` -- which is what attaches the nested
// content and hands it the pinned `DocRoot` it reads the child's membership from. Exactly the
// wiring the offline export driver builds (`offline_sequence.cpp`).
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
  const Viewport viewport{dim, dim, Affine::identity()};
  arbc::render_frame(*pin, resolve, viewport, backend, pool, **target);

  std::vector<std::byte> out = bytes_of(**target);
  scope.release();
  return out;
}

// Load a document from disk through the real filesystem source.
void load_project(Document& doc, KindBridge& bridge, const std::string& path,
                  FilesystemAssetSource& source) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const Registry registry;
  REQUIRE(arbc::load_document(bytes, doc, bridge, registry, path, &source));
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

// enforces: 05-recursive-composition#unresolvable-external-ref-renders-placeholder
TEST_CASE("FilesystemAssetSource reports every failure as absence, never as a throw") {
  // The source is the one place a hostile document reaches the filesystem, so every way that
  // can go wrong must come back as EMPTY BYTES -- which the loader turns into the doc-05
  // placeholder -- rather than as an exception or an error channel of its own
  // (08-serialization#loader-never-faults-on-hostile-input).
  const ProjectDir project("source");
  const std::string real = project.write("widgets/gauge.arbc", k_child);

  FilesystemAssetSource source;
  std::string got;
  const auto fetch = [&](const std::string& uri) {
    got.clear();
    REQUIRE_NOTHROW(
        source.request(uri, [&](std::string_view b) { got.assign(b.begin(), b.end()); }));
    return got;
  };

  CHECK(fetch(real) == std::string(k_child));
  CHECK(fetch("file://" + real) == std::string(k_child)); // the one scheme it can honestly claim
  CHECK(fetch(real + ".nope").empty());                   // absent
  CHECK(fetch(std::filesystem::path(real).parent_path().string()).empty()); // a directory
  CHECK(fetch("https://cdn.example/gauge.arbc").empty()); // a scheme it cannot serve
  CHECK(fetch("").empty());

  CHECK(source.requests() == 6);
  CHECK(source.hits() == 2); // only the two spellings of the real file
}

// enforces: 05-recursive-composition#external-nested-loads-through-loadcontext
// enforces: 16-sdlc-and-quality#byte-exact-goldens
TEST_CASE("an external nested project loads through LoadContext and renders the child's pixels") {
  // The milestone promise, end to end. `widgets/gauge.arbc` is a real file the parent names by
  // a relative URI; `FilesystemAssetSource` fetches it; the loader installs its composition
  // graph into the PARENT's own model; and the parent's `NestedContent` composes it exactly as
  // it would compose an in-document child -- which the in-document oracle below proves by
  // rendering to the same bytes.
  const ProjectDir project("render");
  project.write("widgets/gauge.arbc", k_child);
  const std::string parent_path = project.write("parent.arbc", k_parent);

  FilesystemAssetSource source;
  Document external;
  KindBridge external_bridge;
  load_project(external, external_bridge, parent_path, source);

  // The reference resolved: a real child composition, in this document's model, named by the
  // authored (never the absolutised) URI.
  const NestedContent& nested = root_nested(external);
  REQUIRE(nested.child().valid());
  CHECK(nested.ref() == "widgets/gauge.arbc");
  CHECK(external.pin()->find_composition(nested.child()) != nullptr);
  CHECK(source.requests() == 1);
  CHECK(source.hits() == 1);

  // The in-document oracle: the same scene, the child in the `compositions` table.
  Document inline_doc;
  KindBridge inline_bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(k_inline, inline_doc, inline_bridge, registry));

  constexpr int k_dim = 16;
  const std::vector<std::byte> from_external = render_root(external, k_dim);
  const std::vector<std::byte> from_inline = render_root(inline_doc, k_dim);
  REQUIRE(from_external.size() == from_inline.size());
  REQUIRE(!from_external.empty());
  // Byte-exact: doc 05:47-52's "no difference between an inline child and an external one",
  // asserted at the pixel.
  CHECK(std::memcmp(from_external.data(), from_inline.data(), from_external.size()) == 0);
}

// enforces: 08-serialization#external-composition-ref-round-trips
// enforces: 08-serialization#canonical-output-is-byte-stable
TEST_CASE("a loaded external reference saves back as a reference, never inlined") {
  // The data-loss hazard, closed. Before this task a rendering external child would have had
  // its `composition_ref()` resolve, and the writer would have hoisted the OTHER document's
  // contents into this one's `contents` table and emitted `"composition": "1"` in place of the
  // URI. Open, save, and the widget is a frozen copy.
  const ProjectDir project("roundtrip");
  project.write("widgets/gauge.arbc", k_child);
  const std::string parent_path = project.write("parent.arbc", k_parent);

  FilesystemAssetSource source;
  Document doc;
  KindBridge bridge;
  load_project(doc, bridge, parent_path, source);
  REQUIRE(root_nested(doc).child().valid()); // the child really did load

  const std::string saved = save(doc, bridge);
  CHECK(saved.find(R"("ref": "widgets/gauge.arbc")") != std::string::npos);
  CHECK(saved.find("\"compositions\"") == std::string::npos); // not hoisted into the table ...
  CHECK(saved.find("\"contents\"") == std::string::npos);     // ... nor into shared contents
  CHECK(saved.find("org.arbc.solid") == std::string::npos);   // the child's contents: absent
  CHECK(saved.find("\"inputs\"") == std::string::npos);       // nor its projected input edges

  // Byte-stable across a reload -- with the widget file STILL THERE, so the child loads again
  // and the writer must suppress it again.
  Document reloaded;
  KindBridge reloaded_bridge;
  FilesystemAssetSource reload_source;
  const Registry registry;
  REQUIRE(
      arbc::load_document(saved, reloaded, reloaded_bridge, registry, parent_path, &reload_source));
  CHECK(root_nested(reloaded).child().valid());
  CHECK(save(reloaded, reloaded_bridge) == saved);
}

// enforces: 08-serialization#nesting-inputs-are-derived-not-persisted
// enforces: 08-serialization#external-composition-ref-round-trips
TEST_CASE("an external nesting save is invariant under a live binding AND under load state") {
  // Constraint 9, both halves. A save must not depend on whether a render binding happened to
  // be attached when it was taken (`NestedContent::inputs()` is memo-derived from the child's
  // layers and is non-empty exactly when `bind_operators` has attached it -- the state every
  // rendered frame leaves it in) ... and it must not depend on whether the external child's
  // bytes were successfully LOADED either. A document saved with the widget file missing is
  // byte-identical to the same document saved with it present.
  const ProjectDir project("invariance");
  project.write("widgets/gauge.arbc", k_child);
  const std::string parent_path = project.write("parent.arbc", k_parent);

  FilesystemAssetSource source;
  Document doc;
  KindBridge bridge;
  load_project(doc, bridge, parent_path, source);

  const std::string unbound = save(doc, bridge);

  std::string bound;
  {
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

    const NestedContent& nested = root_nested(doc);
    REQUIRE(nested.attached());
    REQUIRE_FALSE(nested.inputs().empty()); // the derived edges the writer must NOT persist
    bound = save(doc, bridge);
    scope.release();
  }
  CHECK(bound == unbound);

  // The load-state half: the SAME bytes, opened with no asset source at all, so the child
  // never loads and the nested content holds a null child -- and the save is the same bytes.
  Document unloaded;
  KindBridge unloaded_bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(k_parent, unloaded, unloaded_bridge, registry));
  CHECK_FALSE(root_nested(unloaded).child().valid());
  CHECK(save(unloaded, unloaded_bridge) == unbound);
}
