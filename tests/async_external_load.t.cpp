// runtime.async_external_load: the deferring loader's behavioural proofs, driven by an
// in-memory `AssetSource` double whose `on_ready` fires when the TEST says so and not before.
//
// That is the whole point of the task made drivable. `nested_external_ref`'s `MemoryAssetSource`
// answers INSIDE `request()`, exactly as `FilesystemAssetSource` does; this one records the
// continuation and returns, which is what every real source does -- an HTTP fetch, a content
// store, a plugin-supplied loader. Arrival is therefore a thing the test SCHEDULES, so every
// assertion here is a behavioural counter over a deterministic sequence and not one line of it
// depends on wall-clock timing (doc 16:54-62).
//
// The pixel half -- the placeholder actually replaced by the child's real pixels, byte-exactly
// -- is tests/async_external_load_golden.t.cpp.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc`): these drive the full L5 load façade
// plus a real `CpuBackend` + `OperatorBindingScope` for the render-does-not-fault proofs, which
// a runtime component test may not name (doc 17 / check_levels.py).

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/external_composition_loader.hpp> // k_external_ref_depth_cap
#include <arbc/runtime/host_viewport.hpp>               // the Document-bound viewport
#include <arbc/runtime/interactive.hpp>                 // InteractiveRenderer
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/serialize/load_context.hpp> // AssetSource
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using arbc::Affine;
using arbc::ContentResolver;
using arbc::CpuBackend;
using arbc::Damage;
using arbc::DocStatePtr;
using arbc::Document;
using arbc::KindBridge;
using arbc::NestedContent;
using arbc::ObjectId;
using arbc::PullConfig;
using arbc::PullServiceImpl;
using arbc::Rect;
using arbc::Registry;
using arbc::Surface;
using arbc::SurfacePool;
using arbc::TileCache;
using arbc::TimeRange;
using arbc::Viewport;

// The DEFERRING `AssetSource` double: `request()` records `(uri, on_ready)` and fires nothing.
// The test decides when the bytes arrive, so the async boundary is crossed on command rather
// than on a timer -- no sleeps, no polling, no flake.
class DeferringAssetSource final : public arbc::AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.insert_or_assign(std::move(uri), std::move(bytes));
  }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    ++d_requests;
    d_outstanding.push_back(Request{std::string(resolved_uri), std::move(on_ready)});
  }

  // Deliver every request outstanding RIGHT NOW, and return how many. Requests issued by the
  // settle these arrivals trigger (a grandchild's) stay outstanding for the next round: a
  // deferring chain lands one link per settle, which is exactly the shape of the real thing.
  std::size_t fire_all() {
    std::vector<Request> firing;
    firing.swap(d_outstanding);
    for (const Request& r : firing) {
      deliver(r);
    }
    return firing.size();
  }

  // Deliver only the outstanding requests for one URI.
  std::size_t fire(const std::string& uri) {
    std::vector<Request> firing;
    std::vector<Request> keep;
    for (Request& r : d_outstanding) {
      (r.uri == uri ? firing : keep).push_back(std::move(r));
    }
    d_outstanding.swap(keep);
    for (const Request& r : firing) {
      deliver(r);
    }
    return firing.size();
  }

  // The dedup witness across the async boundary (doc 08 Principle 3): two references to one
  // file must cost ONE fetch, whether or not its bytes have come back yet.
  std::size_t requests() const noexcept { return d_requests; }
  std::size_t outstanding() const noexcept { return d_outstanding.size(); }

private:
  struct Request {
    std::string uri;
    std::function<void(std::string_view)> on_ready;
  };

  void deliver(const Request& r) const {
    const auto it = d_files.find(r.uri);
    // Absent == empty bytes, exactly as the `AssetSource` contract spells absence.
    r.on_ready(it != d_files.end() ? std::string_view(it->second) : std::string_view{});
  }

  std::unordered_map<std::string, std::string> d_files;
  std::vector<Request> d_outstanding;
  std::size_t d_requests{0};
};

// A source that ANSWERS inline with empty bytes -- the source that "did answer", which is
// UNAVAILABLE and always was (Constraint 2). Its existence beside the deferring double is the
// two-outcomes-become-three split, asserted from both sides.
class AbsentAssetSource final : public arbc::AssetSource {
public:
  void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
    ++d_requests;
    on_ready(std::string_view{});
  }
  std::size_t requests() const noexcept { return d_requests; }

private:
  std::size_t d_requests{0};
};

// Records each `flush` a commit makes, so "the arrival's damage rides the install's OWN
// commit" is a checkable statement about which flush carried what, not a hope.
class RecordingDamageSink final : public arbc::DamageSink {
public:
  void flush(const std::vector<Damage>& damage) override { d_flushes.push_back(damage); }
  const std::vector<std::vector<Damage>>& flushes() const noexcept { return d_flushes; }

private:
  std::vector<std::vector<Damage>> d_flushes;
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

ObjectId root_composition_of(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const arbc::CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  return root;
}

// The ObjectIds of the root composition's layer-root contents, in membership order.
std::vector<ObjectId> root_content_ids(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  std::vector<ObjectId> out;
  pin->for_each_layer_in(root_composition_of(doc), [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    out.push_back(lr->content);
  });
  return out;
}

ObjectId nested_id(const Document& doc, std::size_t index) {
  const std::vector<ObjectId> ids = root_content_ids(doc);
  REQUIRE(index < ids.size());
  return ids[index];
}

const NestedContent& nested_at(const Document& doc, std::size_t index) {
  const auto* const nested = dynamic_cast<const NestedContent*>(doc.resolve(nested_id(doc, index)));
  REQUIRE(nested != nullptr);
  return *nested;
}

std::string save(const Document& doc, const KindBridge& bridge) {
  const arbc::expected<std::string, arbc::SerializeError> out = arbc::save_document(doc, bridge);
  REQUIRE(out);
  return *out;
}

// Render the root composition through the live engine -- a real backend, a real pull service, a
// real binding scope. Used only to prove the pending state does not FAULT; the byte-exact
// oracle comparison is the golden's job.
void render_root(Document& doc, int dim) {
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
  scope.release();
}

bool names(const std::vector<Damage>& set, ObjectId object) {
  return std::any_of(set.begin(), set.end(),
                     [object](const Damage& d) { return d.object == object; });
}

// A constant clock: the free-run transport advances by zero flicks every step, so a still,
// undamaged scene is genuinely still and no assertion below reads the wall clock.
arbc::HostViewport::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// The whole host side of a Document-bound viewport, in one object
// (runtime.host_viewport_document_binding). Everything the frame needs -- the resolver, the
// damage-sink install, the external-arrival settle hook -- is derived by the constructor from
// `doc` and `binding`. There is no `settle_external_loads` call ANYWHERE in the two tests that
// use this: `step()` is the only thing the host drives.
class DocumentViewport {
public:
  DocumentViewport(Document& doc, KindBridge& bridge, const Registry& registry, int dim)
      : d_cache(64U * 1024 * 1024), d_pool(d_backend),
        d_target(d_backend.make_surface(dim, dim, doc.pin()->working_space())),
        d_viewport(d_renderer, doc, arbc::HostViewport::DocumentBinding{&bridge, &registry},
                   d_backend, d_pool, d_cache, checked(d_target), epoch_clock(), config(doc, dim)) {
  }

  arbc::HostViewport& operator*() noexcept { return d_viewport; }
  arbc::HostViewport* operator->() noexcept { return &d_viewport; }

private:
  using Target = arbc::expected<std::unique_ptr<Surface>, arbc::SurfaceError>;

  static Surface& checked(Target& target) {
    REQUIRE(target.has_value()); // throws on failure, so the viewport below is never built blind
    return **target;
  }

  static arbc::HostViewport::Config config(const Document& doc, int dim) {
    arbc::HostViewport::Config cfg;
    cfg.viewport = Viewport{dim, dim, Affine::identity(), root_composition_of(doc)};
    return cfg;
  }

  CpuBackend d_backend;
  TileCache d_cache;
  SurfacePool d_pool;
  Target d_target;
  arbc::InteractiveRenderer d_renderer{{}, epoch_clock()};
  arbc::HostViewport d_viewport;
};

} // namespace

// enforces: 05-recursive-composition#deferred-external-child-installs-live
// enforces: 05-recursive-composition#external-nested-loads-through-loadcontext
TEST_CASE("a pending external child leaves a VALID id, an absent record, and revision 0") {
  // The state doc 05 now names, asserted directly. A source that has not answered YET is not a
  // source that answered ABSENCE: the loader returns the child id it already minted under
  // allocate-before-fetch, so the embedding content binds a valid id whose composition record
  // simply is not there. Nothing at the render layer needs to know -- a child id resolving to
  // no record is already the doc-05 placeholder.
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));

  // Constraint 3: the parent load SUCCEEDED and did not wait. A document whose every external
  // child is pending lands at exactly the baseline a childless one does.
  CHECK(doc.pin()->revision() == 0);

  const NestedContent& nested = nested_at(doc, 0);
  CHECK(nested.child().valid());                                 // ... a valid id ...
  CHECK(doc.pin()->find_composition(nested.child()) == nullptr); // ... naming no record.
  CHECK(nested.ref() == "child.arbc");                           // the reference survives
  CHECK(nested.bounds().has_value());                            // the empty placeholder
  CHECK_FALSE(nested.time_extent().has_value());

  // The fetch was ISSUED (it is in flight), and it has not answered.
  CHECK(source.requests() == 1);
  CHECK(source.outstanding() == 1);
  CHECK(doc.pending_external_loads() == 1);

  // And it renders -- the placeholder, without faulting on the record that is not there.
  REQUIRE_NOTHROW(render_root(doc, 16));
}

// enforces: 05-recursive-composition#deferred-external-child-installs-live
TEST_CASE("settling a deferred arrival installs on a NEW revision and damages the embedding") {
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));
  const ObjectId root_before = root_composition_of(doc);
  const ObjectId embedding = nested_id(doc, 0);
  const ObjectId child = nested_at(doc, 0).child();

  // Subscribe to the model's damage exactly as a host frame loop does (doc 02:51-52).
  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);

  CHECK(source.fire_all() == 1);                                  // the bytes arrive ...
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1); // ... and land
  CHECK(doc.pending_external_loads() == 0);

  const DocStatePtr pin = doc.pin();
  // A NEW revision, not a republished baseline: `load_composition` installs through an
  // ordinary transaction, so the host's own root id and its own layers survive untouched.
  // `load_baseline` would have discarded both.
  CHECK(pin->revision() > 0);
  CHECK(root_composition_of(doc) == root_before);
  CHECK(root_content_ids(doc).size() == 1);
  CHECK(nested_id(doc, 0) == embedding);
  CHECK(nested_at(doc, 0).child() == child);      // the SAME pre-allocated id ...
  CHECK(pin->find_composition(child) != nullptr); // ... now names a real record

  // Decision 3: the damage naming the embedding content rides the install's OWN commit. It is
  // flushed exactly once across the whole settle (a second commit carrying it would publish an
  // intermediate revision whose damage set was empty despite structurally changing what the
  // parent renders), and the commit that carries it is the one that installed the child --
  // which is what "the placeholder is replaced live" rests on, rather than on an ordering
  // argument between two commits.
  std::size_t naming_embedding = 0;
  const std::vector<Damage>* install_flush = nullptr;
  for (const std::vector<Damage>& flush : sink.flushes()) {
    if (names(flush, embedding)) {
      ++naming_embedding;
      install_flush = &flush;
    }
  }
  CHECK(naming_embedding == 1);
  REQUIRE(install_flush != nullptr);
  CHECK(names(*install_flush, child)); // same commit: the child's composition is in it too
  for (const Damage& d : *install_flush) {
    if (d.object == embedding) {
      // The absorbing, structural shape (damage.hpp:64-73): a child appearing where a
      // placeholder was changes the parent everywhere, at every instant.
      CHECK(d.rect == Rect::infinite());
      CHECK(d.range == TimeRange::all());
    }
  }

  doc.set_damage_sink(nullptr);
}

// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
// enforces: 08-serialization#loadcontext-dedups-by-resolved-identity
TEST_CASE("a DEFERRING cross-document cycle terminates, and fetches each target once") {
  // a.arbc -> b.arbc -> a.arbc, with NOTHING answering inline. The knot-cut is
  // allocate-before-FETCH: b's id is in the dedup map before `request()` is issued, so a
  // back-edge reaching b while its bytes are still in flight resolves to the in-flight id and
  // issues no second request -- and a's own URI is seeded, so a is never fetched at all. This
  // is the case a naive queue gets wrong: record the id only after the parse and the cycle
  // re-fetches forever.
  DeferringAssetSource source;
  source.put("mem/b.arbc", nesting_doc("a.arbc"));

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("b.arbc"), doc, bridge, registry, "mem/a.arbc", &source));

  const ObjectId a_root = root_composition_of(doc);
  const NestedContent& a_nested = nested_at(doc, 0);
  REQUIRE(a_nested.child().valid());
  CHECK(a_nested.child() != a_root);
  CHECK(doc.pin()->find_composition(a_nested.child()) == nullptr); // b: pending
  CHECK(source.requests() == 1);

  CHECK(source.fire_all() == 1);
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1);

  // b is fetched exactly ONCE and a is NEVER fetched: the back-edge deduped across the async
  // boundary rather than re-reading the document already in hand.
  CHECK(source.requests() == 1);
  CHECK(source.outstanding() == 0);
  CHECK(doc.pending_external_loads() == 0);

  const DocStatePtr pin = doc.pin();
  REQUIRE(pin->find_composition(a_nested.child()) != nullptr);

  // b's nested child is a's ROOT -- the knot, cut, on a later revision.
  ObjectId b_nest_content;
  pin->for_each_layer_in(a_nested.child(), [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    b_nest_content = lr->content;
  });
  const auto* const b_nested = dynamic_cast<const NestedContent*>(doc.resolve(b_nest_content));
  REQUIRE(b_nested != nullptr);
  CHECK(b_nested->child() == a_root);

  // The cyclic graph renders, bounded by the sub-pixel cull.
  REQUIRE_NOTHROW(render_root(doc, 16));
}

// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
TEST_CASE("a deferring GRANDCHILD chain lands across successive settles") {
  // a -> b -> c, all deferring. c's request cannot even be ISSUED until b's bytes are parsed,
  // so the chain needs one settle per link -- and each settle must drain its OWN ready queue to
  // quiescence, because a source answering inline for a just-parsed child would have queued its
  // bytes mid-round.
  DeferringAssetSource source;
  source.put("mem/b.arbc", nesting_doc("c.arbc"));
  source.put("mem/c.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("b.arbc"), doc, bridge, registry, "mem/a.arbc", &source));

  // Round 0: only b has been requested -- c is not yet KNOWN to exist.
  CHECK(source.requests() == 1);
  CHECK(doc.pending_external_loads() == 1);

  // Round 1: b lands. Parsing b is what discovers `c.arbc` and issues its fetch.
  CHECK(source.fire_all() == 1);
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1);
  CHECK(source.requests() == 2);
  CHECK(doc.pending_external_loads() == 1); // now it is c that is pending
  const std::uint64_t after_b = doc.pin()->revision();
  CHECK(after_b > 0);

  // A settle with nothing ready installs nothing and costs one queue check.
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 0);

  // Round 2: c lands. The graph is complete.
  CHECK(source.fire_all() == 1);
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1);
  CHECK(source.requests() == 2);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(doc.pin()->revision() > after_b);

  const DocStatePtr pin = doc.pin();
  const ObjectId b_root = nested_at(doc, 0).child();
  REQUIRE(pin->find_composition(b_root) != nullptr);
  ObjectId b_nest_content;
  pin->for_each_layer_in(b_root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    b_nest_content = lr->content;
  });
  const auto* const b_nested = dynamic_cast<const NestedContent*>(doc.resolve(b_nest_content));
  REQUIRE(b_nested != nullptr);
  REQUIRE(b_nested->child().valid());
  CHECK(pin->find_composition(b_nested->child()) != nullptr); // c, installed under b
}

// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
// enforces: 08-serialization#loader-never-faults-on-hostile-input
TEST_CASE("the depth cap counts PRE-deferral depth, so a deferring hostile chain still caps") {
  // The one place a plausible implementation ships a security regression. `d_depth` is a LIVE
  // recursion counter, and a deferred parse resumes on a fresh stack: reset it to zero at
  // settle and every link of a hostile chain looks like depth 0, so the cap silently stops
  // capping in exactly the configuration -- a network source -- where it matters most. Each
  // pending entry therefore carries the depth at which its reference was REACHED, and settle
  // restores it. Link 65 is unavailable; no further request is ever issued.
  constexpr std::size_t k_chain = arbc::k_external_ref_depth_cap + 6;
  DeferringAssetSource source;
  for (std::size_t i = 0; i < k_chain; ++i) {
    source.put("mem/d" + std::to_string(i) + ".arbc",
               nesting_doc("d" + std::to_string(i + 1) + ".arbc"));
  }

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("d0.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));

  // Each round issues at most one new request: the chain descends one link per settle, because
  // link k+1's URI is only discovered by parsing link k. Drive it to quiescence.
  for (std::size_t round = 0; round < k_chain + 4; ++round) {
    if (source.fire_all() == 0) {
      break;
    }
    static_cast<void>(arbc::settle_external_loads(doc, bridge, registry));
  }

  // Exactly `k_external_ref_depth_cap` links were followed, and not one more -- the same
  // behavioural counter the SYNCHRONOUS chain asserts, unchanged by the async boundary.
  CHECK(source.requests() == arbc::k_external_ref_depth_cap);
  CHECK(source.outstanding() == 0);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(nested_at(doc, 0).child().valid()); // the top of the chain landed fine
  REQUIRE_NOTHROW(render_root(doc, 16));
}

// enforces: 05-recursive-composition#unresolvable-external-ref-renders-placeholder
// enforces: 08-serialization#external-composition-ref-round-trips
TEST_CASE("unavailable is still unavailable: the split is about ANSWERING, not about bytes") {
  const std::string parent = nesting_doc("child.arbc");

  SECTION("a source that ANSWERS with empty bytes is unavailable -- a NULL child") {
    // Today's behavior, unchanged, and the whole reason the pending flag is set inside the
    // callback rather than inferred from the emptiness of the bytes afterwards (Constraint 2).
    AbsentAssetSource source;
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));
    CHECK_FALSE(nested_at(doc, 0).child().valid()); // NULL, not pending
    CHECK(doc.pending_external_loads() == 0);
    CHECK(source.requests() == 1);
    // A settle has nothing to do: nothing was ever in flight.
    CHECK(arbc::settle_external_loads(doc, bridge, registry) == 0);
    CHECK(doc.pin()->revision() == 0);
  }

  SECTION("a source that DEFERS and then answers empty settles to a permanent placeholder") {
    // The deferred half of the same absence. The id was already handed to the embedding content
    // before the source spoke (that is what allocate-before-fetch means), and `NestedContent`
    // has no `set_child` -- deliberately, Decision 3: a mutable `d_child` on a content read by
    // render workers is a data race the design does not have. So the child id stays valid and
    // simply never names a record: pixel-for-pixel the doc-05 placeholder, byte-for-byte the
    // same save, and no revision published for a child that never came.
    DeferringAssetSource source; // knows no files: every fire delivers empty bytes
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));
    CHECK(doc.pending_external_loads() == 1);

    CHECK(source.fire_all() == 1);
    CHECK(arbc::settle_external_loads(doc, bridge, registry) == 0); // nothing installed
    CHECK(doc.pending_external_loads() == 0);                       // and nothing still owed
    CHECK(doc.pin()->revision() == 0);                              // no revision for a no-show

    const NestedContent& nested = nested_at(doc, 0);
    CHECK(doc.pin()->find_composition(nested.child()) == nullptr);
    CHECK(nested.ref() == "child.arbc");
    CHECK(nested.bounds().has_value()); // the empty placeholder, never a crash
    REQUIRE_NOTHROW(render_root(doc, 16));
    CHECK(save(doc, bridge).find(R"("ref": "child.arbc")") != std::string::npos);
  }

  SECTION("a source that NEVER answers leaves the child pending forever, and all is well") {
    // The document saves, renders and closes cleanly with a fetch still in flight. This is the
    // steady state of any real editor: open a project, start rendering, the widgets stream in.
    DeferringAssetSource source;
    source.put("mem/child.arbc", k_leaf);
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));
    CHECK(doc.pending_external_loads() == 1);
    CHECK(arbc::settle_external_loads(doc, bridge, registry) == 0);
    REQUIRE_NOTHROW(render_root(doc, 16));
    CHECK(save(doc, bridge).find(R"("ref": "child.arbc")") != std::string::npos);
    CHECK(doc.pin()->revision() == 0);
    // ... and the Document destructs with the fetch still outstanding.
  }
}

// enforces: 05-recursive-composition#deferred-external-child-installs-live
TEST_CASE("a source that answers TWICE installs once: the second arrival finds nothing pending") {
  // `AssetSource::request` promises `on_ready` is invoked "once they are available" -- it does
  // not promise a badly-written source will invoke it only once, and a host installs whatever
  // source a plugin hands it. A second delivery for an already-settled child must therefore be
  // a no-op, not a second install of the same composition under the same id: the pending entry
  // is consumed by the first settle, so the second finds nothing to settle and drops the bytes.
  class DoubleFiringSource final : public arbc::AssetSource {
  public:
    void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
      ++d_requests;
      d_ready.push_back(std::move(on_ready));
    }
    // Deliver every outstanding continuation, WITHOUT consuming it -- so the next call fires
    // the same callbacks again, which is exactly the misbehaviour under test.
    void fire_all_again() const {
      for (const std::function<void(std::string_view)>& on_ready : d_ready) {
        on_ready(k_leaf);
      }
    }
    std::size_t requests() const noexcept { return d_requests; }

  private:
    std::vector<std::function<void(std::string_view)>> d_ready;
    std::size_t d_requests{0};
  };

  DoubleFiringSource source;
  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));
  CHECK(source.requests() == 1);
  CHECK(doc.pending_external_loads() == 1);

  source.fire_all_again();
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1); // the first arrival installs
  const ObjectId child = nested_at(doc, 0).child();
  const std::uint64_t after_first = doc.pin()->revision();
  REQUIRE(doc.pin()->find_composition(child) != nullptr);

  source.fire_all_again();
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 0); // ... the second installs NOTHING
  CHECK(doc.pin()->revision() == after_first);                    // no second revision
  CHECK(nested_at(doc, 0).child() == child);                      // and the same one child
  CHECK(root_content_ids(doc).size() == 1);
}

// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
TEST_CASE("a callback firing after its Document is destroyed drops its bytes and faults nothing") {
  // Constraint 6. A network fetch can fire long after the user closed the document, and the
  // completion queue is exactly the object it would write through. So the queue is owned by
  // `shared_ptr` and every `on_ready` captures a `weak_ptr`: a callback whose queue has expired
  // returns having done nothing. No use-after-free, no resurrection of a dead document. Runs
  // under ASan in the existing lane, where a captured raw pointer would be a hard failure.
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  {
    Document doc;
    KindBridge bridge;
    const Registry registry;
    REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                                &source));
    CHECK(doc.pending_external_loads() == 1);
  } // the Document -- and the completion queue it owned -- are gone here

  CHECK(source.outstanding() == 1);
  REQUIRE_NOTHROW(source.fire_all()); // the bytes arrive for a document that no longer exists
  CHECK(source.outstanding() == 0);
}

// enforces: 05-recursive-composition#deferred-external-child-installs-live
TEST_CASE("two pending references to one URI share one in-flight child and one fetch") {
  // Dedup across the async boundary, and the arrival damaging BOTH embeddings. The second
  // reference resolves to the id minted for the first WHILE its bytes are in flight, so it
  // costs no second request -- and when they land, one install serves both parents.
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);
  const std::string parent =
      R"({"arbc":{"format":1},"composition":{"canvas":[0,0,16,16],"layers":[)"
      R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"child.arbc"}},)"
      R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":"./child.arbc"}}]}})";

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(parent, doc, bridge, registry, "mem/parent.arbc", &source));

  CHECK(source.requests() == 1); // ONE fetch, though the bytes had not come back
  CHECK(doc.pending_external_loads() == 1);
  const ObjectId first = nested_id(doc, 0);
  const ObjectId second = nested_id(doc, 1);
  CHECK(nested_at(doc, 0).child() == nested_at(doc, 1).child());

  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);
  CHECK(source.fire(std::string("mem/child.arbc")) == 1);
  CHECK(arbc::settle_external_loads(doc, bridge, registry) == 1); // one install ...

  // ... damaging BOTH parents, in that same commit.
  const std::vector<Damage>* install_flush = nullptr;
  for (const std::vector<Damage>& flush : sink.flushes()) {
    if (names(flush, first)) {
      install_flush = &flush;
    }
  }
  REQUIRE(install_flush != nullptr);
  CHECK(names(*install_flush, second));
  doc.set_damage_sink(nullptr);

  CHECK(doc.pin()->find_composition(nested_at(doc, 0).child()) != nullptr);
  // Each content still re-saves the string ITS layer authored.
  const std::string saved = save(doc, bridge);
  CHECK(saved.find(R"("ref": "child.arbc")") != std::string::npos);
  CHECK(saved.find(R"("ref": "./child.arbc")") != std::string::npos);
  CHECK(saved.find("\"compositions\"") == std::string::npos);
}

// --- The arrival, settled by a FRAME (runtime.host_viewport_document_binding) -------------
//
// Everything above drives the free `settle_external_loads` by hand, which is the loader's
// contract but not the host's experience of it. These two drive a real `HostViewport` bound to
// the `Document` and call NOTHING but `step()` -- the test Decision 7 of
// `runtime.async_external_load` promised ("the pending count drops to zero and a new revision
// publishes across a frame") and never landed, because the seam it landed the settle hook on
// could not be built against a `Document` at all.

// enforces: 01-core-concepts#viewport-binds-to-document
// enforces: 05-recursive-composition#deferred-external-child-installs-live
TEST_CASE("a deferred external arrival settles inside the frame that observes it") {
  DeferringAssetSource source;
  source.put("mem/child.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("child.arbc"), doc, bridge, registry, "mem/parent.arbc",
                              &source));

  // The load completed WITHOUT the child: the embedding binds a valid, pre-allocated id that
  // names no composition yet, which is the doc-05 placeholder.
  const ObjectId child = nested_at(doc, 0).child();
  REQUIRE(child.valid());
  CHECK(doc.pending_external_loads() == 1);
  CHECK(doc.pin()->find_composition(child) == nullptr);
  const std::uint64_t before = doc.pin()->revision();

  DocumentViewport viewport(doc, bridge, registry, 16);

  // A baseline step with nothing arrived: the load's own commits predate the viewport's sink
  // install, so there is no damage, no frame, and the settle hook finds an empty queue. An
  // idle document-bound viewport costs one queue check per step and nothing more.
  viewport->step();
  CHECK(viewport->frames_issued() == 0);
  CHECK(viewport->external_loads_settled() == 0);
  CHECK(doc.pending_external_loads() == 1);

  // The bytes come back on the source's own schedule. The host does not settle them -- it does
  // not know they arrived. It just runs its next frame.
  CHECK(source.fire_all() == 1);
  viewport->step();

  // And that frame is where the arrival landed: the queue drained, a new revision published,
  // the child now names a real composition, and the SAME step issued the frame that composites
  // it -- the install's commit flushed damage naming the embedding into this viewport's
  // accumulator, ahead of the pin and the drain (doc 02 step 1: an arrival IS damage).
  CHECK(doc.pending_external_loads() == 0);
  CHECK(viewport->external_loads_settled() == 1);
  CHECK(doc.pin()->revision() > before);
  CHECK(doc.pin()->find_composition(child) != nullptr);
  CHECK(viewport->frames_issued() == 1);

  // Steady state again: the hook is polled, settles nothing, and wakes no frame by itself.
  viewport->step();
  CHECK(viewport->external_loads_settled() == 1);
  CHECK(viewport->frames_issued() == 1);
}

// enforces: 05-recursive-composition#deferred-external-chain-and-cycle-terminate
TEST_CASE("a deferring grandchild chain lands over successive frames, driven only by step()") {
  // a -> b -> c, all deferring: c's request cannot even be ISSUED until b's bytes are parsed.
  // The free-function proof above pins that the chain lands one link per settle; this pins that
  // "the host calls this once per frame" (`document_serialize.hpp:172-175`) is a real frame.
  DeferringAssetSource source;
  source.put("mem/b.arbc", nesting_doc("c.arbc"));
  source.put("mem/c.arbc", k_leaf);

  Document doc;
  KindBridge bridge;
  const Registry registry;
  REQUIRE(arbc::load_document(nesting_doc("b.arbc"), doc, bridge, registry, "mem/a.arbc", &source));
  CHECK(doc.pending_external_loads() == 1);

  DocumentViewport viewport(doc, bridge, registry, 16);

  // Frame 1: b lands. Parsing b is what DISCOVERS c and issues its fetch, so the pending count
  // does not fall -- it walks along the chain.
  CHECK(source.fire_all() == 1);
  viewport->step();
  CHECK(viewport->external_loads_settled() == 1);
  CHECK(source.requests() == 2);
  CHECK(doc.pending_external_loads() == 1); // now it is c that is pending
  CHECK(viewport->frames_issued() == 1);

  // Frame 2: c lands under b. The graph is complete, and the second install woke its own frame.
  CHECK(source.fire_all() == 1);
  viewport->step();
  CHECK(viewport->external_loads_settled() == 2);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(viewport->frames_issued() == 2);

  const DocStatePtr pin = doc.pin();
  const ObjectId b_root = nested_at(doc, 0).child();
  REQUIRE(pin->find_composition(b_root) != nullptr);
  ObjectId b_nest_content;
  pin->for_each_layer_in(b_root, [&](ObjectId lid) {
    const arbc::LayerRecord* lr = pin->find_layer(lid);
    REQUIRE(lr != nullptr);
    b_nest_content = lr->content;
  });
  const auto* const b_nested = dynamic_cast<const NestedContent*>(doc.resolve(b_nest_content));
  REQUIRE(b_nested != nullptr);
  CHECK(pin->find_composition(b_nested->child()) != nullptr); // c, installed under b
}
