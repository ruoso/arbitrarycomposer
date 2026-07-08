#pragma once

#include <arbc/audio_engine/mix.hpp>  // MixResolver, MixPolicy, mix_composition
#include <arbc/base/ids.hpp>          // ObjectId
#include <arbc/base/time.hpp>         // Time, TimeRange
#include <arbc/cache/key_shapes.hpp>  // BlockKey
#include <arbc/cache/keyed_store.hpp> // KeyedStore, PriorityClass
#include <arbc/cache/prefetch.hpp>    // temporal_prefetch_ring, prime_ring
#include <arbc/media/audio_block.hpp> // AudioBlock, ChannelLayout, channel_count

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The render-ahead lookahead scheduler for `arbc::audio-engine` (L4, doc 17:57
// "lookahead scheduler, block pipeline"). It is the mechanism that makes audio
// *render ahead*, never on a deadline (doc 12:31-34,155-164): a ring of prepared
// *mixed output* blocks keyed by output-block index at the working rate, filled by
// calling its sibling `mix_composition` once per not-yet-prepared block window from
// a playhead out to an injected horizon, flushed and re-primed on a transport
// change, and invalidated by damage inside the lookahead window (doc 12:187-190).
//
// PURE, CLOCK-FREE, THREAD-FREE (refinement Constraint 1). `prime`/`drain`/
// `reprime`/`invalidate` take a `Time` playhead and `Time` horizon BY VALUE -- no
// wall clock, no `Transport`, no thread. Owning a thread, sampling a clock, and
// binding the worker pool are runtime policy (doc 17:84-86), so the pump and the
// audio worker pool that drive this ring live in `arbc::runtime` (L5).
//
// Levelization (doc 17:41,57): `arbc::audio-engine` reaches only `contract`
// (+ transitive `model`/`media`/`base`) and `cache`. The concrete block-cache
// value (`AudioBlockValue`) rides with the concrete `PullService` in `compositor`
// (an L4 peer, `pull_service.hpp:82-93`), which this component may NOT name -- so
// the cache-touching methods (`prime`/`reprime`/`invalidate`) are TEMPLATED over
// the block-cache value type: the runtime pump instantiates them with
// `compositor::AudioBlockValue`, and this header only names `cache::BlockKey` and
// the generic `cache::KeyedStore`. The prepared output ring is the scheduler's own
// bounded buffer (`std::vector<float>`), distinct from the per-content `BlockCache`
// (Decision: "the ring holds prepared mixed output blocks; the block cache holds
// per-content blocks").

namespace arbc {

// The static configuration of a lookahead ring over a pinned document.
struct LookaheadRingConfig {
  // The root composition the ring prepares mixed output for.
  ObjectId composition{};
  // `ObjectId -> Content*` over the pinned document's bindings (as `mix_composition`).
  MixResolver resolve;
  // The working audio format (doc 12, `DocRoot::working_audio_format`).
  std::uint32_t sample_rate{0};
  ChannelLayout layout{ChannelLayout::Stereo};
  // The FIXED output block size in sample frames -- the prepared-block quantum the
  // reserved `pull_service.hpp:98` note leaves to this task. One output block index
  // spans `block_frames` frames of the working rate.
  std::uint32_t block_frames{0};
  // The doc-global revision the per-content `BlockKey`s carry (doc 12:203-208, the
  // audio revision space IS the visual one). The pump passes `DocRoot::revision()`;
  // it must equal `PullConfig::contribution` for every leaf so the fill keys the
  // ring populates are the exact keys `PullServiceImpl::pull_audio` later probes.
  std::uint64_t revision{0};
  // The per-layer contribution policy (only `Flat` implemented, doc 12:127-130).
  MixPolicy policy{MixPolicy::Flat};
  // The declared-latency pre-roll offset seam (`audio.latency`, doc 12:196-199).
  // This task honors `Time::zero()` only; the additive `-latency()` offset drops in
  // here without a signature change.
  Time preroll{Time::zero()};
  // Maps a contributor content id to the child composition it nests, if any
  // (nullopt for a leaf or non-nesting content). The ring is L4 and MUST NOT
  // introspect an L3 `NestedContent` (doc 17:41,57, Constraint 2 / Decision D2),
  // so the recursive descent's structural edge is INJECTED: the runtime pump --
  // which binds the content -- supplies `content id -> child composition id`,
  // mirroring the way `descend` re-expresses `mix_layer`'s single-level cull and
  // the accepted `mix_child_layer` / `mix_layer` duplication. Empty -> no
  // contributor nests (a flat scene), so the transitive fill degenerates to the
  // shipped single-level enumeration byte-for-byte.
  std::function<std::optional<ObjectId>(ObjectId)> nested_composition;
  // The shared recursion-depth budget (doc 05:68-70) the transitive descent honors,
  // never reset per level: the warming walks exactly the tree the mixer's
  // `pull_audio` depth backstop bounds, so a self-referential (Droste) scene
  // terminates here and the pump's bounded round loop converges rather than
  // spinning it (Constraints 4,5, Decision D5).
  std::uint32_t max_depth{64};
};

// A per-contributor, per-output-block fill descriptor: the block the pump must
// render and insert into the `BlockCache` so a subsequent `pull_audio` hits with
// zero dispatch (doc 12:169-190). `key` is the exact `BlockKey`
// `PullServiceImpl::pull_audio` computes for the mixer's child request.
struct PrefetchWant {
  BlockKey key{};
  ObjectId content{};
  TimeRange window{};
  std::uint32_t rate{0};
  ChannelLayout layout{ChannelLayout::Stereo};
  std::uint32_t frames{0};
};

class LookaheadRing {
public:
  LookaheadRing(const DocRoot& doc, PullService& pull, LookaheadRingConfig config);

  LookaheadRing(const LookaheadRing&) = delete;
  LookaheadRing& operator=(const LookaheadRing&) = delete;

  // The output-block index covering time `t` (floored, sign-correct).
  std::int64_t block_index_at(Time t) const;
  // The composition-local start instant of output block `index`.
  Time block_start(std::int64_t index) const;

  // Fill/mix the ring ahead of `playhead` out to `horizon` in the sign of
  // `direction` (+1 forward / -1 reverse). The output-block progression is
  // enumerated with `cache::temporal_prefetch_ring<BlockKey>` (Constraint 4).
  //
  //   * Cache path (`blocks != nullptr`): the full transitive contributor tree is
  //     enumerated (recursive descent through nested compositions + below-rate native
  //     re-requests); each resident closure member is retained `Temporal`, and a block
  //     is mixed only at FULL transitive residency (so a threaded fill never mixes
  //     silence for a not-yet-rendered descendant). The frontier -- a contributor
  //     dispatched only once its own child closure is resident (bottom-up) -- is
  //     returned as the fill want-list for the pump (Constraints 1,3).
  //   * Inline path (`blocks == nullptr`): every not-yet-prepared block is mixed
  //     immediately (the injected `PullService` settles each pull inline), and the
  //     want-list is empty.
  //
  // Renders/inserts nothing through the cache helpers themselves (they only
  // classify/report); the mix flows through `mix_composition` into the prepared
  // output ring, and per-content blocks land in the cache via the pump's worker path.
  template <class BlockValue>
  std::vector<PrefetchWant> prime(KeyedStore<BlockKey, BlockValue>* blocks, Time playhead,
                                  Time horizon, int direction);

  // Hand out prepared block `index`: copies the already-mixed samples + meta into
  // `out` and returns true if ready, else fills `out` with SILENCE, bumps the
  // underrun counter, and returns false. It NEVER calls `mix_composition` /
  // `render_audio`, allocates, or blocks -- the RT-safety invariant the whole
  // design buys (doc 12:31-34,155-164; Constraint 5).
  bool drain(std::int64_t index, AudioBlock& out, AudioResult& meta);

  // Transport change (`seek`/`set_rate`/direction, doc 12:162-164): drop prepared
  // blocks no longer within the new `[playhead, playhead+horizon)` window; blocks
  // whose output window is unchanged are RETAINED, not re-mixed. Re-enumerate the
  // want-list from the new playhead (the re-mix count equals only the count of
  // newly-needed blocks).
  template <class BlockValue>
  std::vector<PrefetchWant> reprime(KeyedStore<BlockKey, BlockValue>* blocks, Time playhead,
                                    Time horizon, int direction);

  // Damage (doc 12:187-190): drop prepared output blocks and cached per-content
  // `BlockKey`s whose window intersects `range`; the next `prime` re-mixes exactly
  // those. Non-overlapping prepared blocks are retained.
  template <class BlockValue>
  void invalidate(KeyedStore<BlockKey, BlockValue>* blocks, TimeRange range);

  // Behavioral counters (doc 16:54-62), wall-clock-free.
  std::uint64_t blocks_mixed() const noexcept { return d_blocks_mixed; }
  std::uint64_t underruns() const noexcept { return d_underruns; }
  // The transitive-residency gate invariant (Constraint 3, Decision D4): the count
  // of output blocks the fill ever mixed while a transitive contributor was still
  // absent -- structurally 0, since `prime` mixes a block only at full transitive
  // residency, so a threaded fill never mixes silence for a not-yet-rendered
  // descendant (doc 12:210-222 delta).
  std::uint64_t silence_mixed() const noexcept { return d_silence_mixed; }
  std::size_t prepared_count() const noexcept { return d_prepared.size(); }
  bool is_prepared(std::int64_t index) const { return d_prepared.count(index) != 0; }

private:
  struct Prepared {
    std::vector<float> samples; // interleaved, block_frames * channel_count(layout)
    AudioResult meta{};
  };
  // One audible/in-span contributor's child request shape for one output block --
  // the projection of `mix_layer`'s cull + child-window computation (the accepted
  // duplication, doc 17:41 Decision, exactly as `mix_layer` re-expresses nested).
  // `children` holds the recursive sub-contributions when `content` is itself a
  // nested composition (empty for a native leaf) -- the transitive closure the
  // mixer walks (Constraint 1).
  struct Contribution {
    ObjectId content{};
    std::uint32_t rate{0};
    ChannelLayout layout{ChannelLayout::Stereo};
    std::uint32_t frames{0};
    Time window_start{};
    std::vector<Contribution> children{};
  };

  std::vector<Contribution> contributions_for(std::int64_t index) const;
  // Recursive descent through the contributor tree (Constraint 1, Decision D1/D2):
  // enumerate `composition`'s audible in-span layers at `request_rate`/`window_start`
  // and, for a layer that itself nests a composition (via `nested_composition`),
  // descend into its children applying the composed rational rate + time map per
  // edge (never accumulated, doc 11 varispeed), bounded by `max_depth`.
  void descend(ObjectId composition, std::uint32_t request_rate, Time window_start,
               std::uint32_t depth, std::vector<Contribution>& out) const;
  BlockKey contribution_key(const Contribution& c) const;
  // The working-rate discovery `PrefetchWant` for a contributor (leaf or nested).
  PrefetchWant make_want(const Contribution& c) const;
  // The below-rate native re-request `PrefetchWant` a contributor's resident
  // `achieved_rate < rate` provokes (mirrors `mix_layer`'s second native pull,
  // `mix.cpp:127-148`); nullopt for a rate-honoring contributor (Constraint 1/4).
  std::optional<PrefetchWant> native_rerequest_want(const Contribution& c,
                                                    std::uint32_t achieved_rate) const;
  void mix_block(std::int64_t index);
  std::vector<std::int64_t> horizon_blocks(Time playhead, Time horizon, int direction) const;

  const DocRoot& d_doc;
  PullService& d_pull;
  LookaheadRingConfig d_config;
  std::int64_t d_block_span_flicks{0}; // block_frames * (flicks_per_second / rate)
  std::unordered_map<std::int64_t, Prepared> d_prepared;
  std::uint64_t d_blocks_mixed{0};
  std::uint64_t d_underruns{0};
  std::uint64_t d_silence_mixed{0};

  // Recursive want-collection over one contributor subtree (Constraint 3, Decision
  // D4). Returns whether the mixer's `pull_audio` for `c` will fully hit (its
  // working-rate discovery block, plus its below-rate native re-request block when
  // one exists, are resident). Appends the frontier wants: a contributor's discovery
  // block is dispatched only once its whole child closure is resident (warming
  // bottom-up, so a worker's inner recursive pull is a cache hit, never silence);
  // its native re-request is discovered lazily once the discovery block is resident.
  // `seen` dedupes keys within one `prime` so each closure member is submitted once.
  template <class BlockValue>
  bool collect_wants(KeyedStore<BlockKey, BlockValue>* blocks, const Contribution& c,
                     std::vector<PrefetchWant>& wants, std::unordered_set<BlockKey>& seen) const;
};

// --- templated cache-touching methods (Value = compositor::AudioBlockValue at the
// runtime call site; a local double in the audio-engine unit test) -----------------

template <class BlockValue>
bool LookaheadRing::collect_wants(KeyedStore<BlockKey, BlockValue>* blocks, const Contribution& c,
                                  std::vector<PrefetchWant>& wants,
                                  std::unordered_set<BlockKey>& seen) const {
  const BlockKey key = contribution_key(c);
  std::optional<CacheHold<BlockValue>> hit = blocks->lookup(key);
  if (!hit.has_value()) {
    // The working-rate discovery block is absent. Enumerate the child closure FIRST
    // (bottom-up), and dispatch THIS contributor only once its whole closure is
    // resident -- so when a worker renders it (its `render_audio` re-pulling each
    // child) every inner pull is a cache hit, never an async miss mixed as silence
    // (Decision D4). A native leaf has no children and is dispatched immediately.
    bool children_resident = true;
    for (const Contribution& child : c.children) {
      const bool r = collect_wants(blocks, child, wants, seen);
      children_resident = children_resident && r;
    }
    if (children_resident && seen.insert(key).second) {
      wants.push_back(make_want(c));
    }
    return false; // discovery block missing -> the mixer's pull for `c` will not hit
  }
  // Discovery block resident: keep it warm on the temporal prefetch ring (the
  // retention the shipped fill got from `prime_ring`, doc 12:180-190). A resident
  // hit short-circuits any re-render, so the child closure is no longer load-bearing.
  blocks->reclassify(key, PriorityClass::Temporal);
  const std::uint32_t achieved = hit->get().meta.achieved_rate;
  // Below-rate native re-request (Decision D3): discovered lazily from the now-
  // resident `achieved_rate` (a render RESULT, unknowable at enumeration time). The
  // mixer re-pulls a below-rate child at its native rate and band-limit-resamples it
  // (`mix.cpp:115-162`); that distinct `BlockKey` must be resident too, or the
  // threaded mix diverges from the inline oracle.
  const std::optional<PrefetchWant> nw = native_rerequest_want(c, achieved);
  if (nw.has_value()) {
    if (blocks->lookup(nw->key).has_value()) {
      blocks->reclassify(nw->key, PriorityClass::Temporal);
    } else {
      if (seen.insert(nw->key).second) {
        wants.push_back(*nw);
      }
      return false; // native re-request not yet resident -> not fully warm
    }
  }
  return true;
}

template <class BlockValue>
std::vector<PrefetchWant> LookaheadRing::prime(KeyedStore<BlockKey, BlockValue>* blocks,
                                               Time playhead, Time horizon, int direction) {
  std::vector<PrefetchWant> wants;
  std::unordered_set<BlockKey> seen; // dedupe closure keys across the horizon's blocks
  const std::vector<std::int64_t> indices = horizon_blocks(playhead, horizon, direction);
  for (const std::int64_t bi : indices) {
    bool all_resident = true;
    if (blocks != nullptr) {
      // Enumerate the full transitive contributor tree the mixer would walk and
      // collect the frontier want-list; a block is fully resident only when every
      // top-level contributor's whole closure (recursive descent + below-rate native
      // re-request) is resident (Constraints 1,3).
      const std::vector<Contribution> contribs = contributions_for(bi);
      for (const Contribution& c : contribs) {
        const bool r = collect_wants(blocks, c, wants, seen);
        all_resident = all_resident && r;
      }
    }
    // Mix only at FULL transitive residency (the pull will hit every contributor and
    // its closure), or when there is no cache to warm (the injected `PullService`
    // settles each pull inline). A block already prepared (retained across a reprime,
    // or freshly mixed this pass) is not re-mixed. The gate is what keeps the
    // threaded fill from ever mixing silence for a not-yet-rendered descendant
    // (Constraint 3); `d_silence_mixed` records any breach of it and stays 0.
    if (blocks == nullptr || all_resident) {
      if (!is_prepared(bi)) {
        mix_block(bi);
      }
    }
  }
  return wants;
}

template <class BlockValue>
std::vector<PrefetchWant> LookaheadRing::reprime(KeyedStore<BlockKey, BlockValue>* blocks,
                                                 Time playhead, Time horizon, int direction) {
  const std::vector<std::int64_t> keep = horizon_blocks(playhead, horizon, direction);
  const std::unordered_set<std::int64_t> keepset(keep.begin(), keep.end());
  for (auto it = d_prepared.begin(); it != d_prepared.end();) {
    if (keepset.count(it->first) == 0) {
      it = d_prepared.erase(it); // no longer ahead of the new playhead: flush
    } else {
      ++it; // still valid: retained, NOT re-mixed
    }
  }
  return prime(blocks, playhead, horizon, direction);
}

template <class BlockValue>
void LookaheadRing::invalidate(KeyedStore<BlockKey, BlockValue>* blocks, TimeRange range) {
  // Drop prepared output blocks whose window intersects the damage range.
  for (auto it = d_prepared.begin(); it != d_prepared.end();) {
    const std::int64_t s = block_start(it->first).flicks;
    const std::int64_t e = s + d_block_span_flicks;
    const bool overlap = s < range.end.flicks && range.start.flicks < e;
    if (overlap) {
      it = d_prepared.erase(it);
    } else {
      ++it;
    }
  }
  // Drop cached per-content BlockKeys whose window intersects the damage range, so
  // the next prime re-mixes exactly those (the per-frame window is the block index
  // scaled by the key's frame stride; exact for the un-time-mapped working axis).
  if (blocks != nullptr) {
    const std::int64_t block_frames = static_cast<std::int64_t>(d_config.block_frames);
    blocks->remove_if([&](const BlockKey& k) {
      if (k.rate == 0) {
        return false;
      }
      const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(k.rate);
      const std::int64_t s = k.block_index * fpf;
      const std::int64_t e = s + block_frames * fpf;
      return s < range.end.flicks && range.start.flicks < e;
    });
  }
}

} // namespace arbc
