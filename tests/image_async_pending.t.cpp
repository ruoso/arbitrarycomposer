// kinds.image_async_pending: org.arbc.image's THIRD load state, driven by a DEFERRING
// `AssetSource` double whose `on_ready` fires when the TEST says so and not before.
//
// `kinds.image` shipped two outcomes -- resolved, and unavailable -- and read a source that had
// not ANSWERED as absence. So an image behind a content store, an object store or a network
// mount rendered as nothing, permanently, with no error and no recovery: the kind silently
// degraded to a no-op in exactly the deployment its design anticipates. Its Decision 5 named
// that conflation and named this task as the fix.
//
// The split is `external_composition_loader.hpp:93-95`, applied to assets: PENDING and
// UNAVAILABLE differ by WHETHER THE SOURCE ANSWERED, never by the bytes being empty. Every
// assertion below is a behavioural counter over a sequence the test schedules, so not one line
// of it depends on wall-clock timing (doc 16:54-62). The pixel half -- the empty layer actually
// replaced by the photograph -- is `tests/image_async_pending_golden.t.cpp`.
//
// CROSS-COMPONENT (tests/, linking the umbrella `arbc` plus the plugin's impl archive): these
// drive the full L5 load façade plus a real `CpuBackend` for the render-does-not-fault proofs.

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
#include <arbc/kind_image/image_content.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/host_viewport.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/operator_binding.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/serialize/load_context.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/image_fixtures.hpp"

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

using namespace arbc;
namespace fix = arbc::image::testfix;

namespace {

using arbc::image::ImageContent;

// The DEFERRING `AssetSource` double: `request()` records `(uri, on_ready)` and fires nothing.
// The test decides when the bytes arrive, so the async boundary is crossed on command rather
// than on a timer -- no sleeps, no polling, no flake. Modelled on the one
// `runtime.async_external_load` drives (`async_external_load.t.cpp:80-137`), which is the
// point: assets and compositions go through one seam and now share one deferral substrate.
class DeferringAssetSource final : public AssetSource {
public:
  void put(std::string uri, std::string bytes) {
    d_files.insert_or_assign(std::move(uri), std::move(bytes));
  }

  void request(std::string_view resolved_uri,
               std::function<void(std::string_view)> on_ready) override {
    ++d_requests;
    d_outstanding.push_back(Request{std::string(resolved_uri), std::move(on_ready)});
  }

  // Deliver every request outstanding RIGHT NOW, `times` times over, and return how many
  // requests were fired. `times > 1` is the DUPLICATE-ARRIVAL probe: a source that answers
  // twice for one URI must install once and publish one revision.
  std::size_t fire_all(std::size_t times = 1) {
    std::vector<Request> firing;
    firing.swap(d_outstanding);
    for (std::size_t n = 0; n < times; ++n) {
      for (const Request& r : firing) {
        deliver(r);
      }
    }
    return firing.size();
  }

  // The dedup witness across the async boundary (doc 08 Principle 3): two authored spellings of
  // one file must cost ONE fetch, whether or not its bytes have come back yet.
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
// UNAVAILABLE and always was (Constraint 1). Its existence beside the deferring double is the
// two-outcomes-become-three split, asserted from both sides.
class AbsentAssetSource final : public AssetSource {
public:
  void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
    ++d_requests;
    on_ready(std::string_view{});
  }
  std::size_t requests() const noexcept { return d_requests; }

private:
  std::size_t d_requests{0};
};

// Records each `flush` a commit makes, so "the arrival's damage rides the install's OWN commit"
// is a checkable statement about WHICH FLUSH CARRIED WHAT, not a hope.
class RecordingDamageSink final : public DamageSink {
public:
  void flush(const std::vector<Damage>& damage) override { d_flushes.push_back(damage); }
  const std::vector<std::vector<Damage>>& flushes() const noexcept { return d_flushes; }

private:
  std::vector<std::vector<Damage>> d_flushes;
};

// The `Registry` a host has after `arbc_plugin_register` ran, assembled by linking the impl
// archive instead of dlopening the MODULE.
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

// One `org.arbc.image` layer per authored reference. Hand-rolled rather than canonical: nothing
// here compares document BYTES (that is the golden's job), so the writer's normalization is
// irrelevant and the reader's leniency is the point.
std::string image_doc(const std::vector<std::string>& sources) {
  std::string layers;
  for (const std::string& source : sources) {
    if (!layers.empty()) {
      layers += ",";
    }
    layers +=
        R"({"kind":"org.arbc.image","kind_version":"1","params":{"source":")" + source + R"("}})";
  }
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,384,320],"layers":[)" + layers +
         "]}}";
}

// A document nesting `ref` through org.arbc.nested -- the outer half of the two-round settle.
std::string nesting_doc(std::string_view ref) {
  std::string layer = R"({"kind":"org.arbc.nested","kind_version":"1","params":{"ref":")";
  layer += ref;
  layer += R"("}})";
  return R"({"arbc":{"format":1},"composition":{"canvas":[0,0,384,320],"layers":[)" + layer + "]}}";
}

ObjectId root_composition_of(const Document& doc) {
  const DocStatePtr pin = doc.pin();
  ObjectId root;
  const CompositionRecord* rec = nullptr;
  REQUIRE(pin->find_first_composition(root, rec));
  return root;
}

// Every `ImageContent` in the document, with its `ObjectId`, in id order -- the images may live
// in the root composition or in a settled external child, so this walks the content map rather
// than one composition's layers.
std::vector<std::pair<ObjectId, ImageContent*>> images_in(const Document& doc) {
  std::vector<std::pair<ObjectId, ImageContent*>> out;
  doc.for_each_content([&out](ObjectId id, Content* c) {
    if (auto* const image = dynamic_cast<ImageContent*>(c); image != nullptr) {
      out.emplace_back(id, image);
    }
  });
  // `ObjectId` is deliberately only equality-comparable, so order by its underlying value: this
  // is a test-local canonical ordering, not a claim that ids are ordered.
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first.value < b.first.value; });
  return out;
}

std::pair<ObjectId, ImageContent*> only_image(const Document& doc) {
  const std::vector<std::pair<ObjectId, ImageContent*>> all = images_in(doc);
  REQUIRE(all.size() == 1);
  return all[0];
}

// Render the root composition through the live engine -- a real backend, a real pull service, a
// real binding scope. Used only to prove the pending state does not FAULT; the byte-exact oracle
// comparison is the golden's job.
void render_root(Document& doc, int dim) {
  CpuBackend backend;
  TileCache cache(64U * 1024 * 1024);
  const DocStatePtr pin = doc.pin();
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  PullConfig config;
  config.id_of = make_pull_identity_of(*pin, resolve);
  const std::uint64_t revision = pin->revision();
  config.contribution = [revision](const Content*) { return revision; };
  PullServiceImpl service(cache, backend, direct_dispatch(), config);
  register_builtin_operator_binders();
  OperatorBindingScope scope = bind_operators(doc, service, backend, pin);
  const expected<std::unique_ptr<Surface>, SurfaceError> target =
      backend.make_surface(dim, dim, pin->working_space());
  REQUIRE(target.has_value());
  SurfacePool pool(backend);
  const Viewport viewport{dim, dim, Affine::identity()};
  render_frame(*pin, resolve, viewport, backend, pool, **target);
  scope.release();
}

bool names(const std::vector<Damage>& set, ObjectId object) {
  return std::any_of(set.begin(), set.end(),
                     [object](const Damage& d) { return d.object == object; });
}

// The flushes that named `object`, so "exactly one flush carried it" is assertable.
std::vector<const std::vector<Damage>*> flushes_naming(const RecordingDamageSink& sink,
                                                       ObjectId object) {
  std::vector<const std::vector<Damage>*> out;
  for (const std::vector<Damage>& flush : sink.flushes()) {
    if (names(flush, object)) {
      out.push_back(&flush);
    }
  }
  return out;
}

std::uint64_t decodes() { return arbc::image::default_pyramid_cache().decodes_issued(); }

// A constant clock: the free-run transport advances by zero flicks every step, so a still,
// undamaged scene is genuinely still and no assertion below reads the wall clock.
HostViewport::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// The whole host side of a Document-bound viewport, in one object
// (runtime.host_viewport_document_binding). Everything the frame needs -- the resolver, the
// damage-sink install, the external-arrival settle hook -- is derived by the constructor from
// `doc` and `binding`. There is no `settle_external_loads` call anywhere in the test that uses
// it: `step()` is the only thing the host drives, which is Decision 8's whole claim.
class DocumentViewport {
public:
  DocumentViewport(Document& doc, KindBridge& bridge, const Registry& registry, int dim)
      : d_cache(64U * 1024 * 1024), d_pool(d_backend),
        d_target(d_backend.make_surface(dim, dim, doc.pin()->working_space())),
        d_viewport(d_renderer, doc, HostViewport::DocumentBinding{&bridge, &registry}, d_backend,
                   d_pool, d_cache, checked(d_target), epoch_clock(), config(doc, dim)) {}

  HostViewport* operator->() noexcept { return &d_viewport; }

private:
  using Target = expected<std::unique_ptr<Surface>, SurfaceError>;

  static Surface& checked(Target& target) {
    REQUIRE(target.has_value());
    return **target;
  }

  static HostViewport::Config config(const Document& doc, int dim) {
    HostViewport::Config cfg;
    cfg.viewport = Viewport{dim, dim, Affine::identity(), root_composition_of(doc)};
    return cfg;
  }

  CpuBackend d_backend;
  TileCache d_cache;
  SurfacePool d_pool;
  Target d_target;
  InteractiveRenderer d_renderer{{}, epoch_clock()};
  HostViewport d_viewport;
};

} // namespace

// enforces: 08-serialization#pending-asset-installs-live
TEST_CASE("a pending image leaves a live, extent-less layer at revision 0") {
  // The state doc 08 now names, asserted directly. A source that has not answered YET is not a
  // source that answered ABSENCE: the content is minted in the unavailable SHAPE -- which is why
  // this costs the kind nothing, `kinds.image` Decision 7 having already made an extent-less
  // image render nothing -- and the parent document finishes loading at revision 0 without
  // waiting on any fetch.
  DeferringAssetSource source;
  source.put("live0/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  const std::uint64_t before = decodes();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                        "live0/project.arbc", &source)
              .has_value());

  // Constraint 2: the load SUCCEEDED and did not wait. A document whose every image is pending
  // lands at exactly the baseline a photograph-free one does.
  CHECK(doc.pin()->revision() == 0);

  const auto [id, image] = only_image(doc);
  CHECK(id.valid());
  CHECK_FALSE(image->available());
  CHECK(image->external_asset_ref() == "assets/photo.ppm"); // the AUTHORED spelling survives
  REQUIRE(image->bounds().has_value());
  CHECK(image->bounds()->empty()); // EMPTY (Rect{}), never nullopt -- nullopt means UNBOUNDED

  // The fetch was ISSUED (it is in flight), and it has not answered. THIS is what separates
  // pending from unavailable -- not the bytes, which are empty in both.
  CHECK(source.requests() == 1);
  CHECK(source.outstanding() == 1);
  CHECK(doc.pending_external_loads() == 1);

  // Nothing was decoded: there are no bytes to decode yet.
  CHECK(decodes() == before);

  // And it renders -- nothing, without faulting.
  REQUIRE_NOTHROW(render_root(doc, 64));
}

// enforces: 08-serialization#pending-asset-installs-live
// enforces: 14-data-model-and-editing#commit-publishes-once
// enforces: 14-data-model-and-editing#damage-flushes-once-per-commit
TEST_CASE("settling an image arrival installs the pyramid on a NEW revision and damages it") {
  DeferringAssetSource source;
  source.put("live1/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                        "live1/project.arbc", &source)
              .has_value());
  const ObjectId root_before = root_composition_of(doc);
  const auto [id, image] = only_image(doc);

  // Subscribe to the model's damage exactly as a host frame loop does (doc 02:51-52).
  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);

  const std::uint64_t before = decodes();
  CHECK(source.fire_all() == 1);                            // the bytes arrive ...
  CHECK(settle_external_loads(doc, bridge, registry) == 1); // ... and land
  CHECK(doc.pending_external_loads() == 0);

  // A NEW revision, not a republished baseline: the arrival is an ordinary transaction, so the
  // document's own root id and its own layers survive untouched.
  CHECK(doc.pin()->revision() > 0);
  CHECK(root_composition_of(doc) == root_before);
  const auto [id_after, image_after] = only_image(doc);
  CHECK(id_after == id);       // the SAME content, not a swapped-in replacement ...
  CHECK(image_after == image); // ... the very same object, in fact
  CHECK(image->available());   // ... which now has pixels ...
  CHECK(image->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height}); // ... and geometry

  // EXACTLY ONE decode across the whole pending-then-settled life of the image -- not two (a
  // throwaway at construction plus a real one at install), which is what an install that
  // re-decoded rather than re-keying the cache would have cost.
  CHECK(decodes() == before + 1);

  // Decision 5: the damage naming the image content rides the install's OWN commit. It is
  // flushed EXACTLY ONCE across the whole settle -- a second commit carrying it would publish an
  // intermediate revision whose damage set was empty despite structurally changing what the
  // frame must draw -- and it carries the absorbing, structural shape, because the content's
  // bounds were EMPTY and are now not, so the region that changed is not expressible in the old
  // geometry at all (Decision 4).
  const std::vector<const std::vector<Damage>*> carrying = flushes_naming(sink, id);
  REQUIRE(carrying.size() == 1);
  for (const Damage& d : *carrying[0]) {
    if (d.object == id) {
      CHECK(d.rect == Rect::infinite());
      CHECK(d.range == TimeRange::all());
    }
  }

  doc.set_damage_sink(nullptr);
}

// enforces: 08-serialization#pending-asset-is-not-unavailable
// enforces: 08-serialization#unavailable-asset-is-not-a-read-error
TEST_CASE("a source that ANSWERED is unavailable, not pending -- empty bytes, or no source") {
  const Registry registry = image_registry();
  const std::string bytes = image_doc({"assets/photo.ppm"});

  SECTION("a source that answers INLINE with empty bytes") {
    // It answered. That is unavailable, and always was: the bytes being empty is not what makes
    // it so -- the ANSWER is (Constraint 1).
    AbsentAssetSource source;
    Document doc;
    KindBridge bridge;
    REQUIRE(
        load_document(bytes, doc, bridge, registry, "absent/project.arbc", &source).has_value());

    const auto [id, image] = only_image(doc);
    CHECK_FALSE(image->available());
    CHECK(image->bounds()->empty());
    CHECK(source.requests() == 1);
    CHECK(doc.pending_external_loads() == 0); // NOTHING is pending: the question was answered

    // And a settle installs nothing and publishes no revision -- there is nothing to settle.
    CHECK(settle_external_loads(doc, bridge, registry) == 0);
    CHECK(doc.pin()->revision() == 0);
  }

  SECTION("no AssetSource installed at all") {
    // `load_context.cpp:99-104` fires the continuation INLINE with empty bytes when no source is
    // installed. So "no source" ANSWERS, and stays unavailable -- it never becomes pending, which
    // is the degenerate case the whole probe has to get right.
    Document doc;
    KindBridge bridge;
    REQUIRE(load_document(bytes, doc, bridge, registry, {}, nullptr).has_value());

    const auto [id, image] = only_image(doc);
    CHECK_FALSE(image->available());
    CHECK(doc.pending_external_loads() == 0);
    CHECK(settle_external_loads(doc, bridge, registry) == 0);
    CHECK(doc.pin()->revision() == 0);
  }
}

// enforces: 08-serialization#pending-asset-is-not-unavailable
TEST_CASE("a DEFERRED arrival carrying empty bytes settles to unavailable, for free") {
  // The source deferred -- so the content was PENDING, correctly -- and then answered ABSENCE.
  // The outcome is unavailable, and it costs exactly one map erase: nothing observable changed,
  // so no transaction opens, no revision publishes and no damage flushes (Constraint 9).
  DeferringAssetSource source; // deliberately: nothing `put`, so the arrival carries no bytes

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                        "empty/project.arbc", &source)
              .has_value());
  CHECK(doc.pending_external_loads() == 1); // it deferred, so it was genuinely pending

  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);

  const std::uint64_t before = decodes();
  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 0); // arrived, installed nothing

  const auto [id, image] = only_image(doc);
  CHECK_FALSE(image->available());
  CHECK(image->bounds()->empty());
  CHECK(doc.pending_external_loads() == 0); // no longer waiting: the source answered
  CHECK(doc.pin()->revision() == 0);        // NO revision
  CHECK(sink.flushes().empty());            // NO damage
  CHECK(decodes() == before);               // and nothing to decode

  doc.set_damage_sink(nullptr);
}

// enforces: 08-serialization#pending-asset-is-not-unavailable
TEST_CASE("undecodable arriving bytes settle to unavailable, as a VALUE") {
  DeferringAssetSource source;
  source.put("garbage/assets/photo.ppm", "P6 this is not an image at all");

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                        "garbage/project.arbc", &source)
              .has_value());

  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);

  const std::uint64_t before = decodes();
  CHECK(source.fire_all() == 1);
  // No throw crossed the plugin boundary: `install_asset` reported the failure as `false`
  // (Constraint 11), which is exactly the unavailable arrival -- free, per Constraint 9.
  std::size_t settled = 1;
  REQUIRE_NOTHROW(settled = settle_external_loads(doc, bridge, registry));
  CHECK(settled == 0);

  const auto [id, image] = only_image(doc);
  CHECK_FALSE(image->available());
  CHECK(image->bounds()->empty());
  // ... and `install_asset` says so directly, on the content itself.
  CHECK_FALSE(image->install_asset("P6 this is not an image at all"));
  CHECK(doc.pending_external_loads() == 0);
  CHECK(doc.pin()->revision() == 0);
  CHECK(sink.flushes().empty());

  // `decodes_issued()` counts DECODES, not decode ATTEMPTS: `PyramidCache::resolve` bumps it
  // only when a pyramid genuinely comes out (`image_content.cpp:146`), which is what makes it a
  // meaningful witness for `#image-decodes-once-per-resolved-uri`. Garbage bytes were fed to the
  // decoder and rejected, so the counter is unmoved -- the attempt's witness is the `false`
  // above, not this.
  CHECK(decodes() == before);

  doc.set_damage_sink(nullptr);
}

// enforces: 03-layer-plugin-interface#image-decodes-once-per-resolved-uri
TEST_CASE("two authored spellings of one URI share one deferred fetch and one decode") {
  // The property `#image-decodes-once-per-resolved-uri` already promises, extended across the
  // async boundary -- which is where a naive PER-CONTENT fetch would have broken it: two
  // contents deferring on one file would have issued two requests, taken two arrivals and paid
  // two decodes. Dedup therefore lives on the FETCH (Decision 2).
  DeferringAssetSource source;
  source.put("dedup/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm", "assets/./photo.ppm"}), doc, bridge,
                        registry, "dedup/project.arbc", &source)
              .has_value());

  // ONE request in flight for the two layers, and ONE pending entry -- not two.
  CHECK(source.requests() == 1);
  CHECK(doc.pending_external_loads() == 1);

  const std::vector<std::pair<ObjectId, ImageContent*>> before_settle = images_in(doc);
  REQUIRE(before_settle.size() == 2);

  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);

  const std::uint64_t before = decodes();
  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1); // ONE arrival settles BOTH
  CHECK(doc.pending_external_loads() == 0);

  const std::vector<std::pair<ObjectId, ImageContent*>> images = images_in(doc);
  REQUIRE(images.size() == 2);
  for (const auto& [id, image] : images) {
    REQUIRE(image->available());
    CHECK(image->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});
  }
  // ... and they genuinely SHARE the one pyramid, which is what makes the counter below mean
  // "one decode" rather than "two decodes, one discarded".
  CHECK(images[0].second->pyramid().get() == images[1].second->pyramid().get());
  CHECK(decodes() == before + 1);

  // The single arrival's single damage flush names BOTH content ids: each image is itself the
  // damaged object, and the entry's awaiting list is the damage route (Decision 4).
  REQUIRE(sink.flushes().size() == 1);
  CHECK(names(sink.flushes()[0], images[0].first));
  CHECK(names(sink.flushes()[0], images[1].first));

  doc.set_damage_sink(nullptr);
}

// enforces: 08-serialization#pending-asset-installs-live
TEST_CASE("a source that answers TWICE for one URI installs once and publishes one revision") {
  DeferringAssetSource source;
  source.put("twice/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                        "twice/project.arbc", &source)
              .has_value());
  const auto [id, image] = only_image(doc);

  RecordingDamageSink sink;
  doc.set_damage_sink(&sink);

  const std::uint64_t before = decodes();
  CHECK(source.fire_all(2) == 1); // one request, answered twice: TWO arrivals in the queue

  // The second arrival finds nothing pending (`take_pending_asset` already dropped the entry),
  // so it installs into nobody and opens no transaction. And even had it reached the content,
  // `install_asset` publishes at most once and never replaces a published pyramid (Decision 6).
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(image->available());

  const std::uint64_t revision = doc.pin()->revision();
  CHECK(revision == 1);              // ONE revision, not two
  CHECK(sink.flushes().size() == 1); // ONE damage flush, not two
  CHECK(decodes() == before + 1);    // ONE decode

  // The pyramid survives a redundant install of the very same bytes, unreplaced.
  const arbc::image::PyramidPtr published = image->pyramid();
  CHECK(image->install_asset(fix::fixture_bytes()));
  CHECK(image->pyramid().get() == published.get());
  CHECK(doc.pin()->revision() == revision);

  doc.set_damage_sink(nullptr);
}

// enforces: 08-serialization#pending-asset-is-not-unavailable
TEST_CASE("an arrival firing after the Document is destroyed installs nothing and faults nothing") {
  // A network fetch outlives the document that started it -- and it MUST be safe, because
  // nothing can stop a user from closing a project mid-download. The `on_ready` closure captures
  // a `weak_ptr` and nothing else: no `Document`, no `Model`, no `Content`, no loader. When the
  // queue has expired it drops its bytes and returns. Runs in the ASan lane.
  DeferringAssetSource source;
  source.put("teardown/assets/photo.ppm", fix::fixture_bytes());

  const Registry registry = image_registry();
  {
    Document doc;
    KindBridge bridge;
    REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                          "teardown/project.arbc", &source)
                .has_value());
    CHECK(doc.pending_external_loads() == 1);
  } // the Document -- and with it the only owner of the queue -- is gone

  std::size_t fired = 0;
  REQUIRE_NOTHROW(fired = source.fire_all()); // the bytes land on a dead queue, harmlessly
  CHECK(fired == 1);
}

// enforces: 08-serialization#pending-asset-installs-live
TEST_CASE("a pending image inside a pending nested child settles across two rounds") {
  // The one place the two arrival kinds interleave in the shared queue -- and the reason they
  // SHARE one (Decision 2). The image's fetch cannot even be ISSUED until the child's bytes are
  // parsed, so the graph needs two settle rounds: an asset arrival and a composition arrival,
  // one queue, one settle loop, one counter.
  DeferringAssetSource source;
  source.put("chain/child.arbc", image_doc({"photo.ppm"}));
  source.put("chain/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(
      load_document(nesting_doc("child.arbc"), doc, bridge, registry, "chain/parent.arbc", &source)
          .has_value());

  // Round 0: the CHILD is pending. The image inside it is not even known to exist yet.
  CHECK(doc.pending_external_loads() == 1);
  CHECK(images_in(doc).empty());

  const std::uint64_t before = decodes();

  // Round 1: the child's bytes land and parse. Parsing them MINTS the image content and issues
  // its fetch -- which defers, so the document is pending again, on a different arrival kind.
  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 1);
  const auto [id, image] = only_image(doc);
  CHECK_FALSE(image->available());
  CHECK(source.outstanding() == 1); // the image's fetch, in flight

  // Round 2: the image's bytes land.
  CHECK(source.fire_all() == 1);
  CHECK(settle_external_loads(doc, bridge, registry) == 1);
  CHECK(doc.pending_external_loads() == 0); // quiescent: both landed
  CHECK(image->available());
  CHECK(image->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});
  CHECK(decodes() == before + 1);
}

// enforces: 01-core-concepts#viewport-binds-to-document
// enforces: 02-architecture#async-arrival-emits-damage
TEST_CASE("a deferred image settles inside the very frame that observes it") {
  // Decision 8: NO new driver. `settle_external_loads` gained an asset arm inside its existing
  // loop, and `HostViewport::step()` already calls it at step 0 -- before the pin and before the
  // damage drain -- with `derive_document_config` auto-wiring the hook from the `Document`. So an
  // image arrival settles inside the frame that observes it, with ZERO new wiring. There is no
  // `settle_external_loads` call in this test: `step()` is the only thing driven.
  DeferringAssetSource source;
  source.put("frame/assets/photo.ppm", fix::fixture_bytes());

  Document doc;
  KindBridge bridge;
  const Registry registry = image_registry();
  REQUIRE(load_document(image_doc({"assets/photo.ppm"}), doc, bridge, registry,
                        "frame/project.arbc", &source)
              .has_value());
  const auto [id, image] = only_image(doc);
  CHECK(doc.pending_external_loads() == 1);

  register_builtin_operator_binders();
  DocumentViewport viewport(doc, bridge, registry, 64);

  viewport->step(); // a frame BEFORE the bytes arrive: nothing settles, nothing faults
  CHECK(viewport->external_loads_settled() == 0);
  CHECK(doc.pending_external_loads() == 1);
  CHECK_FALSE(image->available());

  CHECK(source.fire_all() == 1);

  viewport->step(); // the frame that OBSERVES the arrival is the frame that SETTLES it
  CHECK(viewport->external_loads_settled() == 1);
  CHECK(doc.pending_external_loads() == 0);
  CHECK(image->available());
  CHECK(doc.pin()->revision() > 0);
  CHECK(image->bounds() == Rect{0.0, 0.0, fix::k_width, fix::k_height});
}
