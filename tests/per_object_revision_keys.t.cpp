// PER-OBJECT REVISION KEYS, end to end through the SHIPPED drivers
// (`model.per_object_revision`, doc 01:155-165, doc 05:129-144).
//
// Doc 01 promises the cache key is `(content identity, revision, region, quantized
// scale)` where the revision is the CONTENT's, not the document's. Until this task the
// interactive planner handed the document-global revision to every layer, so one edit
// anywhere made every layer's cached tiles unreachable -- and doc 05's caching promise
// was enforced only against a test stub that supplied its own per-node contribution
// (`nested_cache.t.cpp`), while every shipped driver passed the global revision. These
// cases enforce the promise against the PRODUCT: a real `InteractiveRenderer`, using its
// own `PullConfig::contribution`.
//
// Behavioral counters throughout (doc 16:54-62) -- `requests_issued`, never wall-clock.

#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>
#include <arbc/cache/keyed_store.hpp>
#include <arbc/compositor/compositor.hpp>
#include <arbc/compositor/counters.hpp>
#include <arbc/compositor/pull_service.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/damage.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/interactive.hpp>
#include <arbc/runtime/pull_identity.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace arbc;

namespace {

constexpr int k_dim = 512; // a 2x2 grid of rung-0 tiles at scale 1.0
constexpr auto k_budget = std::chrono::milliseconds(16);

// A dab: a finite damage rect entirely inside the top-left rung-0 tile, so exactly ONE
// tile of the 2x2 grid is re-planned per commit -- a brush stroke's shape.
constexpr Rect k_dab{16.0, 16.0, 64.0, 64.0};

Rect canvas() { return Rect{0.0, 0.0, static_cast<double>(k_dim), static_cast<double>(k_dim)}; }
// The frame walk is composition-scoped, so a viewport anchors at the composition it draws
// (compositor.root_composition_frame_walk, doc 05:28-36).
Viewport viewport(ObjectId anchor) { return Viewport{k_dim, k_dim, Affine::identity(), anchor}; }

// The fake clock puts every deadline instant in the real past, so no frame blocks and
// the loop is deterministic (doc 16:54-62 -- never a wall-clock assertion).
InteractiveRenderer::Clock epoch_clock() {
  return [] { return std::chrono::steady_clock::time_point{}; };
}

// The key revision the shipped driver plans `content` under: its per-object revision
// contribution, projected through the very seams `InteractiveRenderer` uses
// (`refresh_identity_memo` -> `build_pull_stamp_map` -> `pull_contribution_of`). The test
// composes the product's own projection rather than re-deriving it, so a change to the
// projection cannot pass here while breaking the driver.
std::uint64_t key_revision(const Document& doc, const DocRoot& state, ObjectId content) {
  const ContentResolver resolve = [&doc](ObjectId id) { return doc.resolve(id); };
  const auto ids = build_pull_identity_map(state, resolve);
  const auto stamps = build_pull_stamp_map(state, *ids);
  return pull_contribution_of(ids, stamps)(doc.resolve(content));
}

// How many tiles are resident under `content`'s cache identity.
std::size_t tiles_for(TileCache& cache, ObjectId content) {
  std::size_t live = 0;
  cache.remove_if([&](const TileKey& key) {
    if (key.content == content) {
      ++live;
    }
    return false; // remove nothing
  });
  return live;
}

// One brush dab: re-stamp the painted content and publish the dab's damage. This is what
// a raster paint commit is -- the content's record is path-copied (minting its new
// stamp), and the commit carries the touched rect. The layers it does NOT touch are not
// path-copied and keep their stamps, which is the whole mechanism under test.
void dab(Document& doc, ObjectId content) {
  auto txn = doc.transact("dab");
  txn.set_content_state(content, StateHandle{});
  txn.add_damage(Damage{content, k_dab, TimeRange::all()});
  REQUIRE(txn.commit().has_value());
}

std::vector<float> snapshot(const Surface& surface) {
  const std::span<const float> px = std::as_const(surface).span<PixelFormat::Rgba32fLinearPremul>();
  return {px.begin(), px.end()};
}

bool byte_identical(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// A three-layer scene: one painted layer plus two that the stroke never touches.
struct StrokeScene {
  Document doc;
  ObjectId root{}; // the composition the frame walk anchors at
  ObjectId painted{};
  ObjectId still_a{};
  ObjectId still_b{};

  StrokeScene() {
    root = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    painted =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.5F, 0.2F, 0.1F, 1.0F}, canvas()));
    still_a =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.1F, 0.6F, 0.2F, 0.5F}, canvas()));
    still_b =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.2F, 0.1F, 0.7F, 0.5F}, canvas()));
    doc.attach_layer(root, doc.add_layer(painted, Affine::identity()));
    doc.attach_layer(root, doc.add_layer(still_a, Affine::identity()));
    doc.attach_layer(root, doc.add_layer(still_b, Affine::identity()));
  }
};

} // namespace

// enforces: 14-data-model-and-editing#stroke-does-not-orphan-the-viewport
TEST_CASE("a 60-dab stroke leaves every unedited layer's tiles keyed, cached, and un-rendered") {
  CpuBackend backend;
  StrokeScene scene;
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  InteractiveRenderer renderer({}, epoch_clock());
  auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  // --- Frame 1: cold. All three layers render their whole 2x2 grid. -----------
  DocStatePtr pin = scene.doc.pin();
  renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target, {},
                        Time{0}, k_budget);
  REQUIRE(renderer.counters().requests_issued() == 12U); // 3 layers x 4 tiles
  REQUIRE(tiles_for(cache, scene.still_a) == 4U);
  REQUIRE(tiles_for(cache, scene.still_b) == 4U);

  // The keys the still layers' tiles live under, as of the pre-stroke version.
  const std::uint64_t a_key = key_revision(scene.doc, *pin, scene.still_a);
  const std::uint64_t b_key = key_revision(scene.doc, *pin, scene.still_b);
  const std::uint64_t revision_before = pin->revision();

  // --- The stroke: 60 commits, one transaction per dab. ------------------------
  // Coalescing does not help here and never could: a coalesced commit still publishes a
  // new revision (#coalesced-commits-merge-to-one-entry says so outright), so a
  // document-global key orphans the whole tile cache 60 times over.
  const std::uint64_t requests_before = renderer.counters().requests_issued();
  constexpr int k_dabs = 60;
  for (int i = 0; i < k_dabs; ++i) {
    dab(scene.doc, scene.painted);
    pin = scene.doc.pin();
    const std::vector<Damage> damage{Damage{scene.painted, k_dab, TimeRange::all()}};
    renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target,
                          damage, Time{0}, k_budget);
  }

  // The document revision moved 60 times...
  CHECK(pin->revision() == revision_before + k_dabs);
  // ...and the unedited layers' KEYS did not move at all. This is the mechanism: their
  // records were never path-copied, so they kept their stamps by structural sharing.
  CHECK(key_revision(scene.doc, *pin, scene.still_a) == a_key);
  CHECK(key_revision(scene.doc, *pin, scene.still_b) == b_key);

  // ...so their tiles were never orphaned: the same four each, still resident, still
  // reachable. Under a document-global key every one of these would be sitting behind a
  // dead key -- present in the store, findable by nothing.
  CHECK(tiles_for(cache, scene.still_a) == 4U);
  CHECK(tiles_for(cache, scene.still_b) == 4U);

  // THE HEADLINE COUNTER. Each dab frame re-plans exactly the damaged tile of each of the
  // three layers. The painted layer's is a genuine miss (its stamp moved, and its damage
  // dropped the old tile) -- one render. The two still layers' are FRESH HITS: their keys
  // did not move, so their tiles are exactly where the plan looks. 60 dabs -> 60 renders.
  //
  // Under the document-global key the same 60 frames issue 180: every dab re-keys all
  // three layers, so all three miss on the dab tile, every frame. The factor of three IS
  // the over-invalidation this task removes -- and it is the small half of it, because the
  // eight tiles OUTSIDE the dab are left cached under a dead key too, so the first pan or
  // zoom after the stroke re-renders the entire viewport cold.
  CHECK(renderer.counters().requests_issued() - requests_before == k_dabs);

  // The pan. Sub-tile, so the covered tile set is unchanged. A camera edit is a
  // device-mapping delta, so the frame plans the WHOLE viewport (02-architecture § "A
  // camera change is device damage") -- every tile of all three layers is probed, not
  // just the dab's. The still layers' keys did not move, so all EIGHT of their tiles are
  // fresh hits: ZERO dispatched renders. The painted layer's stamp moved with the
  // carried dab, so its full 2x2 grid misses -- every render the pan dispatches is owed
  // by the edited layer alone. Under the document-global key the same pan re-renders the
  // entire viewport cold: twelve.
  const std::uint64_t before_pan = renderer.counters().requests_issued();
  dab(scene.doc, scene.painted);
  pin = scene.doc.pin();
  const Viewport panned{k_dim, k_dim, Affine::translation(-8.0, -4.0), scene.root};
  const std::vector<Damage> damage{Damage{scene.painted, k_dab, TimeRange::all()}};
  renderer.render_frame(*pin, resolve, panned, cache, backend, pool, **target, damage, Time{0},
                        k_budget);
  CHECK(renderer.counters().requests_issued() - before_pan == 4U);
  // ...and the still layers' tiles survived the pan un-orphaned: the camera change
  // repainted without invalidating anything.
  CHECK(tiles_for(cache, scene.still_a) == 4U);
  CHECK(tiles_for(cache, scene.still_b) == 4U);
}

namespace {

// A parent composition holding ONE nested layer over a child composition of two solids.
// The child's ARRANGEMENT -- its layer order, and each member layer's transform -- is what
// Decision 5 exists for: none of it is a `Content`, so none of it is reachable from the
// compositor's `inputs()` fold, and none of it moves any child CONTENT's stamp.
struct NestedScene {
  Document doc;
  ObjectId root{}; // the PARENT composition the frame walk anchors at
  ObjectId child{};
  ObjectId nested_id{};
  ObjectId lower_layer{}; // child's layer 0
  ObjectId upper_layer{}; // child's layer 1
  ObjectId unrelated{};   // a sibling leaf layer in the PARENT, edited to be "unrelated"
  std::shared_ptr<NestedContent> nested;

  explicit NestedScene(bool reversed_order) {
    // The parent composition the frame walk anchors at holds the nested layer and the
    // unrelated sibling; the child composition holds the two solids the nested content
    // shows (compositor.root_composition_frame_walk, doc 05:28-36).
    root = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    child = doc.add_composition(static_cast<double>(k_dim), static_cast<double>(k_dim));
    const ObjectId lower_c =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.60F, 0.10F, 0.10F, 1.00F}, canvas()));
    const ObjectId upper_c = doc.add_content(std::make_shared<SolidContent>(
        Rgba{0.05F, 0.35F, 0.60F, 0.50F}, Rect{0.0, 0.0, 256.0, 256.0}));
    lower_layer = doc.add_layer(lower_c, Affine::identity());
    upper_layer = doc.add_layer(upper_c, Affine::identity());
    // The two authorings differ ONLY in the child's membership order -- same contents,
    // same transforms, same everything else. The upper solid is translucent, so
    // bottom-to-top order is visible in the pixels.
    if (reversed_order) {
      doc.attach_layer(child, upper_layer);
      doc.attach_layer(child, lower_layer);
    } else {
      doc.attach_layer(child, lower_layer);
      doc.attach_layer(child, upper_layer);
    }
    nested = std::make_shared<NestedContent>(child);
    nested_id = doc.add_content(nested);
    doc.attach_layer(root, doc.add_layer(nested_id, Affine::identity()));

    unrelated =
        doc.add_content(std::make_shared<SolidContent>(Rgba{0.02F, 0.02F, 0.02F, 0.10F}, canvas()));
    doc.attach_layer(root, doc.add_layer(unrelated, Affine::identity()));
  }
};

// Render `scene` to quiescence with a fresh renderer + cold cache, and return the pixels.
std::vector<float> cold_pixels(Backend& backend, NestedScene& scene) {
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  InteractiveRenderer renderer({}, epoch_clock());
  auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());
  const DocStatePtr pin = scene.doc.pin();
  const FrameBinding binding{&scene.doc, pin};
  renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target, {},
                        Time{0}, k_budget, binding);
  return snapshot(**target);
}

} // namespace

// enforces: 05-recursive-composition#composition-arrangement-joins-the-contribution
TEST_CASE("a nested child's arrangement edit re-renders the parent's composite, at the pixel") {
  // THE GOLDEN. The arrangement fold's failure mode is a STALE PIXEL, not a slow frame, so
  // it is pinned byte-exactly: after editing the child's arrangement in a WARM cache, the
  // parent's composed result must equal the same scene authored fresh from cold.
  //
  // Without the fold the parent's composed-result key is unchanged by these edits -- no
  // child CONTENT's stamp moved, and the `inputs()` fold sees only contents -- so the
  // cache serves the PRE-EDIT composite and these comparisons fail on the pixels.
  CpuBackend backend;

  SECTION("a layer REORDER of the child") {
    NestedScene warm(/*reversed_order=*/false);
    const ContentResolver resolve = [&warm](ObjectId id) { return warm.doc.resolve(id); };
    SurfacePool pool(backend);
    TileCache cache(64U * 1024 * 1024);
    InteractiveRenderer renderer({}, epoch_clock());
    auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
    REQUIRE(target.has_value());

    // Frame 1 warms the parent's composed-result tiles under the pre-reorder key.
    DocStatePtr pin = warm.doc.pin();
    renderer.render_frame(*pin, resolve, viewport(warm.root), cache, backend, pool, **target, {},
                          Time{0}, k_budget, FrameBinding{&warm.doc, pin});
    const std::uint64_t key_before = key_revision(warm.doc, *pin, warm.nested_id);

    // Reorder the child: swap its two members. This touches the CHILD COMPOSITION's record
    // -- and nothing else. No content anywhere is path-copied, so no content's stamp moves.
    {
      auto txn = warm.doc.transact("reorder");
      txn.reorder_layer(warm.child, 0, 1);
      REQUIRE(txn.commit().has_value());
    }
    pin = warm.doc.pin();

    // The embedder's key MOVED, because its contribution folds the arrangement of the
    // composition it names (Decision 5). This is the assertion the whole decision rests on.
    CHECK(key_revision(warm.doc, *pin, warm.nested_id) != key_before);

    // Re-render, damaging the nested layer so the frame re-plans it. The composed-result
    // tiles it looks up are keyed by the NEW arrangement, so the cache cannot hand back
    // the pre-reorder composite.
    const std::vector<Damage> damage{Damage{warm.nested_id, Rect::infinite(), TimeRange::all()}};
    renderer.render_frame(*pin, resolve, viewport(warm.root), cache, backend, pool, **target,
                          damage, Time{0}, k_budget, FrameBinding{&warm.doc, pin});

    NestedScene fresh(/*reversed_order=*/true);
    CHECK(byte_identical(snapshot(**target), cold_pixels(backend, fresh)));
  }

  SECTION("a member layer's TRANSFORM nudge inside the child") {
    NestedScene warm(/*reversed_order=*/false);
    const ContentResolver resolve = [&warm](ObjectId id) { return warm.doc.resolve(id); };
    SurfacePool pool(backend);
    TileCache cache(64U * 1024 * 1024);
    InteractiveRenderer renderer({}, epoch_clock());
    auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
    REQUIRE(target.has_value());

    DocStatePtr pin = warm.doc.pin();
    renderer.render_frame(*pin, resolve, viewport(warm.root), cache, backend, pool, **target, {},
                          Time{0}, k_budget, FrameBinding{&warm.doc, pin});
    const std::uint64_t key_before = key_revision(warm.doc, *pin, warm.nested_id);

    // Nudge one member LAYER's transform. `LayerRecord` is not a `Content` and carries no
    // back-pointer to its composition, so this edit is invisible to every content stamp in
    // the document; only the member walk in `composition_revision` can see it.
    const Affine nudged = Affine::translation(64.0, 32.0);
    warm.doc.set_layer_transform(warm.upper_layer, nudged);
    pin = warm.doc.pin();
    CHECK(key_revision(warm.doc, *pin, warm.nested_id) != key_before);

    const std::vector<Damage> damage{Damage{warm.nested_id, Rect::infinite(), TimeRange::all()}};
    renderer.render_frame(*pin, resolve, viewport(warm.root), cache, backend, pool, **target,
                          damage, Time{0}, k_budget, FrameBinding{&warm.doc, pin});

    NestedScene fresh(/*reversed_order=*/false);
    fresh.doc.set_layer_transform(fresh.upper_layer, nudged);
    CHECK(byte_identical(snapshot(**target), cold_pixels(backend, fresh)));
  }
}

// enforces: 05-recursive-composition#static-subtree-served-from-cache
TEST_CASE("a static nested subtree survives an unrelated edit through the SHIPPED driver") {
  // The registered claim reads "survives a clock advance AND AN UNRELATED EDIT (zero
  // dispatched renders)". Until this task that clause was true only of `nested_cache.t.cpp`'s
  // stub, which supplies its own hand-written per-node contribution, while every shipped
  // driver passed the document-global revision for every node -- under which an unrelated
  // edit re-keys the nested subtree and re-renders all of it. This case is the claim
  // enforced against the PRODUCT: a real `InteractiveRenderer`, using its own
  // `PullConfig::contribution`.
  CpuBackend backend;
  NestedScene scene(/*reversed_order=*/false);
  const ContentResolver resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  SurfacePool pool(backend);
  TileCache cache(64U * 1024 * 1024);
  InteractiveRenderer renderer({}, epoch_clock());
  auto target = backend.make_surface(k_dim, k_dim, k_working_rgba32f);
  REQUIRE(target.has_value());

  DocStatePtr pin = scene.doc.pin();
  renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target, {},
                        Time{0}, k_budget, FrameBinding{&scene.doc, pin});
  const std::uint64_t nested_key = key_revision(scene.doc, *pin, scene.nested_id);
  const std::size_t nested_tiles = tiles_for(cache, scene.nested_id);
  REQUIRE(nested_tiles > 0U);

  // THE UNRELATED EDIT: re-stamp a different layer's content, and damage only that content.
  // Nothing in the nested subtree is touched -- not the child composition, not its member
  // layers, not any child content.
  const std::uint64_t requests_before = renderer.counters().requests_issued();
  dab(scene.doc, scene.unrelated);
  pin = scene.doc.pin();
  REQUIRE(pin->revision() > 1U);

  const std::vector<Damage> damage{Damage{scene.unrelated, k_dab, TimeRange::all()}};
  renderer.render_frame(*pin, resolve, viewport(scene.root), cache, backend, pool, **target, damage,
                        Time{0}, k_budget, FrameBinding{&scene.doc, pin});

  // The nested subtree's composed-result key did not move, its tiles were not orphaned, and
  // NOT ONE of them was re-rendered. The only render the frame owed was the edited layer's
  // single dab tile.
  CHECK(key_revision(scene.doc, *pin, scene.nested_id) == nested_key);
  CHECK(tiles_for(cache, scene.nested_id) == nested_tiles);
  CHECK(renderer.counters().requests_issued() - requests_before == 1U);
}

// ---------------------------------------------------------------------------------
// The AUDIO half (constraint 6). The lookahead ring warms blocks under a key it computes
// ITSELF, and `PullServiceImpl::pull_audio` probes a key it computes independently. Under
// a document-global revision the two agreed by coincidence -- both were the same scalar.
// Under per-object stamps they agree only if they read the SAME contribution map, and if
// they did not, the ring would warm keys nobody probes: every pull would miss, the ring
// would become pure waste, and `12-audio#block-key-disambiguates-spatial-context`'s
// residency clause would break. So the ring takes the functor rather than a number.

namespace {

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_block_frames = 32;

// Byte-exact procedural sine over an EXACT integer flick phase (never std::sin) -- the
// same discipline as the audio-engine's own doubles.
float parab_sine(std::int64_t t_flicks, std::uint32_t freq_hz, float amp) {
  const std::int64_t fps = Time::flicks_per_second;
  std::int64_t t = t_flicks % fps;
  if (t < 0) {
    t += fps;
  }
  const std::int64_t r = (static_cast<std::int64_t>(freq_hz) * t) % fps;
  double p = 2.0 * (static_cast<double>(r) / static_cast<double>(fps));
  if (p > 1.0) {
    p -= 2.0;
  }
  const double abs_p = p < 0.0 ? -p : p;
  return static_cast<float>(static_cast<double>(amp) * (4.0 * p * (1.0 - abs_p)));
}

class SineLeaf final : public Content {
public:
  SineLeaf(std::uint32_t freq_hz, float amp) : d_facet(freq_hz, amp) {}
  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest&,
                                     std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
    return std::nullopt;
  }
  AudioFacet* audio() override { return &d_facet; }

private:
  class Facet final : public AudioFacet {
  public:
    Facet(std::uint32_t freq_hz, float amp) : d_freq(freq_hz), d_amp(amp) {}
    std::optional<TimeRange> audio_extent() const override { return std::nullopt; }
    Stability audio_stability() const override { return Stability::Static; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t ch = channel_count(request.layout);
      const std::int64_t fpf =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const float v = parab_sine(request.window.start.flicks + static_cast<std::int64_t>(f) * fpf,
                                   d_freq, d_amp);
        for (std::uint32_t c = 0; c < ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * ch + c] = v;
        }
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    std::uint32_t d_freq;
    float d_amp;
  };
  Facet d_facet;
};

// Two audible tone layers in one composition: one edited, one left alone.
struct AudioScene {
  Document doc;
  ObjectId comp{};
  ObjectId edited{};
  ObjectId untouched{};

  AudioScene() {
    comp = doc.add_composition(0.0, 0.0);
    edited = doc.add_content(std::make_shared<SineLeaf>(300, 0.6F));
    untouched = doc.add_content(std::make_shared<SineLeaf>(700, 0.4F));
    doc.attach_layer(comp, doc.add_layer(edited, Affine::identity()));
    doc.attach_layer(comp, doc.add_layer(untouched, Affine::identity()));
  }
};

// Every block key the ring warms this pass, by contributor. The ring's WRITE-SIDE key.
std::vector<PrefetchWant> ring_wants(const AudioScene& scene, const DocRoot& state,
                                     PullService& pull, BlockCache& blocks) {
  const auto resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
  const auto ids = build_pull_identity_map(state, resolve);
  const auto stamps = build_pull_stamp_map(state, *ids);

  LookaheadRingConfig cfg;
  cfg.composition = scene.comp;
  cfg.resolve = resolve;
  cfg.sample_rate = k_rate;
  cfg.layout = ChannelLayout::Stereo;
  cfg.block_frames = k_block_frames;
  // THE POINT: the ring reads the very map `PullConfig::contribution` reads. Equal by
  // construction, not by coincidence.
  cfg.contribution = object_contribution_of(stamps);
  LookaheadRing ring(state, pull, cfg);
  return ring.prime(&blocks, Time{0}, Time{0}, /*direction=*/1);
}

} // namespace

// Constraint 6 of `model.per_object_revision`. It takes no `enforces:` of its own: the
// registered audio claims (`12-audio#block-cache-is-tile-cache-1d`,
// `#block-key-disambiguates-spatial-context`) are unchanged in MEANING by this task, and
// this case guards the invariant that keeps them true once the revision slot goes
// per-object -- the write-side warm key and the read-side probe key are the same key.
TEST_CASE("the audio ring's warm key equals the pull's probe key under per-object stamps") {
  CpuBackend backend;
  AudioScene scene;
  TileCache tiles(16U * 1024 * 1024);
  BlockCache blocks(16U * 1024 * 1024);

  const auto probe_key = [&](const DocRoot& state, ObjectId content, std::int64_t block_index) {
    const auto resolve = [&scene](ObjectId id) { return scene.doc.resolve(id); };
    const auto ids = build_pull_identity_map(state, resolve);
    const auto stamps = build_pull_stamp_map(state, *ids);
    // Exactly what `PullServiceImpl::pull_audio` computes for a leaf input: the content's
    // id, its own contribution, the block index, the rate, and a Flat (0) spatial digest.
    return BlockKey{content, pull_contribution_of(ids, stamps)(scene.doc.resolve(content)),
                    block_index, k_rate, 0};
  };

  DocStatePtr pin = scene.doc.pin();
  PullConfig config;
  config.blocks = &blocks;
  config.audio_dispatch = direct_audio_dispatch();
  PullServiceImpl pull(tiles, backend, direct_dispatch(), config);

  // The ring's warm keys ARE the keys the pull probes -- for every contributor, at every
  // block the pass wants. Wire the two from different maps and this equality is what
  // breaks, silently, into an all-miss ring.
  const std::vector<PrefetchWant> wants = ring_wants(scene, *pin, pull, blocks);
  REQUIRE(wants.size() >= 2U);
  for (const PrefetchWant& w : wants) {
    CHECK(w.key == probe_key(*pin, w.content, w.key.block_index));
  }

  // The keys the two contributors warm at block 0, before any edit.
  const BlockKey edited_before = probe_key(*pin, scene.edited, 0);
  const BlockKey untouched_before = probe_key(*pin, scene.untouched, 0);

  // Edit ONE audible layer's content. Under a document-global key this re-keys BOTH
  // contributors, so the unrelated layer's prepared blocks are orphaned and the ring
  // re-mixes them from scratch every edit.
  dab(scene.doc, scene.edited);
  pin = scene.doc.pin();

  // The edited layer's block key moved; the untouched layer's did NOT -- so its prepared
  // blocks stay resident and reachable, and the ring re-mixes nothing for it.
  CHECK_FALSE(probe_key(*pin, scene.edited, 0) == edited_before);
  CHECK(probe_key(*pin, scene.untouched, 0) == untouched_before);

  // And the ring still agrees with the pull after the edit -- both moved together, or
  // neither did, per contributor.
  for (const PrefetchWant& w : ring_wants(scene, *pin, pull, blocks)) {
    CHECK(w.key == probe_key(*pin, w.content, w.key.block_index));
  }
}
