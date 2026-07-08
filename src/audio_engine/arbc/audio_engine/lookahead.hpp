#pragma once

#include <arbc/audio_engine/mix.hpp>       // MixResolver, MixPolicy, mix_composition
#include <arbc/base/ids.hpp>               // ObjectId
#include <arbc/base/time.hpp>              // Time, TimeRange
#include <arbc/cache/key_shapes.hpp>       // BlockKey
#include <arbc/cache/keyed_store.hpp>      // KeyedStore, PriorityClass
#include <arbc/cache/prefetch.hpp>         // temporal_prefetch_ring, prime_ring
#include <arbc/media/audio_block.hpp>      // AudioBlock, ChannelLayout, channel_count

#include <algorithm>
#include <cstddef>
#include <cstdint>
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
  //   * Cache path (`blocks != nullptr`): each block's contributor `BlockKey`s are
  //     classified `Temporal` on the prefetch ring via `cache::prime_ring`; a block
  //     is mixed only once ALL its contributors are resident (so a threaded fill
  //     never mixes silence for a not-yet-rendered contributor); the absent
  //     contributor blocks are returned as the fill want-list for the pump.
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
  struct Contribution {
    ObjectId content{};
    std::uint32_t rate{0};
    ChannelLayout layout{ChannelLayout::Stereo};
    std::uint32_t frames{0};
    Time window_start{};
  };

  std::vector<Contribution> contributions_for(std::int64_t index) const;
  BlockKey contribution_key(const Contribution& c) const;
  void mix_block(std::int64_t index);
  std::vector<std::int64_t> horizon_blocks(Time playhead, Time horizon, int direction) const;

  const DocRoot& d_doc;
  PullService& d_pull;
  LookaheadRingConfig d_config;
  std::int64_t d_block_span_flicks{0}; // block_frames * (flicks_per_second / rate)
  std::unordered_map<std::int64_t, Prepared> d_prepared;
  std::uint64_t d_blocks_mixed{0};
  std::uint64_t d_underruns{0};
};

// --- templated cache-touching methods (Value = compositor::AudioBlockValue at the
// runtime call site; a local double in the audio-engine unit test) -----------------

template <class BlockValue>
std::vector<PrefetchWant> LookaheadRing::prime(KeyedStore<BlockKey, BlockValue>* blocks,
                                               Time playhead, Time horizon, int direction) {
  std::vector<PrefetchWant> wants;
  const std::vector<std::int64_t> indices = horizon_blocks(playhead, horizon, direction);
  for (const std::int64_t bi : indices) {
    const std::vector<Contribution> contribs = contributions_for(bi);
    bool all_resident = true;
    if (blocks != nullptr) {
      std::vector<BlockKey> keys;
      keys.reserve(contribs.size());
      for (const Contribution& c : contribs) {
        keys.push_back(contribution_key(c));
      }
      // Classify residents `Temporal`, report the absent want-list (renders /
      // inserts nothing -- the pump's worker path fills the absent ones).
      const std::vector<BlockKey> absent =
          cache::prime_ring(*blocks, std::span<const BlockKey>(keys), PriorityClass::Temporal);
      if (!absent.empty()) {
        all_resident = false;
        const std::unordered_set<BlockKey> absent_set(absent.begin(), absent.end());
        for (const Contribution& c : contribs) {
          const BlockKey k = contribution_key(c);
          if (absent_set.count(k) != 0) {
            const std::int64_t fpf =
                Time::flicks_per_second / static_cast<std::int64_t>(c.rate);
            wants.push_back(PrefetchWant{
                k, c.content,
                TimeRange{c.window_start,
                          Time{c.window_start.flicks +
                               static_cast<std::int64_t>(c.frames) * fpf}},
                c.rate, c.layout, c.frames});
          }
        }
      }
    }
    // Mix only when the pull will hit (all contributors resident) or when there is
    // no cache to warm (the inline pull settles synchronously). A block already
    // prepared (retained across a reprime, or freshly mixed this pass) is not
    // re-mixed.
    if ((blocks == nullptr || all_resident) && !is_prepared(bi)) {
      mix_block(bi);
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
