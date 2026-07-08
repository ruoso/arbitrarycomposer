#include <arbc/audio_engine/lookahead.hpp>

#include <arbc/base/expected.hpp>      // expected
#include <arbc/base/rational_time.hpp> // Rational, TimeMap, TimeError
#include <arbc/base/time.hpp>

#include <cstddef>
#include <utility>

namespace arbc {

LookaheadRing::LookaheadRing(const DocRoot& doc, PullService& pull, LookaheadRingConfig config)
    : d_doc(doc), d_pull(pull), d_config(std::move(config)) {
  if (d_config.sample_rate != 0 && d_config.block_frames != 0) {
    const std::int64_t fpf =
        Time::flicks_per_second / static_cast<std::int64_t>(d_config.sample_rate);
    d_block_span_flicks = static_cast<std::int64_t>(d_config.block_frames) * fpf;
  }
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
  const BlockKey anchor{ObjectId{}, 0, base, d_config.sample_rate};
  const Time step{d_block_span_flicks};
  const std::vector<BlockKey> ring =
      cache::temporal_prefetch_ring(anchor, direction, step, horizon);
  out.reserve(out.size() + ring.size());
  for (const BlockKey& k : ring) {
    out.push_back(k.block_index);
  }
  return out;
}

std::vector<LookaheadRing::Contribution>
LookaheadRing::contributions_for(std::int64_t index) const {
  std::vector<Contribution> out;
  const CompositionRecord* comp = d_doc.find_composition(d_config.composition);
  if (comp == nullptr) {
    return out;
  }
  const Time win_start = block_start(index);
  const std::int64_t win_end = win_start.flicks + d_block_span_flicks;
  const ChannelLayout child_layout = comp->working_audio_format.layout;
  d_doc.for_each_layer_in(d_config.composition, [&](ObjectId layer_id) {
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
        layer->span.end.flicks <= win_start.flicks) {
      return;
    }
    const std::int64_t num = layer->time_map.rate.num();
    const std::int64_t den = layer->time_map.rate.den();
    if (num <= 0) {
      return;
    }
    const std::uint32_t child_rate = static_cast<std::uint32_t>(
        static_cast<std::int64_t>(d_config.sample_rate) * den / num);
    if (child_rate == 0) {
      return;
    }
    const expected<Time, TimeError> child_start = layer->time_map.evaluate(win_start);
    if (!child_start.has_value()) {
      return;
    }
    out.push_back(
        Contribution{layer->content, child_rate, child_layout, d_config.block_frames, *child_start});
  });
  return out;
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
  return BlockKey{c.content, d_config.revision, block_index, c.rate};
}

void LookaheadRing::mix_block(std::int64_t index) {
  const std::uint32_t ch = channel_count(d_config.layout);
  Prepared prepared;
  prepared.samples.assign(static_cast<std::size_t>(d_config.block_frames) * ch, 0.0F);
  AudioBlock block{prepared.samples.data(), d_config.block_frames, d_config.layout,
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
  };
  // The ring renders nothing itself beyond calling its sibling `mix_composition`
  // once per prepared output block (doc 12:11-21,150-208).
  prepared.meta =
      mix_composition(d_doc, d_config.composition, d_config.resolve, d_pull, req, d_config.policy);
  d_prepared[index] = std::move(prepared);
  ++d_blocks_mixed;
}

bool LookaheadRing::drain(std::int64_t index, AudioBlock& out, AudioResult& meta) {
  const std::size_t n = static_cast<std::size_t>(out.frames) * channel_count(out.layout);
  const auto it = d_prepared.find(index);
  if (it != d_prepared.end() && it->second.samples.size() == n) {
    // Pure consume: copy the already-mixed block. No mix, no allocation, no block.
    if (out.samples != nullptr) {
      for (std::size_t i = 0; i < n; ++i) {
        out.samples[i] = it->second.samples[i];
      }
    }
    meta = it->second.meta;
    return true;
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
  ++d_underruns;
  return false;
}

} // namespace arbc
