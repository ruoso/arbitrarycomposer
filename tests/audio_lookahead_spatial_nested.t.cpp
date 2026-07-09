#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/audio_engine/mix.hpp>
#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/compositor/pull_service.hpp> // BlockCache, AudioBlockValue
#include <arbc/contract/content.hpp>
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

// Pump-driven byte-exact golden + counters for the SPATIAL nested-composition lookahead
// fill (audio.spatial_nested_warm_context, doc 12:167-206,210-222). Predecessors proved
// the Flat, nested, below-rate, and Spatial-Droste-cull cases; this file drives a Spatial
// monitor over a scene whose contributor is a NESTED COMPOSITION holding an off-center
// child tone -- so the nested composition's internal Spatial mix (constant-power pan +
// mono-collapse) differs from its Flat mix -- through the real LookaheadPump +
// AudioWorkerPool at worker_count>0, and asserts the drained mixed blocks are byte-
// identical to (a) the worker_count==0 drain and (b) a direct Spatial mix_composition
// oracle. Before the fix the pump warmed the nested contributor FLAT (no spatial context
// on the pump's AudioRequest, lookahead_pump.cpp), so the threaded drain read a Flat-
// warmed nested block where the mixer pulls a Spatial one -- confirmed to fail on the
// pre-fix tree (see the divergence-witness assertion below). Cross-component
// (kind_nested + kind_tone + compositor + runtime), so it lives here and links `arbc`.

namespace {

using namespace arbc;

// The Spatial tone contributors are `org.arbc.tone` leaves (byte-exact `parab_sine` via
// integer-flick phase, √-law pan via IEEE `std::sqrt`), so this file needs no local tone
// generator -- the sibling goldens' `parab_sine` discipline lives inside `ToneContent`.

std::int64_t block_index_of(const AudioRequest& request) {
  if (request.sample_rate == 0) {
    return 0;
  }
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
  return fpf != 0 ? request.window.start.flicks / fpf : 0;
}

// The mix-pass / inner-pull `PullService`: cache-first over the REAL compositor
// `BlockCache`, rendering inline on a miss, counting dispatches (misses). One instance
// serves both the ring's top-level Spatial mix and each nested contributor's inner
// recursive pulls, so a warmed closure is all hits (identical to the recursive golden).
// The BlockKey is spatial-agnostic (`key_shapes.hpp`), so the warmed nested block serves
// the mixer's Spatial pull only when the warm context matched the pull context (the fix).
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
      // Mirror the real PullServiceImpl::pull_audio read key (pull_service.cpp:301): fold the
      // request's Spatial-context digest so this read key equals the write-side warm key
      // `contribution_key` builds under the same per-edge context (Flat digests to 0, a no-op).
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
// no-cache inline pull carrying the monitor's `Spatialization` context (so the nested
// contributor renders Spatial, not Flat), then apply the camera post-scale by
// `accum_atten` exactly as `LookaheadRing::mix_block` does. Byte-identical to the ring's
// worker_count==0 drain by construction; the threaded (worker_count>0) drain must match
// it too once the warm context equals the pull context (the fix under test).
std::vector<float> spatial_direct_mix(const DocRoot& doc, ObjectId root, const MixResolver& resolve,
                                      PullService& pull, std::int64_t index,
                                      const Spatialization& seed) {
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

// The Flat mix oracle for the same scene (no `Spatialization` context): the block the
// pre-fix pump warmed for the nested contributor. Used only to prove the scene actually
// spatializes -- the Spatial drain must NOT equal this, or the test would be vacuous.
std::vector<float> flat_direct_mix(const DocRoot& doc, ObjectId root, const MixResolver& resolve,
                                   PullService& pull, std::int64_t index) {
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
                         StateHandle{}};
  mix_composition(doc, root, resolve, pull, req);
  return buf;
}

// Build the Spatial nested scene: a root composition whose single contributor is a nested
// composition (c_inner) holding one OFF-CENTER child tone. The root layer centers the
// nested contributor (both output channels non-trivial); the child layer is translated
// off-center inside c_inner, so its composed viewport x-position pans asymmetrically --
// the internal Spatial mix (constant-power pan then top-level mono-collapse) differs from
// the Flat mix. `c_inner`/`c_root`/`c_nest` are returned via out-params.
struct SpatialNestedScene {
  Model model;
  std::unordered_map<ObjectId, Content*> binding;
  std::unordered_map<const Content*, ObjectId> ids;
  ObjectId c_inner{};
  ObjectId c_root{};
  ObjectId c_nest{};
  ToneContent child{770, 0.5F};

  SpatialNestedScene() {
    auto tx = model.transact("spatial nested of an off-center tone");
    c_inner = tx.add_composition(0.0, 0.0);
    const ObjectId c_child = tx.add_content(1);
    // The child sits off-center inside its composition (x = 3/4 viewport once composed),
    // so its constant-power pan is asymmetric (gl != gr) -- the Spatial internal mix
    // cannot collapse to the Flat mix.
    tx.attach_layer(c_inner, tx.add_layer(c_child, Affine::translation(k_view / 4.0, 0.0)));
    c_root = tx.add_composition(0.0, 0.0);
    c_nest = tx.add_content(1);
    // The nested contributor is centered at the top (x = viewport/2), so the top pan is
    // equal-power and both output channels carry the nested block.
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

// enforces: 12-audio#spatial-warms-nested-with-pull-context
TEST_CASE("the threaded pump drains a Spatial nested scene byte-identical to the Spatial oracle") {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k_rate);
  const std::int64_t span = static_cast<std::int64_t>(k_block_frames) * fpf;

  // The monitor's Spatial context: identity listener, x-only pan over `k_view`, unit
  // camera attenuation (so the mix_block post-scale is a no-op and the oracle math stays
  // legible), default sub-audible threshold. Present => Spatial (the branch keys off the
  // request's `spatial`, never a policy enum).
  const Spatialization seed{Affine::identity(), k_view, k_view, 1.0F, k_sub_audible_atten};

  for (const std::size_t worker_count : {std::size_t{0}, std::size_t{4}}) {
    SpatialNestedScene scene;
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
    // Serialize the (cache-reading) nested contributor renders so at most one worker
    // touches the single-writer BlockCache at a time; leaf tone renders stay parallel.
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

    // (b) The mixer's pull_audio for the warmed nested contributor is a zero-dispatch
    // cache hit: the whole Spatial closure (leaf tone + nested block) warmed to zero
    // dispatch, proving the warmed nested block is BOTH resident AND the spatially-
    // correct one the mixer wanted (a Flat-warmed block would be a distinct render but
    // the same key -- served as a hit yet WRONG, which the oracle comparison below
    // catches; the zero-dispatch assertion pins the residency half).
    REQUIRE(pull.dispatches() == 0);

    CachingPull oracle_pull(nullptr, {}, 0); // no cache: render every pull inline, Spatial
    for (std::int64_t i = 0; i < 3; ++i) {
      std::vector<float> got(static_cast<std::size_t>(k_block_frames) * 2, 0.0F);
      AudioBlock out{got.data(), k_block_frames, ChannelLayout::Stereo, k_rate};
      AudioResult meta{};
      REQUIRE(pump.drain(i, out, meta));

      // The threaded/inline drain equals the direct Spatial mix_composition oracle: the
      // warmed nested block was rendered under the identical context the mixer pulls it
      // with, so the nested composition's off-center child spatializes on the same
      // footing whether warmed on a worker or rendered inline.
      const std::vector<float> want =
          spatial_direct_mix(*doc, scene.c_root, map_resolver(scene.binding), oracle_pull, i, seed);
      REQUIRE(bytes_equal(got, want));

      // Divergence witness (D4): the scene genuinely spatializes -- the Spatial drain is
      // NOT the Flat mix of the same scene. This is exactly the block the pre-fix pump
      // warmed for the nested contributor; before the fix `got` equaled THIS (a Flat-
      // warmed nested block) and so failed the oracle assertion above. Guards the golden
      // against a degenerate scene where Spatial == Flat.
      const std::vector<float> flat =
          flat_direct_mix(*doc, scene.c_root, map_resolver(scene.binding), oracle_pull, i);
      REQUIRE_FALSE(bytes_equal(want, flat));
    }

    // (a) The transitive-residency gate held: no output block was mixed while a
    // contributor was still absent.
    REQUIRE(ring.silence_mixed() == 0);

    pump.request_stop();
  }
}
