#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/cache/key_shapes.hpp>       // BlockKey, std::hash<BlockKey>
#include <arbc/compositor/pull_service.hpp> // BlockCache, AudioBlockValue
#include <arbc/contract/content.hpp>        // Spatialization, spatial_context_digest
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/audio_worker_pool.hpp>
#include <arbc/runtime/lookahead_pump.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

// Confirming golden + counters for the SPATIAL block-cache-key disambiguation
// (audio.spatial_blockkey_disambiguation, doc 12:249-254). The spatial-agnostic block key
// `(content, revision, block index, rate)` collides two DISTINCT spatial contexts over the
// same content on one cache slot: a nested composition's `render_audio` output depends on
// the listener (pan/attenuation of its children), so two embeddings of one nested
// composition at different positions but the same time map -- identical
// `(content, revision, block index, rate)`, different composed listener -- warm/pull the
// FIRST embedding's block for BOTH, diverging from a fresh per-embedding render.
//
// This file drives that exact scene through the real LookaheadPump + AudioWorkerPool
// sharing one BlockCache and asserts the primed-ring drain is byte-identical (no tolerance)
// to a direct `mix_composition` oracle rendering each embedding under its own context, for
// worker_count 0 and 4. A second case pins the two-monitor variant (two monitors of
// differing listener over one shared cache). PRE-FIX (before the trailing
// `spatial_digest` field folded from `spatial_context_digest`), the two contributions key
// IDENTICALLY -- `BlockKey{c_nest, rev, idx, rate}` for both -- so the second context reads
// the first's block and the drain equals neither oracle: the confirmed collision. Post-fix
// the digest folds each per-edge `Spatialization` into a distinct key, so each embedding
// hits its own slot (write-side warm digest == read-side pull digest -> zero dispatch),
// and the drain equals the per-context oracle. Cross-component (kind_nested + kind_tone +
// compositor + runtime), so it lives here and links `arbc`.

namespace {

using namespace arbc;

std::int64_t block_index_of(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  return fpf != 0 ? request.window.start.flicks / fpf : 0;
}

// The mix-pass / inner-pull `PullService`: cache-first over the REAL compositor `BlockCache`,
// rendering inline on a miss, counting dispatches (misses). Mirrors the real
// `PullServiceImpl::pull_audio` read key (pull_service.cpp:301) -- INCLUDING the trailing
// spatial-context digest -- so this read key equals the write-side warm key
// `contribution_key` builds under the same per-edge context. Two distinct contexts over the
// same content therefore probe two distinct slots (the disambiguation under test).
class CachingPull final : public PullService {
public:
  CachingPull(BlockCache* blocks, std::function<ObjectId(const Content*)> id_of,
              std::uint64_t revision)
      : d_blocks(blocks), d_id_of(std::move(id_of)), d_revision(revision) {}
  void pull(ContentRef, const RenderRequest&, std::shared_ptr<RenderCompletion> done) override {
    done->fail(RenderError::ContentFailed);
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    if (d_blocks != nullptr) {
      const BlockKey key{d_id_of ? d_id_of(input) : ObjectId{}, d_revision, block_index_of(request),
                         request.sample_rate, spatial_context_digest(request.spatial)};
      if (std::optional<CacheHold<AudioBlockValue>> hit = d_blocks->lookup(key);
          hit.has_value() && hit->get().meta.exact &&
          hit->get().meta.achieved_rate == request.sample_rate &&
          hit->get().frames == request.target.frames &&
          hit->get().layout == request.target.layout) {
        const AudioBlockValue& value = hit->get();
        const std::size_t n = static_cast<std::size_t>(value.frames) * channel_count(value.layout);
        if (request.target.samples != nullptr && value.samples.size() >= n) {
          for (std::size_t i = 0; i < n; ++i) {
            request.target.samples[i] = value.samples[i];
          }
        }
        done->complete(value.meta);
        return; // resident exact-fresh hit: zero dispatch
      }
    }
    d_dispatches.fetch_add(1, std::memory_order_acq_rel);
    AudioFacet* af = input != nullptr ? input->audio() : nullptr;
    if (af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> r = af->render_audio(request, done);
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }
  std::uint64_t dispatches() const { return d_dispatches.load(std::memory_order_acquire); }

private:
  BlockCache* d_blocks;
  std::function<ObjectId(const Content*)> d_id_of;
  std::uint64_t d_revision;
  std::atomic<std::uint64_t> d_dispatches{0};
};

bool bytes_equal(const std::vector<float>& a, const std::vector<float>& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

NestedResolver map_resolver(const std::unordered_map<ObjectId, Content*>& binding) {
  return [&binding](ObjectId id) -> Content* {
    const auto it = binding.find(id);
    return it != binding.end() ? it->second : nullptr;
  };
}

constexpr std::uint32_t k_rate = 48'000;
constexpr std::uint32_t k_block_frames = 32;
constexpr double k_view = 100.0;

// The direct Spatial mix oracle for one output block: mix the root composition with a
// no-cache inline pull carrying the monitor's `Spatialization` context, then apply the
// camera post-scale by `accum_atten` exactly as `LookaheadRing::mix_block` does. `pull`
// has no cache, so each nested embedding renders FRESH under its own composed listener --
// the correct per-embedding reference. Byte-identical to the ring's worker_count==0 drain
// by construction; the threaded drain must match it too once each context keys its own slot.
std::vector<float> spatial_direct_mix(const DocRoot& doc, ObjectId root,
                                      const MixResolver& resolve, PullService& pull,
                                      std::int64_t index, const Spatialization& seed) {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;
  const std::int64_t t0 = index * span;
  std::vector<float> buf(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
  AudioBlock block{buf.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
  const AudioRequest req{TimeRange{Time{t0}, Time{t0 + span}},
                         k_rate,
                         ChannelLayout::Stereo,
                         block,
                         Exactness::BestEffort,
                         StateHandle{},
                         seed};
  mix_composition(doc, root, resolve, pull, req, MixPolicy::Spatial);
  for (float& s : buf) {
    s *= seed.accum_atten; // the camera's uniform scale-attenuation (mix_block post-scale)
  }
  return buf;
}

// A root composition embedding ONE nested composition (c_inner, an off-center child tone)
// TWICE, as two layers at DIFFERENT pan positions but the same (identity) time map. Both
// layers resolve the same content id `c_nest`, so both contributions land on the identical
// `(content, revision, block index, rate)` -- and differ ONLY in composed listener. That is
// the collision the spatial digest disambiguates. `tf_a`/`tf_b` are pure translations
// (max_scale == 1 => edge_atten == 1, so neither is culled and both share accum_atten),
// isolating the difference to the pan position.
struct TwoEmbeddingScene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  ToneContent child{770, 0.5F};

  static Affine tf_a() { return Affine::translation(k_view / 4.0, 0.0); }
  static Affine tf_b() { return Affine::translation(k_view / 2.0, 0.0); }

  TwoEmbeddingScene() {
    auto tx = model.transact("spatial two-embedding of one nested composition");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId c_child = tx.add_content(1);
    // The child sits off-center inside its composition, so its constant-power pan is
    // asymmetric and each embedding's composed child position differs.
    tx.attach_layer(c_inner, tx.add_layer(c_child, Affine::translation(k_view / 4.0, 0.0)));
    c_root = tx.add_composition(0.0, 0.0);
    c_nest = tx.add_content(1);
    // The SAME nested content, embedded twice at two positions but one time map: identical
    // (content, revision, block index, rate), different composed listener.
    tx.attach_layer(c_root, tx.add_layer(c_nest, tf_a()));
    tx.attach_layer(c_root, tx.add_layer(c_nest, tf_b()));
    tx.commit();
    binding[c_child] = &child;
    ids[&child] = c_child;
  }

  std::function<ObjectId(const Content*)> id_of() {
    return [this](const Content* c) {
      const auto it = ids.find(c);
      return it != ids.end() ? it->second : ObjectId{};
    };
  }
  std::function<std::optional<ObjectId>(ObjectId)> nesting() const {
    const ObjectId nest = c_nest;
    const ObjectId inner = c_inner;
    return [nest, inner](ObjectId content) -> std::optional<ObjectId> {
      return content == nest ? std::optional<ObjectId>(inner) : std::nullopt;
    };
  }
};

// A single-embedding nested scene (root -> one nested contributor -> off-center child),
// for the two-monitor variant: two monitors of DIFFERING listener over this one content
// share one BlockCache. Each monitor's per-edge contribution digest differs, so neither
// monitor reads the other's warmed block.
struct OneEmbeddingScene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  ToneContent child{770, 0.5F};

  OneEmbeddingScene() {
    auto tx = model.transact("spatial one-embedding nested for two-monitor share");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId c_child = tx.add_content(1);
    tx.attach_layer(c_inner, tx.add_layer(c_child, Affine::translation(k_view / 4.0, 0.0)));
    c_root = tx.add_composition(0.0, 0.0);
    c_nest = tx.add_content(1);
    tx.attach_layer(c_root, tx.add_layer(c_nest, Affine::translation(k_view / 2.0, 0.0)));
    tx.commit();
    binding[c_child] = &child;
    ids[&child] = c_child;
  }

  std::function<ObjectId(const Content*)> id_of() {
    return [this](const Content* c) {
      const auto it = ids.find(c);
      return it != ids.end() ? it->second : ObjectId{};
    };
  }
  std::function<std::optional<ObjectId>(ObjectId)> nesting() const {
    const ObjectId nest = c_nest;
    const ObjectId inner = c_inner;
    return [nest, inner](ObjectId content) -> std::optional<ObjectId> {
      return content == nest ? std::optional<ObjectId>(inner) : std::nullopt;
    };
  }
};

} // namespace

// enforces: 12-audio#block-key-disambiguates-spatial-context
// enforces: 12-audio#spatial-warms-nested-with-pull-context
TEST_CASE("two embeddings of one nested composition drain per-context, not one shared block") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  const Spatialization seed{Affine::identity(), k_view, k_view, 1.0F, k_sub_audible_atten};

  // Key-level proof of the disambiguation (Constraint 3, Decision D3): the two embeddings'
  // per-edge root contributions carry the SAME (content, revision, block index, rate) but
  // composed listeners that differ (compose(identity, tf_a) vs compose(identity, tf_b)),
  // so their spatial digests differ -- distinct keys, distinct slots. Zeroing the digest
  // (the pre-fix key) makes the two keys EQUAL: the confirmed collision.
  const Spatialization ctx_a{compose(seed.listener, TwoEmbeddingScene::tf_a()), k_view, k_view,
                             seed.accum_atten * spatial_edge_atten(TwoEmbeddingScene::tf_a()),
                             seed.sub_audible};
  const Spatialization ctx_b{compose(seed.listener, TwoEmbeddingScene::tf_b()), k_view, k_view,
                             seed.accum_atten * spatial_edge_atten(TwoEmbeddingScene::tf_b()),
                             seed.sub_audible};
  const std::uint64_t digest_a = spatial_context_digest(ctx_a);
  const std::uint64_t digest_b = spatial_context_digest(ctx_b);
  REQUIRE(digest_a != 0);
  REQUIRE(digest_b != 0);
  REQUIRE(digest_a != digest_b);
  const ObjectId nest_id{1}; // placeholder id for the key-shape witness only
  REQUIRE(BlockKey{nest_id, 0, 0, k_rate, digest_a} != BlockKey{nest_id, 0, 0, k_rate, digest_b});
  REQUIRE(BlockKey{nest_id, 0, 0, k_rate, 0} == BlockKey{nest_id, 0, 0, k_rate, 0}); // pre-fix collision

  for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
    TwoEmbeddingScene scene;
    const DocStatePtr doc = scene.model.current();

    BlockCache blocks{64u * 1024 * 1024};
    CachingPull pull(&blocks, scene.id_of(), doc->revision());

    CpuBackend backend;
    NestedContent nested(scene.c_inner);
    nested.attach(pull, backend, map_resolver(scene.binding), *doc);
    scene.binding[scene.c_nest] = &nested;
    scene.ids[&nested] = scene.c_nest;

    LookaheadRingConfig ringcfg;
    ringcfg.composition = scene.c_root;
    ringcfg.resolve = map_resolver(scene.binding);
    ringcfg.sample_rate = k_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = k_block_frames;
    ringcfg.revision = doc->revision();
    ringcfg.policy = MixPolicy::Spatial;
    ringcfg.spatial = seed;
    ringcfg.nested_composition = scene.nesting();
    LookaheadRing ring(*doc, pull, ringcfg);

    AudioWorkerPoolConfig poolcfg;
    poolcfg.worker_count = worker_count;
    poolcfg.serialize_predicate = [&nested](const Content* c) { return c == &nested; };
    AudioWorkerPool pool(poolcfg);

    std::atomic<std::uint64_t> fake_tick{0};
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{2 * span}; // blocks 0..2
    pumpcfg.resolve = map_resolver(scene.binding);
    pumpcfg.sample_rate = k_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [&fake_tick] {
      return fake_tick.fetch_add(1, std::memory_order_relaxed);
    };
    pumpcfg.playhead_source = [] { return Time::zero(); };
    LookaheadPump pump(ring, blocks, pool, pumpcfg);

    pump.flush();
    pump.flush();

    // Residency held for BOTH digest-distinct slots: every mixer pull hits (write-side warm
    // digest == read-side pull digest), so the closure warmed to zero dispatch (Constraint 2).
    REQUIRE(pull.dispatches() == 0);
    // Settle-once over the shared cache: the pool warmed each distinct-key want exactly once
    // and completed every one -- two distinct nested slots were warmed (not one reused).
    REQUIRE(pool.tasks_completed() == pool.tasks_submitted());
    // Two digest-distinct nested slots + their two (leaf) children = 4 contributors per output
    // block, over 3 output blocks (horizon 2*span): 12 warm tasks. The pre-digest single-slot
    // path collapses each embedding pair to one key -> 2 contributors/block -> 6 (half), the
    // behavioral witness of "two distinct cache slots, two renders, not one reused".
    REQUIRE(pool.tasks_submitted() == 4u * 3u);

    CachingPull oracle_pull(nullptr, {}, 0); // no cache: render each embedding fresh, per-context
    for (std::int64_t i = 0; i < 3; ++i) {
      std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
      AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
      AudioResult meta{};
      REQUIRE(pump.drain(i, out, meta));

      // The drain equals the fresh per-embedding oracle: each embedding rendered under its
      // own composed listener, NOT one shared block served for both. Pre-fix (digest absent)
      // both embeddings shared one slot, so `got` matched neither embedding's context and
      // this assertion failed -- the confirmed collision this task closes.
      const std::vector<float> want =
          spatial_direct_mix(*doc, scene.c_root, map_resolver(scene.binding), oracle_pull, i, seed);
      REQUIRE(bytes_equal(got, want));
    }

    REQUIRE(ring.silence_mixed() == 0);
    pump.request_stop();
  }
}

// enforces: 12-audio#block-key-disambiguates-spatial-context
TEST_CASE("two monitors of differing listener over one shared cache each drain their own oracle") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // Two monitors over the SAME content, differing only in the listener transform. Sharing one
  // BlockCache, their per-edge contribution digests differ (D4), so neither reads the other's
  // warmed block; pre-fix both keyed the nested contributor identically and monitor B read
  // monitor A's block.
  const Spatialization seed_a{Affine::identity(), k_view, k_view, 1.0F, k_sub_audible_atten};
  const Spatialization seed_b{Affine::translation(k_view / 3.0, 0.0), k_view, k_view, 1.0F,
                              k_sub_audible_atten};
  REQUIRE(spatial_context_digest(seed_a) != spatial_context_digest(seed_b));

  OneEmbeddingScene scene;
  const DocStatePtr doc = scene.model.current();

  BlockCache blocks{64u * 1024 * 1024}; // ONE cache shared by both monitors
  CachingPull pull(&blocks, scene.id_of(), doc->revision());

  CpuBackend backend;
  NestedContent nested(scene.c_inner);
  nested.attach(pull, backend, map_resolver(scene.binding), *doc);
  scene.binding[scene.c_nest] = &nested;
  scene.ids[&nested] = scene.c_nest;

  auto make_pump = [&](LookaheadRing& ring, AudioWorkerPool& pool,
                       std::atomic<std::uint64_t>& tick) {
    LookaheadPumpConfig pumpcfg;
    pumpcfg.horizon = Time{2 * span};
    pumpcfg.resolve = map_resolver(scene.binding);
    pumpcfg.sample_rate = k_rate;
    pumpcfg.layout = ChannelLayout::Stereo;
    pumpcfg.block_frames = k_block_frames;
    pumpcfg.tick_period = std::chrono::hours(1);
    pumpcfg.tick_source = [&tick] { return tick.fetch_add(1, std::memory_order_relaxed); };
    pumpcfg.playhead_source = [] { return Time::zero(); };
    return std::make_unique<LookaheadPump>(ring, blocks, pool, pumpcfg);
  };

  auto make_ringcfg = [&](const Spatialization& seed) {
    LookaheadRingConfig ringcfg;
    ringcfg.composition = scene.c_root;
    ringcfg.resolve = map_resolver(scene.binding);
    ringcfg.sample_rate = k_rate;
    ringcfg.layout = ChannelLayout::Stereo;
    ringcfg.block_frames = k_block_frames;
    ringcfg.revision = doc->revision();
    ringcfg.policy = MixPolicy::Spatial;
    ringcfg.spatial = seed;
    ringcfg.nested_composition = scene.nesting();
    return ringcfg;
  };

  AudioWorkerPoolConfig poolcfg;
  poolcfg.worker_count = 4;
  poolcfg.serialize_predicate = [&nested](const Content* c) { return c == &nested; };
  AudioWorkerPool pool_a(poolcfg);
  AudioWorkerPool pool_b(poolcfg);

  LookaheadRing ring_a(*doc, pull, make_ringcfg(seed_a));
  LookaheadRing ring_b(*doc, pull, make_ringcfg(seed_b));
  std::atomic<std::uint64_t> tick_a{0};
  std::atomic<std::uint64_t> tick_b{0};
  auto pump_a = make_pump(ring_a, pool_a, tick_a);
  auto pump_b = make_pump(ring_b, pool_b, tick_b);

  pump_a->flush();
  pump_a->flush();
  pump_b->flush();
  pump_b->flush();

  // Both monitors' closures warmed into their own digest-distinct slots: every pull hits.
  REQUIRE(pull.dispatches() == 0);

  CachingPull oracle_pull(nullptr, {}, 0);
  for (std::int64_t i = 0; i < 3; ++i) {
    std::vector<float> got_a(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    std::vector<float> got_b(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
    AudioBlock out_a{got_a.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioBlock out_b{got_b.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
    AudioResult meta_a{};
    AudioResult meta_b{};
    REQUIRE(pump_a->drain(i, out_a, meta_a));
    REQUIRE(pump_b->drain(i, out_b, meta_b));

    const std::vector<float> want_a =
        spatial_direct_mix(*doc, scene.c_root, map_resolver(scene.binding), oracle_pull, i, seed_a);
    const std::vector<float> want_b =
        spatial_direct_mix(*doc, scene.c_root, map_resolver(scene.binding), oracle_pull, i, seed_b);
    REQUIRE(bytes_equal(got_a, want_a));
    REQUIRE(bytes_equal(got_b, want_b));
    // The two monitors genuinely differ: a shared slot would have served one block for both.
    REQUIRE_FALSE(bytes_equal(got_a, got_b));
  }

  REQUIRE(ring_a.silence_mixed() == 0);
  REQUIRE(ring_b.silence_mixed() == 0);
  pump_a->request_stop();
  pump_b->request_stop();
}

// enforces: 12-audio#block-key-disambiguates-spatial-context
TEST_CASE("spatial_context_digest reduces the whole Spatialization; BlockKey folds it") {
  const Spatialization base{Affine::identity(), 100.0, 80.0, 0.75F, k_sub_audible_atten};

  // Determinism: the same context always yields the same digest (Constraint 5).
  REQUIRE(spatial_context_digest(base) == spatial_context_digest(base));
  // Zero exactly when Flat; present is nonzero (the zero-when-Flat invariant, Constraint 1).
  REQUIRE(spatial_context_digest(std::optional<Spatialization>{}) == 0);
  REQUIRE(spatial_context_digest(std::optional<Spatialization>{base}) == spatial_context_digest(base));
  REQUIRE(spatial_context_digest(base) != 0);

  // A one-field perturbation in EACH of the five fields changes the digest (Constraint 3):
  // the digest is over the whole struct, an over-key, never an under-key.
  Spatialization listener_moved = base;
  listener_moved.listener.tx += 1.0;
  REQUIRE(spatial_context_digest(listener_moved) != spatial_context_digest(base));

  Spatialization listener_scaled = base;
  listener_scaled.listener.a += 0.5; // a distinct coefficient of the 6-coefficient Affine
  REQUIRE(spatial_context_digest(listener_scaled) != spatial_context_digest(base));

  Spatialization vw = base;
  vw.viewport_w += 1.0;
  REQUIRE(spatial_context_digest(vw) != spatial_context_digest(base));

  Spatialization vh = base;
  vh.viewport_h += 1.0;
  REQUIRE(spatial_context_digest(vh) != spatial_context_digest(base));

  Spatialization atten = base;
  atten.accum_atten *= 0.5F;
  REQUIRE(spatial_context_digest(atten) != spatial_context_digest(base));

  Spatialization sub = base;
  sub.sub_audible *= 0.5F;
  REQUIRE(spatial_context_digest(sub) != spatial_context_digest(base));

  // BlockKey equality/hash consistency: equal keys hash equal; a digest-only difference makes
  // keys unequal and disperses the hash (the field participates in `==` and `std::hash`).
  const std::hash<BlockKey> hash{};
  const BlockKey k0{ObjectId{7}, 3, 42, k_rate, 0};
  const BlockKey k0_same{ObjectId{7}, 3, 42, k_rate, 0};
  const BlockKey kd{ObjectId{7}, 3, 42, k_rate, spatial_context_digest(base)};
  REQUIRE(k0 == k0_same);
  REQUIRE(hash(k0) == hash(k0_same));
  REQUIRE(k0 != kd);            // a digest-only difference makes the keys distinct
  REQUIRE(hash(k0) != hash(kd)); // ...and lands them in different buckets
}
