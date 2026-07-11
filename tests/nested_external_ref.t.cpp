// runtime.nested_external_ref: the loader's behavioural proofs, driven by an IN-MEMORY
// `AssetSource` double holding a URI->bytes map. No temp files, no filesystem, fully
// deterministic -- and the source counts its `request()` calls, which is what turns "dedup"
// and "the cycle terminates" from timing claims into behavioural counters (doc 16:54-62).
// The end-to-end proof over a REAL `FilesystemAssetSource` and real files, including the
// pixel golden, is `tests/nested_external_ref_golden.t.cpp`.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): these drive the full L5 load façade
// and, for the cycle case, a real `CpuBackend` + `OperatorBindingScope`, which a runtime
// component test may not name (doc 17 / check_levels.py). The codec's own units live in
// src/runtime/t/nested_codec.t.cpp, and `load_composition`'s in src/serialize/t/reader.t.cpp.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/external_composition_loader.hpp> // k_external_ref_depth_cap
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/serialize/load_context.hpp> // AssetSource

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using arbc::ContentResolver;
using arbc::CpuBackend;
using arbc::DocRoot;
using arbc::DocStatePtr;
using arbc::Document;
using arbc::KindBridge;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::PullConfig;
using arbc::PullServiceImpl;
using arbc::Registry;
using arbc::TileCache;

// The in-memory `AssetSource` double: a URI->bytes map plus a request counter. Fires
// `on_ready` inline, exactly as `FilesystemAssetSource` does; an absent URI yields empty
// bytes, which is how the contract already spells absence.
class MemoryAssetSource final : public arbc::AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.emplace(std::move(uri), std::move(bytes));
  }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    ++d_requests;
    const auto it = d_files.find(std::string(resolved_uri));
    on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
  }

  // The dedup witness (doc 08 Principle 3): two references resolving to one URI must cost
  // exactly ONE fetch.
  std::size_t requests() const noexcept { return d_requests; }

private:
  std::unordered_map<std::string, std::string> d_files;
  std::size_t d_requests{0};
};

// A one-layer document embedding `ref` through org.arbc.nested.
std::string nesting_doc(std::string_view ref) {
  std::string layer = R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":")";
  layer += ref;
  layer += R"("}})";
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)" + layer + "]}}";
}

// A leaf document: one solid, no nesting.
constexpr const char* k_leaf =
    R"({"arbc":{"format":1},"composition":{"canvas":[0,0,8,8],"layers":[)"
    R"({"kind":"org.arbc.solid","kind_version":"1","params":{"color":[0.0,1.0,0.0,1.0]}}]}})";

// Every layer-root content of the root composition, in membership order.
std::vector<const arbc::Content*> root_contents(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  std::vector<const arbc::Content*> out;
  pin->for_each_layer_in(root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    out.push_back(doc.resolve(lr->content));
  });
  return out;
}

const NestedContent& nested_at(const Document& doc, std::size_t index) {
  const std::vector<const arbc::Content*> contents = root_contents(doc);
  REQUIRE(index < contents.size());
  const auto* const nested = dynamic_cast<const NestedContent*>(contents[index]);
  REQUIRE(nested != nullptr);
  return *nested;
}

std::string save(const Document& doc, const KindBridge& bridge) {
  const arbc::expected<std::string, arbc::SerializeError> out = arbc::save_document(doc, bridge);
  REQUIRE(out);
  return *out;
}

} // namespace

// enforces: 08-serialization#loadcontext-dedups-by-resolved-identity
// enforces: 05-recursive-composition#external-nested-loads-through-loadcontext
TEST_CASE("two references to one external child load it ONCE and share one composition") {
  // Doc 05:41-45: content is shared, placement is per-instance, and the child's tile caches
  // are keyed by the child's content identities so all embeddings share them. Two parents
  // loading one `.arbc` twice would get two composition identities and two COLD caches --
  // which is exactly what doc 08 Principle 3's "deduplicates through the loader by resolved
  // identity" exists to prevent. The witness is a behavioural counter, not a timing.
  //
  // The two references are spelled DIFFERENTLY on purpose (`child.arbc` and `./child.arbc`):
  // dedup is by resolved IDENTITY, not by the authored string.
  MemoryAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  const std::string parent =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)"
      R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"child.arbc"}},)"
      R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"./child.arbc"}}]}})";

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));

  const NestedContent& first = nested_at(doc, 0);
  const NestedContent& second = nested_at(doc, 1);
  CHECK(first.child().valid());
  CHECK(first.child() == second.child()); // ONE child composition ...
  CHECK(source.requests() == 1);          // ... fetched exactly ONCE

  // Each content keeps the string ITS layer authored (Decision 4): the suppression is
  // per-content, so two nested contents may share one child while carrying different
  // authored spellings, and each re-saves the one it was given.
  CHECK(first.ref() == "child.arbc");
  CHECK(second.ref() == "./child.arbc");
  const std::string saved = save(doc, bridge);
  CHECK(saved.find(R"("ref": "child.arbc")") != std::string::npos);
  CHECK(saved.find(R"("ref": "./child.arbc")") != std::string::npos);
  // The child's contents belong to the OTHER document: not hoisted into this one's tables.
  CHECK(saved.find("\"compositions\"") == std::string::npos);
  CHECK(saved.find("\"contents\"") == std::string::npos);
  CHECK(saved.find("org.arbc.solid") == std::string::npos);
}

// enforces: 05-recursive-composition#external-cycle-terminates-at-load
// enforces: 08-serialization#droste-cycle-round-trips-as-data
TEST_CASE("a cross-document external cycle loads as a finite graph") {
  // A→B→A. Doc 08:151-156 protects this explicitly -- "composition cycles (Droste, doc 05)
  // ride the compositions table OR an external params.ref URI -- neither is a contents/$ref
  // edge" -- so an external cycle must NOT be rejected. Termination is
  // ALLOCATE-BEFORE-PARSE: the loader records `a.arbc -> a's root` before a's bytes are
  // parsed, so B's back-edge resolves to the IN-FLIGHT composition instead of re-loading it.
  MemoryAssetSource source;
  source.put("mem/b.arbc", nesting_doc("a.arbc"));

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("b.arbc"), doc, bridge, registry, "mem/a.arbc", &source));

  const DocStatePtr pin = doc.pin();
  ObjectId a_root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(a_root, rec));

  // A's nested child is B's composition; B's nested child is A's ROOT -- the knot, cut.
  const NestedContent& a_nested = nested_at(doc, 0);
  REQUIRE(a_nested.child().valid());
  CHECK(a_nested.child() != a_root);

  ObjectId b_nest_content;
  pin->for_each_layer_in(a_nested.child(), [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    b_nest_content = lr->content;
  });
  const auto* const b_nested = dynamic_cast<const NestedContent*>(doc.resolve(b_nest_content));
  REQUIRE(b_nested != nullptr);
  CHECK(b_nested->child() == a_root); // the back-edge landed on A's own root

  // B was fetched exactly once, and A was NEVER fetched: the loader seeded its map with this
  // document's own base URI -> its own root composition, so the back-edge deduped instead of
  // re-reading the bytes it is already reading.
  CHECK(source.requests() == 1);

  // The cyclic graph renders, bounded by the sub-pixel cull -- no unbounded recursion.
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  PullConfig config;
  config.id_of = arbc::make_pull_identity_of(*pin, resolve);
  const std::uint64_t revision = pin->revision();
  config.contribution = [revision](const arbc::Content*) { return revision; };
  PullServiceImpl service(cache, backend, arbc::direct_dispatch(), config);
  arbc::register_builtin_operator_binders();
  arbc::OperatorBindingScope scope = arbc::bind_operators(doc, service, backend, pin);
  CHECK(a_nested.bounds().has_value()); // the memo walk terminated over the cycle
  scope.release();
}

// enforces: 05-recursive-composition#external-cycle-terminates-at-load
TEST_CASE("a document that references ITSELF resolves to its own root composition") {
  // The degenerate cycle. Constraint 6: the top-level document seeds the loader's map with
  // its OWN base URI -> its own root composition id, so a self-reference collapses onto the
  // in-document Droste case exactly -- same graph, same id, no second copy, no fetch at all.
  MemoryAssetSource source;

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("a.arbc"), doc, bridge, registry, "mem/a.arbc", &source));

  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));

  const NestedContent& nested = nested_at(doc, 0);
  CHECK(nested.child() == root);
  CHECK(source.requests() == 0); // the document is already in hand: never re-fetched
  CHECK(nested.ref() == "a.arbc");
}

// enforces: 05-recursive-composition#unresolvable-external-ref-renders-placeholder
TEST_CASE("an unresolvable external reference is unavailable, not a read error") {
  const std::string parent = nesting_doc("missing.arbc");

  auto is_unavailable = [&](Document& doc, KindBridge& bridge) {
    // The parent load SUCCEEDED, the content is a real NestedContent (not an unknown-kind
    // placeholder -- the kind is perfectly well known and its reference must stay live,
    // editable and re-resolvable), its child is null, and its `ref` survives verbatim.
    const NestedContent& nested = nested_at(doc, 0);
    CHECK_FALSE(nested.child().valid());
    CHECK(nested.ref() == "missing.arbc");
    CHECK(nested.bounds().has_value()); // describes as the empty placeholder, never a crash
    const std::string saved = save(doc, bridge);
    CHECK(saved.find(R"("ref": "missing.arbc")") != std::string::npos);
    CHECK(saved.find(R"("kind": "org.arbc.nested")") != std::string::npos);
    CHECK(saved.find("\"compositions\"") == std::string::npos);
    // Re-loading the re-save is a fixed point: the reference survives arbitrarily many
    // open/save cycles with the file still missing.
    Document again;
    KindBridge again_bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(saved, again, again_bridge, registry));
    CHECK(save(again, again_bridge) == saved);
  };

  SECTION("the source does not have the file") {
    MemoryAssetSource source; // empty: every request reports absence
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));
    is_unavailable(doc, bridge);
    CHECK(source.requests() == 1);
  }

  SECTION("a SECOND reference to the same missing file costs no second request") {
    // Unavailability is remembered in the dedup map exactly as a successful load is, so a
    // broken target is fetched once however many contents point at it.
    MemoryAssetSource source;
    const std::string two =
        R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)"
        R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"missing.arbc"}},)"
        R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"./missing.arbc"}}]}})";
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(two, doc, bridge, registry, "mem/parent.arbc", &source));
    CHECK_FALSE(nested_at(doc, 0).child().valid());
    CHECK_FALSE(nested_at(doc, 1).child().valid());
    CHECK(source.requests() == 1);
  }

  SECTION("no AssetSource is installed at all") {
    // `LoadContext::load_asset` with no source fires the continuation with empty bytes, so
    // this is the same path -- which is why the fuzz lane exercises unavailability by
    // construction, and why a host that never wires a source still opens every document.
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(parent, doc, bridge, registry));
    is_unavailable(doc, bridge);
  }

  SECTION("the target is not a valid .arbc") {
    MemoryAssetSource source;
    source.put("mem/missing.arbc", "{ this is not a document");
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));
    is_unavailable(doc, bridge);
  }
}

// enforces: 08-serialization#loader-never-faults-on-hostile-input
TEST_CASE("a hostile acyclic external chain is bounded by the load-time depth cap") {
  // Dedup bounds every CYCLE, but it cannot bound a chain of ten thousand DISTINCT files,
  // which a malicious project directory can produce and which would otherwise overflow the
  // C++ stack. So the chain is capped (Decision 5): the load succeeds, does not fault, and
  // the reference AT the cap is simply unavailable -- a null child and a preserved `ref`,
  // like a missing file.
  constexpr std::size_t k_chain = arbc::k_external_ref_depth_cap + 8;
  MemoryAssetSource source;
  for (std::size_t i = 0; i < k_chain; ++i) {
    source.put("mem/d" + std::to_string(i) + ".arbc",
               nesting_doc("d" + std::to_string(i + 1) + ".arbc"));
  }

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("d0.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));

  // Exactly `k_external_ref_depth_cap` links were followed, and not one more: the cap is a
  // behavioural counter, not a hope.
  CHECK(source.requests() == arbc::k_external_ref_depth_cap);
  CHECK(nested_at(doc, 0).child().valid()); // the top of the chain loaded fine
  CHECK(save(doc, bridge).find(R"("ref": "d0.arbc")") != std::string::npos);
}
