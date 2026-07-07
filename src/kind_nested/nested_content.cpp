#include <arbc/kind_nested/nested_content.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace arbc {

namespace {

// Map a child-local audio extent back into parent time through a layer's time
// map inverse (doc 11:59-71). The per-edge map is `local = (parent - in)*rate +
// offset`, so the inverse is `parent = (local - offset)/rate + in`, itself an
// affine time map with rate `1/rate`, in `offset`, offset `in`. A zero rate (a
// degenerate map collapsing all parent time to one instant) and an overflowing
// evaluation are treated as UNBOUNDED (`nullopt`) -- a conservative extent that
// never wrongly silences a layer. A negative rate flips the interval, so the
// mapped endpoints are min/max-ordered.
std::optional<TimeRange> map_child_extent_to_parent(const TimeMap& tm, const TimeRange& child) {
  if (tm.rate.num() == 0) {
    return std::nullopt;
  }
  const TimeMap inverse{tm.offset, Rational{tm.rate.den(), tm.rate.num()}, tm.in};
  const expected<Time, TimeError> a = inverse.evaluate(child.start);
  const expected<Time, TimeError> b = inverse.evaluate(child.end);
  if (!a.has_value() || !b.has_value()) {
    return std::nullopt;
  }
  return TimeRange{Time{std::min(a->flicks, b->flicks)}, Time{std::max(a->flicks, b->flicks)}};
}

} // namespace

NestedContent::NestedContent(ObjectId child) : d_child(child) {}

void NestedContent::attach(PullService& pull, Backend& backend, NestedResolver resolver,
                           const DocRoot& doc) {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  d_pull = &pull;
  d_backend = &backend;
  d_resolver = std::move(resolver);
  d_doc = &doc;
  d_memo.valid = false; // a new pin re-keys the metadata (doc 05:15-16)
}

// --- metadata (composed + memoized on the pinned aggregate revision) ----------

void NestedContent::ensure_memo() const {
  // Caller holds d_mutex.
  assert(d_doc != nullptr && "NestedContent metadata queried before attach");
  const std::uint64_t revision = d_doc->revision();
  if (d_memo.valid && d_memo.revision == revision) {
    return; // a stable aggregate revision returns the cached values (doc 05:14)
  }

  ++d_metadata_recomputes;
  d_memo = Memo{};
  d_memo.revision = revision;
  d_memo.valid = true;

  const CompositionRecord* comp = d_doc->find_composition(d_child);
  if (comp == nullptr) {
    // Unresolved child: an empty, bounded-at-nothing, static placeholder graph.
    d_memo.bounds = Rect{0.0, 0.0, 0.0, 0.0};
    d_memo.stability = Stability::Static;
    return;
  }

  // Gather the child's member-layer contents (bottom-to-top membership, the
  // order `inputs()` exposes) plus their embedding transforms. The same pass
  // folds the audio metadata (doc 12:202-208): stability, and the time-mapped
  // union of the reachable audible child audio extents.
  bool audio_unbounded = false;
  d_doc->for_each_layer_in(d_child, [&](ObjectId layer_id) {
    const LayerRecord* layer = d_doc->find_layer(layer_id);
    if (layer == nullptr) {
      return;
    }
    Content* content = d_resolver ? d_resolver(layer->content) : nullptr;
    if (content == nullptr) {
      return; // an unresolved layer contributes no input edge (async/not loaded)
    }
    d_memo.inputs.push_back(ChildInput{content, layer->transform, layer->opacity});
    d_memo.input_refs.push_back(content);

    // Audio metadata fold (constraint 4): an inaudible layer or a layer with no
    // audio facet contributes nothing. Stability is Static iff every reachable
    // audible child audio facet is Static (Live dominates, else Timed).
    if (!layer->audible()) {
      return;
    }
    AudioFacet* af = content->audio();
    if (af == nullptr) {
      return;
    }
    const Stability s = af->audio_stability();
    if (s == Stability::Live) {
      d_memo.audio_stability = Stability::Live;
    } else if (s == Stability::Timed && d_memo.audio_stability != Stability::Live) {
      d_memo.audio_stability = Stability::Timed;
    }
    // Extent: the time-mapped union of the reachable child extents; a
    // Static-unbounded (nullopt) child (or a non-invertible map) makes the whole
    // extent unbounded (doc 12:26-29, constraint 4).
    const std::optional<TimeRange> ce = af->audio_extent();
    if (!ce.has_value()) {
      audio_unbounded = true;
    } else if (!audio_unbounded) {
      const std::optional<TimeRange> mapped = map_child_extent_to_parent(layer->time_map, *ce);
      if (!mapped.has_value()) {
        audio_unbounded = true;
      } else if (!d_memo.audio_extent.has_value()) {
        d_memo.audio_extent = *mapped;
      } else {
        d_memo.audio_extent =
            TimeRange{Time{std::min(d_memo.audio_extent->start.flicks, mapped->start.flicks)},
                      Time{std::max(d_memo.audio_extent->end.flicks, mapped->end.flicks)}};
      }
    }
  });
  if (audio_unbounded) {
    d_memo.audio_extent = std::nullopt; // any unbounded reachable child audio -> unbounded
  }

  // Bounds: the child canvas rect if declared, else the recursive union of the
  // reachable child-layer bounds mapped through their transforms; unbounded if
  // any reachable layer is unbounded (doc 05:15-16).
  if (comp->canvas_w > 0.0 && comp->canvas_h > 0.0) {
    d_memo.bounds = Rect::from_size(comp->canvas_w, comp->canvas_h);
  } else {
    std::optional<Rect> acc;
    bool unbounded = false;
    for (const ChildInput& in : d_memo.inputs) {
      const std::optional<Rect> b = in.content->bounds();
      if (!b.has_value()) {
        unbounded = true;
        break;
      }
      const Rect mapped = in.transform.map_rect(*b);
      if (!acc.has_value()) {
        acc = mapped;
      } else {
        acc = Rect{std::min(acc->x0, mapped.x0), std::min(acc->y0, mapped.y0),
                   std::max(acc->x1, mapped.x1), std::max(acc->y1, mapped.y1)};
      }
    }
    d_memo.bounds = unbounded ? std::optional<Rect>{}
                              : std::optional<Rect>{acc.value_or(Rect{0.0, 0.0, 0.0, 0.0})};
  }

  // Stability: Static iff every reachable child layer is Static; else Live if any
  // is Live, otherwise Timed (doc 05:21-22).
  Stability stability = Stability::Static;
  for (const ChildInput& in : d_memo.inputs) {
    const Stability s = in.content->stability();
    if (s == Stability::Live) {
      stability = Stability::Live;
      break;
    }
    if (s == Stability::Timed) {
      stability = Stability::Timed;
    }
  }
  d_memo.stability = stability;

  // Time extent: the union of the child layers' extents (doc 13:95); layers with
  // no extent contribute nothing.
  std::optional<TimeRange> extent;
  for (const ChildInput& in : d_memo.inputs) {
    const std::optional<TimeRange> t = in.content->time_extent();
    if (!t.has_value()) {
      continue;
    }
    if (!extent.has_value()) {
      extent = *t;
    } else {
      extent = TimeRange{Time{std::min(extent->start.flicks, t->start.flicks)},
                         Time{std::max(extent->end.flicks, t->end.flicks)}};
    }
  }
  d_memo.time_extent = extent;
}

std::optional<Rect> NestedContent::bounds() const {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  ensure_memo();
  return d_memo.bounds;
}

Stability NestedContent::stability() const {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  ensure_memo();
  return d_memo.stability;
}

std::optional<TimeRange> NestedContent::time_extent() const {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  ensure_memo();
  return d_memo.time_extent;
}

std::span<const ContentRef> NestedContent::inputs() const {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  ensure_memo();
  // The storage is stable for the pinned revision (a fixed pin never re-keys);
  // the span views the memo's own vector in declared order (content.hpp:287-289).
  return d_memo.input_refs;
}

Rect NestedContent::map_input_damage(std::size_t input, const Rect& rect) const {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  ensure_memo();
  if (input >= d_memo.inputs.size()) {
    return rect; // out of range: identity (safe over-approximation)
  }
  // Local space = child's identically, so an input layer's damage maps into
  // nested's output through that layer's embedding transform (covering).
  return d_memo.inputs[input].transform.map_rect(rect);
}

std::optional<std::size_t> NestedContent::identity(const RenderRequest& /*request*/) const {
  return std::nullopt; // conservative: never a pass-through (always faithful)
}

std::uint64_t NestedContent::metadata_recomputes() const noexcept {
  const std::lock_guard<std::recursive_mutex> lock(d_mutex);
  return d_metadata_recomputes;
}

// --- rendering (the synthetic viewport; "rendering is recursion", doc 05:24) --

void NestedContent::compose_child_layer(const LayerRecord& layer, const Affine& camera,
                                        const Rect& device_rect, const RenderRequest& request,
                                        Backend& backend, Surface& target) const {
  if (!layer.visible() || layer.opacity <= 0.0) {
    return;
  }
  Content* content = d_resolver ? d_resolver(layer.content) : nullptr;
  if (content == nullptr) {
    return; // unresolved layer: placeholder (nothing) for this layer (doc 05:50)
  }

  // Compose the synthetic camera (child-local -> device) with the layer's
  // embedding transform, then invert to map the visible device region back into
  // the layer's content-local space (the pull contract, doc 03; doc 04 culls).
  const Affine composed = compose(camera, layer.transform);
  const std::optional<Affine> inv = composed.inverse();
  if (!inv.has_value()) {
    return; // degenerate placement: cull (doc 04)
  }

  Rect region = inv->map_rect(device_rect);
  if (const std::optional<Rect> b = content->bounds(); b.has_value()) {
    region = region.intersect(*b);
  }
  if (region.empty()) {
    return;
  }
  // Re-project the bounds-clipped region forward to device and clip to the
  // visible device rect: a floating-point sliver at the half-open bounds edge
  // (content just outside declared bounds surviving `intersect` by rounding)
  // maps to zero device area and is culled here, so bounds honesty holds exactly
  // -- the doc 04 visibility/sub-pixel cull expressed robustly.
  if (composed.map_rect(region).intersect(device_rect).empty()) {
    return;
  }

  const double scale = composed.max_scale();
  if (!(scale > 0.0) || !std::isfinite(scale)) {
    return;
  }
  const int temp_width = static_cast<int>(std::ceil(region.width() * scale));
  const int temp_height = static_cast<int>(std::ceil(region.height() * scale));
  if (temp_width <= 0 || temp_height <= 0) {
    // Sub-pixel cull (doc 04) -- also the guaranteed termination of a <1x Droste
    // cycle, which bottoms out here after finitely many turns (doc 05:61-65).
    return;
  }

  expected<std::unique_ptr<Surface>, SurfaceError> temp_result =
      backend.make_surface(temp_width, temp_height, target.format());
  if (!temp_result.has_value()) {
    return; // backend cannot store the working format: cull (doc 09)
  }
  Surface& temp = **temp_result;
  backend.clear(temp, 0.0F, 0.0F, 0.0F, 0.0F);

  // The sub-request carries the outer request's snapshot, exactness, and deadline
  // VERBATIM (doc 05:93-101, constraint 2) -- never reset, recomputed, or
  // sub-budgeted per level. Only region/scale/target are the layer's own.
  const RenderRequest sub{
      region, scale, request.time, request.snapshot, temp, request.exactness, request.deadline};

  // Reuse the injected PullService, never `content->render` (doc 13:69-71): cache
  // lookup, worker dispatch, snapshot/deadline inheritance, aggregate revision,
  // the recursion-depth backstop (which terminates >=1x Droste cycles, doc
  // 05:66-70), identity short-circuit, and async are all the service's.
  auto done = std::make_shared<RenderCompletion>();
  d_pull->pull(content, sub, done);
  if (!done->settled()) {
    // The service dispatched the render to a worker (a cache miss). Nested's
    // descent is synchronous on the frame thread, so this pass shows the
    // placeholder for this layer (doc 05:50-52), exactly as `render_frame` skips
    // an async layer. Cancel the completion we will not drain.
    done->cancel();
    return;
  }
  const std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return; // budget-exceeded / failed pull: placeholder for this layer
  }
  const RenderResult result = **settled;

  // temp pixel (i, j) covers local (region origin + (i, j) / achieved): map temp
  // space through content-local space to device space (mirrors render_layer).
  const Affine temp_to_dst = compose(
      composed, compose(Affine::translation(region.x0, region.y0),
                        Affine::scaling(1.0 / result.achieved_scale, 1.0 / result.achieved_scale)));
  backend.composite(target, temp, temp_to_dst, layer.opacity);
}

std::optional<RenderResult> NestedContent::render(const RenderRequest& request,
                                                  std::shared_ptr<RenderCompletion>) {
  assert(d_pull != nullptr && d_backend != nullptr && d_doc != nullptr &&
         "NestedContent rendered before attach");
  Backend& backend = *d_backend;
  Surface& target = request.target;

  // The composed result is an ordinary content's pixels (doc 05:77-84): clear the
  // target, then source-over the child's layers. Settles inline -- nested's own
  // descent is synchronous; the leaf renders are what the service may defer.
  backend.clear(target, 0.0F, 0.0F, 0.0F, 0.0F);

  const CompositionRecord* comp = d_doc->find_composition(d_child);
  if (comp == nullptr) {
    // Unresolved / not-yet-loaded child (doc 05:50-52): the placeholder is empty
    // pixels. Honest: no crash, no wrong pixels.
    return RenderResult{request.scale, true};
  }

  // Homogeneous working-space precondition (doc 07:34-35). Nested composites the
  // child directly into the parent's working space (the request target's tag), so
  // the child's working space MUST equal it -- "homogeneous trees pay nothing".
  // The heterogeneous boundary needs a `Backend` conversion operation that does
  // not exist yet: it is the deferred `kinds.nested_working_space_conversion`, so
  // here the assumption is a precondition, never silently coerced.
  assert(comp->working_space == target.format() &&
         "heterogeneous nesting boundary deferred to kinds.nested_working_space_conversion");

  // Synthetic viewport (doc 05:24): the camera maps child-composition-local
  // coordinates to device (target) pixels, derived from the request's
  // region-to-surface mapping -- device = scale * (local - region.origin).
  const Affine camera = compose(Affine::scaling(request.scale, request.scale),
                                Affine::translation(-request.region.x0, -request.region.y0));
  const Rect device_rect =
      Rect::from_size(static_cast<double>(target.width()), static_cast<double>(target.height()));

  // Bottom-to-top over the child's members at the pinned version (doc 02/05:71-75)
  // -- membership read from the frozen `DocRoot`, so a Droste scene sees the same
  // revisions on every visit within the frame.
  d_doc->for_each_layer_in(d_child, [&](ObjectId layer_id) {
    const LayerRecord* layer = d_doc->find_layer(layer_id);
    if (layer == nullptr) {
      return;
    }
    compose_child_layer(*layer, camera, device_rect, request, backend, target);
  });

  return RenderResult{request.scale, true};
}

// --- audio (the synthetic monitor; "the aggregate revision covers audio", 12:208)

AudioFacet* NestedContent::audio() { return &d_audio_facet; }

std::optional<TimeRange> NestedContent::NestedAudioFacet::audio_extent() const {
  const std::lock_guard<std::recursive_mutex> lock(d_owner->d_mutex);
  d_owner->ensure_memo();
  return d_owner->d_memo.audio_extent;
}

Stability NestedContent::NestedAudioFacet::audio_stability() const {
  const std::lock_guard<std::recursive_mutex> lock(d_owner->d_mutex);
  d_owner->ensure_memo();
  return d_owner->d_memo.audio_stability;
}

void NestedContent::mix_child_layer(const LayerRecord& layer, const CompositionRecord& comp,
                                    const AudioRequest& request, std::uint32_t& achieved,
                                    bool& exact) const {
  // Audio visibility cull (doc 12:86-87,129-130): an inaudible or zero-gain layer
  // contributes nothing.
  if (!layer.audible() || layer.gain <= 0.0) {
    return;
  }
  Content* content = d_resolver ? d_resolver(layer.content) : nullptr;
  if (content == nullptr) {
    return; // unresolved layer: silence for this layer (doc 05:50)
  }
  AudioFacet* af = content->audio();
  if (af == nullptr) {
    return; // no audio facet: skipped at zero cost (doc 12:86-87)
  }

  // Span cull (doc 11:62-73): a degenerate span, or one that does not overlap the
  // request window, contributes nothing.
  if (layer.span.empty() || request.window.end.flicks <= layer.span.start.flicks ||
      layer.span.end.flicks <= request.window.start.flicks) {
    return;
  }

  // Varispeed (doc 12:107-118): request the child at the composed rational rate
  // `child_rate = request.sample_rate / rate` (rate = num/den), so a rate-1/2
  // layer requests at twice the rate and pitches the child down an octave. The
  // rate is recomputed from the per-edge rational every descent -- never
  // accumulated (doc 11:216-234). Reverse / zero-rate audio (num <= 0) is out of
  // scope (deferred with time-stretch, doc 12:118): cull rather than mis-mix.
  const std::int64_t num = layer.time_map.rate.num();
  const std::int64_t den = layer.time_map.rate.den();
  if (num <= 0) {
    return;
  }
  const std::uint32_t child_rate =
      static_cast<std::uint32_t>(static_cast<std::int64_t>(request.sample_rate) * den / num);
  if (child_rate == 0) {
    return;
  }

  // Child-local window start: the parent window start mapped through the layer
  // time map. A procedural child then samples frame f at child_start + f *
  // flicks_per_second/child_rate == the time-mapped parent frame instant, so a
  // nested-of-tones scene is byte-exact at any depth.
  const expected<Time, TimeError> child_start = layer.time_map.evaluate(request.window.start);
  if (!child_start.has_value()) {
    return; // time-map overflow: cull (doc 11:52-56)
  }

  // Request the child at its working-format layout (the nesting boundary converts,
  // doc 12:95-105) and the composed rate. A homogeneous tree pays nothing (child
  // layout == request layout -> a direct mix); a layout mismatch is remixed inline.
  const std::uint32_t out_ch = channel_count(request.layout);
  const ChannelLayout child_layout = comp.working_audio_format.layout;
  const std::uint32_t in_ch = channel_count(child_layout);
  const std::uint32_t frames = request.target.frames;
  const std::int64_t fpf_child = Time::flicks_per_second / static_cast<std::int64_t>(child_rate);

  std::vector<float> child_buf(static_cast<std::size_t>(frames) * in_ch, 0.0F);
  AudioBlock child_block{child_buf.data(), frames, child_layout, child_rate};
  const AudioRequest child_req{
      TimeRange{*child_start,
                Time{child_start->flicks + static_cast<std::int64_t>(frames) * fpf_child}},
      child_rate,
      child_layout,
      child_block,
      request.exactness, // carried verbatim (constraint 2)
      request.snapshot,  // carried verbatim (constraint 8)
  };

  // Pull through the injected service, NEVER `af->render_audio` directly (doc 13
  // operator rule, audio twin): block-cache serve, worker dispatch, and the
  // recursion-depth backstop are the service's, exactly as the visual descent
  // relies on `pull` (constraint 2, 7).
  auto done = std::make_shared<AudioCompletion>();
  d_pull->pull_audio(content, child_req, done);
  if (!done->settled()) {
    // The service deferred to a worker (a miss): this pass mixes silence for the
    // layer (doc 05:50-52), exactly as the visual descent shows the placeholder.
    done->cancel();
    return;
  }
  const std::optional<expected<AudioResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return; // budget-exceeded / unavailable pull (a Droste backstop): silence
  }
  const AudioResult cr = **settled;

  // Additive Flat-mode mix (doc 12:129-130): contribution = gain * child, summed
  // into the target, remixed to the request layout. Placement is 1:1 -- the child
  // was requested at the composed rate over `frames` frames.
  const float gain = static_cast<float>(layer.gain);
  for (std::uint32_t f = 0; f < frames; ++f) {
    for (std::uint32_t c = 0; c < out_ch; ++c) {
      float s = 0.0F;
      if (in_ch == out_ch) {
        s = child_buf[static_cast<std::size_t>(f) * in_ch + c];
      } else if (in_ch == 1) {
        s = child_buf[f]; // mono child -> every request channel
      } else {
        // stereo child -> mono request: average the channels (baseline downmix).
        s = 0.5F * (child_buf[static_cast<std::size_t>(f) * 2] +
                    child_buf[static_cast<std::size_t>(f) * 2 + 1]);
      }
      request.target.samples[static_cast<std::size_t>(f) * out_ch + c] += gain * s;
    }
  }

  // achieved_rate / exact honesty (constraint 6). A child honoring the composed
  // rate keeps the boundary at the request rate; a below-rate child (its block
  // reinterpreted by a baseline nearest/hold fill -- high-quality resampling is
  // the deferred `kinds.nested_audio_resampling`) lowers the aggregate and marks
  // it inexact.
  if (!cr.exact || cr.achieved_rate != child_rate) {
    exact = false;
    const std::uint64_t eff =
        static_cast<std::uint64_t>(cr.achieved_rate) * request.sample_rate / child_rate;
    achieved = std::min(achieved, static_cast<std::uint32_t>(eff));
  }
}

std::optional<AudioResult>
NestedContent::NestedAudioFacet::render_audio(const AudioRequest& request,
                                              std::shared_ptr<AudioCompletion>) {
  NestedContent& self = *d_owner;
  assert(self.d_pull != nullptr && self.d_doc != nullptr &&
         "NestedContent audio rendered before attach");

  // The composed block is an ordinary content's samples (doc 12:202-208): start
  // from silence, then additively mix each audible child layer. Settles INLINE --
  // nested's descent is synchronous; only the leaf audio pulls are what the
  // service may defer, mirroring the visual `render`.
  const std::uint32_t ch = channel_count(request.layout);
  const std::size_t n = static_cast<std::size_t>(request.target.frames) * ch;
  for (std::size_t i = 0; i < n; ++i) {
    request.target.samples[i] = 0.0F;
  }

  const CompositionRecord* comp = self.d_doc->find_composition(self.d_child);
  if (comp == nullptr) {
    // Unresolved / not-yet-loaded child (doc 05:50-52): a silent block, honest.
    return AudioResult{request.sample_rate, true};
  }

  // achieved_rate = min over contributing children (a honoring child keeps it at
  // the request rate); exact = the conjunction. No contributor -> a faithful
  // silent block at the request rate.
  std::uint32_t achieved = request.sample_rate;
  bool exact = true;

  // Bottom-to-top over the child's members at the pinned version (doc 02/05:71-75)
  // -- membership read from the frozen `DocRoot`, so a Droste audio scene sees the
  // same revisions on every visit within the frame (the audio revision space is
  // the visual one, doc 12:208).
  self.d_doc->for_each_layer_in(self.d_child, [&](ObjectId layer_id) {
    const LayerRecord* layer = self.d_doc->find_layer(layer_id);
    if (layer == nullptr) {
      return;
    }
    self.mix_child_layer(*layer, *comp, request, achieved, exact);
  });

  return AudioResult{achieved, exact};
}

} // namespace arbc
