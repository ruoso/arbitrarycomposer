#include <arbc/audio_engine/lookahead.hpp>
#include <arbc/base/expected.hpp>      // expected
#include <arbc/base/rational_time.hpp> // Rational, TimeMap, TimeError
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp> // Content, AudioFacet::latency()

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace arbc {

LookaheadRing::LookaheadRing(const DocRoot& doc, PullService& pull, LookaheadRingConfig config)
    : d_doc(doc), d_pull(pull), d_config(std::move(config)) {
  if (d_config.sample_rate != 0 && d_config.block_frames != 0) {
    const std::int64_t fpf =
        Time::flicks_per_second / static_cast<std::int64_t>(d_config.sample_rate);
    d_block_span_flicks = static_cast<std::int64_t>(d_config.block_frames) * fpf;
  }
  // Allocate the fixed lock-free prepared-block ring once (Decision D2,
  // Constraint 3): each slot's sample buffer is pre-sized to one output block and
  // never resized, so the RT drain neither allocates nor races a reallocation. The
  // producer-owned `d_mix_scratch` is where `mix_block` mixes before the short
  // seqlock publish.
  d_capacity = d_config.ring_capacity != 0 ? d_config.ring_capacity : k_default_ring_capacity;
  const std::size_t frame_floats =
      static_cast<std::size_t>(d_config.block_frames) * channel_count(d_config.layout);
  d_slots = std::make_unique<Slot[]>(d_capacity);
  for (std::size_t i = 0; i < d_capacity; ++i) {
    d_slots[i].samples.assign(frame_floats, 0.0F);
  }
  d_mix_scratch.assign(frame_floats, 0.0F);
}

LookaheadRing::Slot& LookaheadRing::slot_at(std::int64_t index) {
  const std::int64_t cap = static_cast<std::int64_t>(d_capacity);
  return d_slots[static_cast<std::size_t>(((index % cap) + cap) % cap)];
}

const LookaheadRing::Slot& LookaheadRing::slot_at(std::int64_t index) const {
  const std::int64_t cap = static_cast<std::int64_t>(d_capacity);
  return d_slots[static_cast<std::size_t>(((index % cap) + cap) % cap)];
}

void LookaheadRing::publish_slot(std::int64_t index, const AudioResult& meta) {
  // Seqlock write (producer side): mark the slot writing (odd), publish the payload
  // through atomic_ref so the concurrent RT read is race-free, then mark it stable
  // (even) with a release so a reader that observes the even generation also
  // observes the payload. The write critical section is a bounded copy of one
  // pre-mixed block -- the RT reader only ever retries across it, never blocks.
  Slot& slot = slot_at(index);
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  const std::size_t n = d_mix_scratch.size();
  for (std::size_t i = 0; i < n; ++i) {
    std::atomic_ref<float>(slot.samples[i]).store(d_mix_scratch[i], std::memory_order_relaxed);
  }
  slot.achieved_rate.store(meta.achieved_rate, std::memory_order_relaxed);
  slot.exact.store(meta.exact, std::memory_order_relaxed);
  slot.index.store(index, std::memory_order_relaxed);
  slot.seq.store(s + 2, std::memory_order_release);
}

void LookaheadRing::retire_slot(Slot& slot) {
  // Seqlock write that empties a slot on reprime/damage: the generation bump makes
  // a concurrent drain of this block re-validate and see the empty index, returning
  // silence + underrun (composes with seek_drain_realign's cursor re-seat).
  const std::uint64_t s = slot.seq.load(std::memory_order_relaxed);
  slot.seq.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  slot.index.store(k_empty_slot, std::memory_order_relaxed);
  slot.seq.store(s + 2, std::memory_order_release);
}

void LookaheadRing::set_spatial(std::optional<Spatialization> spatial) {
  // Stage the new listener seed the next `prime`/`reprime` reads. Called on the pump
  // thread under the pump's `d_mutex`, serialized with every ring read, so no worker
  // observes a torn seed (audio.spatial_camera_follow, Decision D5).
  d_config.spatial = std::move(spatial);
  // A listener change invalidates the whole prepared ring (every slot carries the old
  // listener's content), unlike a seek's window-shift retention: retire all live slots
  // so the following reprime re-mixes them under the new listener. The generation bump
  // makes a concurrent RT drain of a retired slot underrun (the same discipline as
  // `reprime`/`invalidate`).
  for (std::size_t i = 0; i < d_capacity; ++i) {
    Slot& slot = d_slots[i];
    if (slot.index.load(std::memory_order_relaxed) != k_empty_slot) {
      retire_slot(slot);
    }
  }
}

std::size_t LookaheadRing::prepared_count() const noexcept {
  std::size_t count = 0;
  for (std::size_t i = 0; i < d_capacity; ++i) {
    if (d_slots[i].index.load(std::memory_order_relaxed) != k_empty_slot) {
      ++count;
    }
  }
  return count;
}

bool LookaheadRing::is_prepared(std::int64_t index) const {
  return slot_at(index).index.load(std::memory_order_relaxed) == index;
}

std::int64_t LookaheadRing::block_index_at(Time t) const {
  if (d_block_span_flicks <= 0) {
    return 0;
  }
  std::int64_t q = t.flicks / d_block_span_flicks;
  // Floor toward negative infinity so a reverse playhead lands in the block that
  // CONTAINS it (C++ integer division truncates toward zero).
  if (t.flicks % d_block_span_flicks != 0 && t.flicks < 0) {
    --q;
  }
  return q;
}

Time LookaheadRing::block_start(std::int64_t index) const {
  return Time{index * d_block_span_flicks};
}

std::vector<std::int64_t> LookaheadRing::horizon_blocks(Time playhead, Time horizon,
                                                        int direction) const {
  const std::int64_t base = block_index_at(playhead);
  std::vector<std::int64_t> out{base};
  if (d_block_span_flicks <= 0) {
    return out;
  }
  // Enumerate the upcoming output blocks with the shared temporal prefetch ring in
  // the sign of the playback direction (Constraint 4): one bucket == one output
  // block, the horizon bounds it to `horizon / block_span` blocks.
  // The root/anchor slot folds the monitor's own listener digest (D4): two monitors
  // of differing listener sharing one BlockCache must not collide on the root output
  // slot. Flat digests to 0, so this anchor is byte-identical to the pre-task one.
  const BlockKey anchor{ObjectId{}, 0, base, d_config.sample_rate,
                        spatial_context_digest(d_config.spatial)};
  const Time step{d_block_span_flicks};
  const std::vector<BlockKey> ring =
      cache::temporal_prefetch_ring(anchor, direction, step, horizon);
  out.reserve(out.size() + ring.size());
  for (const BlockKey& k : ring) {
    out.push_back(k.block_index);
  }
  return out;
}

Time LookaheadRing::effective_preroll(Time playhead) const {
  // v1 constant-latency pre-roll (audio.latency, doc 12:200-212, Constraint 4): read
  // latency() from the anchor block's audible DIRECT (depth-1) contributors -- exactly
  // the top-level entries `contributions_for` returns after `descend` applies
  // mix_layer's audible/gain/facet/span culls -- and take the maximum, floored by the
  // configured manual pre-roll. The value is treated as an output-(root-)time quantity;
  // mapping a nested contributor's local latency back through its time map is the
  // doc-12-deferred "latency in nested graphs' effect chains" and is NOT walked here.
  Time pre = d_config.preroll;
  const std::vector<Contribution> direct = contributions_for(block_index_at(playhead));
  for (const Contribution& c : direct) {
    Content* content = d_config.resolve ? d_config.resolve(c.content) : nullptr;
    if (content == nullptr) {
      continue;
    }
    const AudioFacet* af = content->audio();
    if (af == nullptr) {
      continue;
    }
    const Time lat = af->latency();
    if (lat.flicks > pre.flicks) {
      pre = lat;
    }
  }
  return pre;
}

Time LookaheadRing::lifted_horizon(Time playhead, Time horizon) const {
  const Time eff = effective_preroll(playhead);
  if (d_block_span_flicks <= 0 || eff.flicks <= 0) {
    return horizon; // no declared latency (the common path): the shipped window verbatim
  }
  // Round the pre-roll UP to whole output-block spans: the enumerated block count then
  // grows by exactly `ceil(eff / block_span)` regardless of the base horizon's
  // alignment (`horizon_blocks` enumerates `extended / block_span` buckets after the
  // anchor, and `preroll_blocks * block_span` is an exact multiple).
  const std::int64_t preroll_blocks = (eff.flicks + d_block_span_flicks - 1) / d_block_span_flicks;
  return Time{horizon.flicks + preroll_blocks * d_block_span_flicks};
}

std::vector<LookaheadRing::Contribution>
LookaheadRing::contributions_for(std::int64_t index) const {
  std::vector<Contribution> out;
  // The root composition's layers are pulled at descent depth 1 (`mix_block` mixes
  // the root itself, then `mix_layer` pulls each layer at `d_depth 0 -> 1`), so the
  // transitive descent bottoms out on the same `pull_audio` depth backstop the
  // mixer does (Constraint 5). The Spatial sub-audible cull is seeded from the
  // context's own `accum_atten` (the camera's uniform scale-attenuation the monitor
  // seeds), exactly as the mixer starts from `request.spatial->accum_atten`
  // (audio.spatial_fill_cull, Decision D1); Flat seeds a harmless 1.0F.
  const float seed_atten = d_config.spatial ? d_config.spatial->accum_atten : 1.0F;
  // The root composition's frame is the monitor's listener (audio.spatial_nested_warm_context,
  // Decision D2): the mixer starts `mix_layer` from `request.spatial->listener`, so the warming
  // descent starts from the same seed. Flat seeds a harmless identity (the spatial branch is
  // gated off, so it is never read).
  const Affine seed_listener = d_config.spatial ? d_config.spatial->listener : Affine::identity();
  descend(d_config.composition, d_config.sample_rate, block_start(index), 1, seed_atten,
          seed_listener, out);
  return out;
}

void LookaheadRing::descend(ObjectId composition, std::uint32_t request_rate, Time window_start,
                            std::uint32_t depth, float accum_atten, const Affine& parent_listener,
                            std::vector<Contribution>& out) const {
  const CompositionRecord* comp = d_doc.find_composition(composition);
  if (comp == nullptr) {
    return;
  }
  // The request window at THIS level's rate (`window_start` is composition-local):
  // the span cull compares layer spans in this composition's local time, exactly as
  // `mix_layer` / `mix_child_layer` compare against `request.window` (mix.cpp:44-45,
  // nested_content.cpp:413-414).
  const std::int64_t fpf_req =
      request_rate != 0 ? Time::flicks_per_second / static_cast<std::int64_t>(request_rate) : 0;
  const std::int64_t win_start = window_start.flicks;
  const std::int64_t win_end =
      win_start + static_cast<std::int64_t>(d_config.block_frames) * fpf_req;
  const ChannelLayout child_layout = comp->working_audio_format.layout;
  d_doc.for_each_layer_in(composition, [&](ObjectId layer_id) {
    const LayerRecord* layer = d_doc.find_layer(layer_id);
    if (layer == nullptr) {
      return;
    }
    // Mirror `mix_layer`'s culls (doc 12:86-87,129-130; doc 11:62-73): an inaudible,
    // zero-gain, unresolved, facet-less, out-of-span, or reverse/zero-rate layer
    // pulls nothing, so it warms no per-content block.
    if (!layer->audible() || layer->gain <= 0.0) {
      return;
    }
    Content* content = d_config.resolve ? d_config.resolve(layer->content) : nullptr;
    if (content == nullptr || content->audio() == nullptr) {
      return;
    }
    if (layer->span.empty() || win_end <= layer->span.start.flicks ||
        layer->span.end.flicks <= win_start) {
      return;
    }
    // Spatial sub-audible cull (audio.spatial_fill_cull, doc 12:200-206): the exact
    // `mix.cpp:64-66` predicate ported into the warming walk (Constraint 1, Decision
    // D3). Gated on `d_config.spatial.has_value()` -- byte-identical to `mix.cpp:57`'s
    // `request.spatial.has_value()` gate -- so a Flat scene warms exactly what it warms
    // today (Constraint 2, Decision D2). Only the scalar attenuation product drives the
    // decision, so no full `Spatialization` / pan is composed here (Decision D1). A
    // sub-audible layer is neither warmed nor descended, matching the tree the mixer
    // pulls edge-for-edge, so the warmed set equals the culled tree (Constraint 3) and a
    // shrinking Droste chain terminates at the cull depth, backstopped by `max_depth`
    // (Constraint 5). `child_atten` is the accumulated product this edge gains, threaded
    // to the nested descent exactly as `mix_layer` threads `sp.accum_atten * edge_atten`
    // to the child context (mix.cpp:72-73).
    float child_atten = accum_atten;
    std::optional<Spatialization> child_spatial;
    Affine child_listener = parent_listener;
    if (d_config.spatial.has_value()) {
      const float edge_atten = spatial_edge_atten(layer->transform);
      if (accum_atten * edge_atten < d_config.spatial->sub_audible) {
        return; // sub-audible: not warmed, not descended (recursion terminator, D3)
      }
      child_atten = accum_atten * edge_atten;
      // Per-edge composed listener + full Spatialization, byte-identical to the
      // `child_spatial` `mix_layer` derives for this edge (mix.cpp:62-73)
      // (audio.spatial_nested_warm_context, Decision D2/D3, Constraint 1): the same
      // `compose(parent_listener, transform)` (per-edge, never accumulated -- doc
      // 11:187-188), the same viewport extent and sub-audible threshold (invariant down
      // the descent, so read from `d_config.spatial`), and the same
      // `accum_atten * edge_atten` product (the scalar already threaded for the cull).
      // Attached to the Contribution so the pump warms this contributor under the exact
      // context the mixer pulls it with, and threaded as the next level's listener.
      child_listener = compose(parent_listener, layer->transform);
      child_spatial =
          Spatialization{child_listener, d_config.spatial->viewport_w, d_config.spatial->viewport_h,
                         child_atten, d_config.spatial->sub_audible};
    }
    // Varispeed: the composed rational rate `request_rate * den/num`, recomputed per
    // edge, never accumulated (doc 11:187-188) -- identical to `mix_layer`:55-64.
    const std::int64_t num = layer->time_map.rate.num();
    const std::int64_t den = layer->time_map.rate.den();
    if (num <= 0) {
      return;
    }
    const std::uint32_t child_rate =
        static_cast<std::uint32_t>(static_cast<std::int64_t>(request_rate) * den / num);
    if (child_rate == 0) {
      return;
    }
    const expected<Time, TimeError> child_start = layer->time_map.evaluate(window_start);
    if (!child_start.has_value()) {
      return;
    }
    Contribution c{layer->content,        child_rate,   child_layout,
                   d_config.block_frames, *child_start, {}};
    c.spatial = child_spatial; // the per-edge pull context (nullopt in Flat mode)
    // Recurse into a nested-composition contributor (the structural edge the runtime
    // pump injects, Decision D2), bounded by the shared depth budget so a Droste
    // scene terminates on the doc-05 backstop rather than building an infinite tree
    // (Constraint 5). Descent stops one level before the mixer's `pull_audio`
    // backstop would (`depth < max_depth`), matching the tree the mixer walks. The
    // composed listener flows down as the child level's `parent_listener`, so a nested
    // composition's own layers compose against it exactly as `render_audio` ->
    // `mix_child_layer` does with `child_req.spatial->listener` (Decision D2).
    if (d_config.nested_composition && depth < d_config.max_depth) {
      const std::optional<ObjectId> child_comp = d_config.nested_composition(layer->content);
      if (child_comp.has_value()) {
        descend(*child_comp, child_rate, *child_start, depth + 1, child_atten, child_listener,
                c.children);
      }
    }
    out.push_back(std::move(c));
  });
}

BlockKey LookaheadRing::contribution_key(const Contribution& c) const {
  // The exact `BlockKey` `PullServiceImpl::pull_audio` computes for the mixer's
  // child request (`pull_service.cpp:301`, `audio_block_index`): the content id, the
  // doc-global revision, the window-start sample-frame index at the child rate, and
  // the child rate. The pump wires `id_of(content) == layer.content` and
  // `contribution(content) == revision`, so the key the fill populates is the key
  // the mix pass probes.
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(c.rate);
  const std::int64_t block_index = fpf != 0 ? c.window_start.flicks / fpf : 0;
  // Fold the per-edge Spatial context digest (D1/D4): this write-side warm key must
  // equal the read-side pull key `pull_service.cpp:301` builds from `request.spatial`
  // for the same edge, preserved because `spatial_nested_warm_context` made the two
  // `Spatialization` structs bit-identical (Constraint 2). Flat digests to 0.
  return BlockKey{c.content, d_config.revision, block_index, c.rate,
                  spatial_context_digest(c.spatial)};
}

PrefetchWant LookaheadRing::make_want(const Contribution& c) const {
  const std::int64_t fpf = Time::flicks_per_second / static_cast<std::int64_t>(c.rate);
  return PrefetchWant{contribution_key(c),
                      c.content,
                      TimeRange{c.window_start, Time{c.window_start.flicks +
                                                     static_cast<std::int64_t>(c.frames) * fpf}},
                      c.rate,
                      c.layout,
                      c.frames,
                      c.spatial}; // the per-edge pull context the pump threads onto AudioRequest
}

std::optional<PrefetchWant>
LookaheadRing::native_rerequest_want(const Contribution& c, std::uint32_t achieved_rate) const {
  // A rate-honoring contributor conveys no native re-request (mix.cpp:127): the mix
  // keeps the working-rate discovery block 1:1.
  if (achieved_rate == 0 || achieved_rate >= c.rate) {
    return std::nullopt;
  }
  // The exact second pull `mix_layer` issues (mix.cpp:127-148): `native_frames` spans
  // the SAME child-local window at the native rate; the key's `rate` field is the
  // native rate, so it is a distinct `BlockKey` from the discovery block. One
  // rounding at the leaf (doc 11:187-188).
  const std::int64_t fpf_native =
      Time::flicks_per_second / static_cast<std::int64_t>(achieved_rate);
  const std::uint32_t native_frames =
      static_cast<std::uint32_t>(static_cast<std::uint64_t>(c.frames) * achieved_rate / c.rate + 1);
  const std::int64_t block_index = fpf_native != 0 ? c.window_start.flicks / fpf_native : 0;
  // Same per-edge context as the discovery pull (D4): the below-rate native re-request
  // is a distinct `BlockKey` (native `rate`) but the SAME spatial context, so a
  // below-rate Spatial contributor does not collide on its native slot either.
  const BlockKey key{c.content, d_config.revision, block_index, achieved_rate,
                     spatial_context_digest(c.spatial)};
  return PrefetchWant{
      key,
      c.content,
      TimeRange{c.window_start, Time{c.window_start.flicks +
                                     static_cast<std::int64_t>(native_frames) * fpf_native}},
      achieved_rate,
      c.layout,
      native_frames,
      c.spatial}; // same context as the discovery pull (mix.cpp:176: leaves ignore it)
}

void LookaheadRing::mix_block(std::int64_t index) {
  // Mix into the producer-owned scratch (off the RT thread; allocation here is
  // fine), then publish it into the slot with a short seqlock write so the RT drain
  // never observes a half-mixed buffer (Decision D2).
  std::fill(d_mix_scratch.begin(), d_mix_scratch.end(), 0.0F);
  AudioBlock block{d_mix_scratch.data(), d_config.block_frames, d_config.layout,
                   d_config.sample_rate};
  const Time win_start = block_start(index);
  const std::int64_t fpf =
      Time::flicks_per_second / static_cast<std::int64_t>(d_config.sample_rate);
  const AudioRequest req{
      TimeRange{win_start,
                Time{win_start.flicks + static_cast<std::int64_t>(d_config.block_frames) * fpf}},
      d_config.sample_rate,
      d_config.layout,
      block,
      Exactness::BestEffort, // audio renders ahead, not against a deadline (doc 12:33-34)
      StateHandle{},
      d_config.spatial, // the static Spatial seed (nullopt => Flat, byte-identical)
  };
  // The ring renders nothing itself beyond calling its sibling `mix_composition`
  // once per prepared output block (doc 12:11-21,150-208). Spatial mixing is
  // producer-side here (off the RT drain, doc 12:31-34; refinement Constraint 8).
  const AudioResult meta =
      mix_composition(d_doc, d_config.composition, d_config.resolve, d_pull, req, d_config.policy);
  if (d_config.spatial.has_value()) {
    // Post-scale the top mix by the camera's uniform scale-attenuation (doc 12:186-190,
    // refinement point 5): each layer applied only its OWN edge attenuation, so the
    // camera's own scale multiplies the whole root mix once, here.
    const float cam_atten = d_config.spatial->accum_atten;
    const std::size_t n =
        static_cast<std::size_t>(d_config.block_frames) * channel_count(d_config.layout);
    for (std::size_t i = 0; i < n; ++i) {
      d_mix_scratch[i] *= cam_atten;
    }
  }
  publish_slot(index, meta);
  d_blocks_mixed.fetch_add(1, std::memory_order_relaxed);
}

bool LookaheadRing::drain(std::int64_t index, AudioBlock& out,
                          AudioResult& meta) ARBC_RT_NONBLOCKING {
  const std::size_t n = static_cast<std::size_t>(out.frames) * channel_count(out.layout);
  Slot& slot = slot_at(index);
  // Lock-free seqlock read (Decision D2): acquire the generation, copy the payload
  // through atomic_ref, then re-validate. A bounded retry absorbs the rare case of
  // catching the producer mid-publish (in steady state the drained block was mixed
  // many ticks ago, so the read is uncontended and never retries -- underruns stay
  // 0). No lock, no allocation, no block: pure consume, RealtimeSanitizer-clean.
  constexpr int k_attempts = 4;
  for (int attempt = 0; attempt < k_attempts; ++attempt) {
    const std::uint64_t s1 = slot.seq.load(std::memory_order_acquire);
    if ((s1 & 1U) != 0U) {
      continue; // a publish is in progress -> retry
    }
    const std::int64_t have = slot.index.load(std::memory_order_relaxed);
    const std::uint32_t ar = slot.achieved_rate.load(std::memory_order_relaxed);
    const bool ex = slot.exact.load(std::memory_order_relaxed);
    if (have == index && out.samples != nullptr && slot.samples.size() == n) {
      for (std::size_t i = 0; i < n; ++i) {
        out.samples[i] = std::atomic_ref<float>(slot.samples[i]).load(std::memory_order_relaxed);
      }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.seq.load(std::memory_order_relaxed) != s1) {
      continue; // torn: the producer republished this slot during the read -> retry
    }
    if (have == index) {
      meta = AudioResult{ar, ex};
      return true; // consistent snapshot of a ready block
    }
    break; // stable read of a different / empty block -> underrun
  }
  // Underrun: silence + a counter, NEVER a synchronous mix (Constraint 5,
  // Decision "the drain path is pure-consume"). The correct response to chronic
  // underrun is a larger horizon or more workers, a device_monitor tuning concern.
  if (out.samples != nullptr) {
    for (std::size_t i = 0; i < n; ++i) {
      out.samples[i] = 0.0F;
    }
  }
  meta = AudioResult{out.rate, true};
  d_underruns.fetch_add(1, std::memory_order_relaxed);
  return false;
}

} // namespace arbc
