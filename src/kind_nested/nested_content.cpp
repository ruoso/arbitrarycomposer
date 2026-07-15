#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/media/audio_resampler.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace arbc {

namespace {

// ONE lock for every NestedContent memo, not one per content. `ensure_memo` holds the
// lock across the CHILD contents' metadata queries (the audio fold), and a child may
// embed back into this very content: an embedding graph may be CYCLIC (doc 05:54-75),
// and a cyclic graph admits no consistent per-node lock order. With a lock per content,
// two threads walking the same cycle from different entry nodes take A-then-B and
// B-then-A -- a lock-order inversion, and a real deadlock the moment they interleave.
// A single lock has nothing to invert.
//
// It stays RECURSIVE for the reason the per-content lock was: the same-thread re-entrant
// query a cycle produces (a node's walk reaching itself) must short-circuit on the
// up-front-valid memo, not self-deadlock.
//
// Cheap enough to share: it covers only the metadata memo walk -- id lookups in the
// pinned immutable snapshot, the lockless side-map resolve, and the children's memos.
// `render`/`render_audio`, the heavy paths and the only ones that block on a pull, never
// take it.
std::recursive_mutex& memo_mutex() {
  static std::recursive_mutex mutex;
  return mutex;
}

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

// The external-child ctor (doc 05:47-61). Nothing below this line branches on `d_ref`:
// an externally-loaded child is an ordinary composition id, so `ensure_memo`, `render`,
// `render_audio` and `inputs()` take exactly the path they take for an in-document one
// -- and an UNAVAILABLE reference is the `d_child == ObjectId{}` case those three already
// answer with the empty placeholder (`find_composition` returns null for an absent id).
NestedContent::NestedContent(ObjectId child, std::string ref)
    : d_child(child), d_ref(std::move(ref)) {}

void NestedContent::attach(PullService& pull, Backend& backend, NestedResolver resolver,
                           const DocRoot& doc) {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  // A NEW pin re-keys the metadata (doc 05:15-16); re-injecting the very snapshot
  // the memo already holds does not (Decision 3). The comparison reads the MEMO's
  // recorded snapshot, not `d_doc`: the runtime binder detaches at the end of every
  // frame (which nulls `d_doc`) and re-attaches the same pin on the next one, so
  // keying off `d_doc` would re-key on every frame of an export and make
  // `metadata_recomputes()` grow linearly with frame count.
  const bool repinned = d_memo.doc != &doc || d_memo.revision != doc.revision();
  d_pull = &pull;
  d_backend = &backend;
  d_resolver = std::move(resolver);
  d_doc = &doc;
  if (repinned) {
    d_memo.valid = false;
  }
}

void NestedContent::detach() noexcept {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  d_pull = nullptr;
  d_backend = nullptr;
  d_resolver = nullptr;
  d_doc = nullptr;
  // The memo is deliberately left intact: it is a pure function of the child's
  // aggregate revision, so re-attaching the same pin reuses it (Decision 3/4).
}

bool NestedContent::attached() const noexcept {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  return d_pull != nullptr && d_backend != nullptr && d_doc != nullptr &&
         static_cast<bool>(d_resolver);
}

// --- metadata (composed + memoized on the pinned aggregate revision) ----------

void NestedContent::ensure_memo() const {
  // Caller holds the shared memo lock (`memo_mutex`).
  if (d_doc == nullptr) {
    // No live snapshot to key against, and a released one must NOT be dereferenced.
    // Two ways to be here, both answered from the memo:
    //
    // DETACHED (the binding scope was released): the memo survives `detach` precisely
    // so a metadata query after teardown is still honest (Constraint 7).
    //
    // NEVER ATTACHED: describing an unbound nested content is NOT a programming error,
    // it is the runtime's normal order of operations. The drivers build their
    // `PullService`'s identity map by walking `inputs()` over the pinned graph
    // (`pull_identity.cpp`), and that service is an INPUT to `bind_operators` -- so the
    // very content the binder is about to attach is necessarily described once while
    // still unattached. With no snapshot there is no child membership to read, so the
    // honest answer is the empty placeholder an unresolved child already gets (doc
    // 05:50-52): nothing to show, no child edges. It is keyed to NO snapshot
    // (`doc == nullptr`), so the binder's `attach` re-keys it as a matter of course,
    // and it is not counted as a recompute -- it memoizes no revision.
    if (!d_memo.valid) {
      d_memo = Memo{};
      d_memo.bounds = Rect{0.0, 0.0, 0.0, 0.0};
      d_memo.stability = Stability::Static;
      d_memo.valid = true;
    }
    return;
  }
  const std::uint64_t revision = d_doc->revision();
  if (d_memo.valid && d_memo.doc == d_doc && d_memo.revision == revision) {
    return; // a stable aggregate revision returns the cached values (doc 05:14)
  }

  ++d_metadata_recomputes;
  d_memo = Memo{};
  d_memo.revision = revision;
  d_memo.doc = d_doc;
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
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  ensure_memo();
  return d_memo.bounds;
}

Stability NestedContent::stability() const {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  ensure_memo();
  return d_memo.stability;
}

std::optional<TimeRange> NestedContent::time_extent() const {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  ensure_memo();
  return d_memo.time_extent;
}

std::span<const ContentRef> NestedContent::inputs() const {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  ensure_memo();
  // The storage is stable for the pinned revision (a fixed pin never re-keys);
  // the span views the memo's own vector in declared order (content.hpp:287-289).
  return d_memo.input_refs;
}

Rect NestedContent::map_input_damage(std::size_t input, const Rect& rect) const {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
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
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  return d_metadata_recomputes;
}

// --- rendering (the synthetic viewport; "rendering is recursion", doc 05:24) --

bool NestedContent::compose_child_layer(const LayerRecord& layer, const Affine& camera,
                                        const Rect& device_rect, const RenderRequest& request,
                                        Backend& backend, Surface& target) const {
  // Every early return below is a layer that contributes NOTHING and is DONE
  // contributing nothing -- culled, unresolved, or unstorable. Each is a final
  // answer no later pass improves on, so each is exact (`true`), matching the
  // exact-placeholder posture `render` already takes for an unresolved child
  // composition. Only a DEFERRED pull (below) is transient and answers `false`.
  if (!layer.visible() || layer.opacity <= 0.0) {
    return true;
  }
  // Span cull before descending (doc 11:21,73-74; Decision D3): a child layer whose
  // half-open span [in, out) does not contain the request instant contributes
  // nothing and is NOT descended -- zero pulls, exactly the flat walk's discipline
  // (`tile_planning.cpp:390`) and the audio twin's (`mix_child_layer`, doc
  // 12:200-206). The default all() span is always present, so this gate is
  // byte-neutral for a layer with no temporal placement.
  if (!present_in_span(layer.span, request.time)) {
    return true;
  }
  Content* content = d_resolver ? d_resolver(layer.content) : nullptr;
  if (content == nullptr) {
    return true; // unresolved layer: placeholder (nothing) for this layer (doc 05:50)
  }

  // Compose the synthetic camera (child-local -> device) with the layer's
  // embedding transform, then invert to map the visible device region back into
  // the layer's content-local space (the pull contract, doc 03; doc 04 culls).
  const Affine composed = compose(camera, layer.transform);
  const std::optional<Affine> inv = composed.inverse();
  if (!inv.has_value()) {
    return true; // degenerate placement: cull (doc 04)
  }

  Rect region = inv->map_rect(device_rect);
  if (const std::optional<Rect> b = content->bounds(); b.has_value()) {
    region = region.intersect(*b);
  }
  if (region.empty()) {
    return true;
  }
  // Re-project the bounds-clipped region forward to device and clip to the
  // visible device rect: a floating-point sliver at the half-open bounds edge
  // (content just outside declared bounds surviving `intersect` by rounding)
  // maps to zero device area and is culled here, so bounds honesty holds exactly
  // -- the doc 04 visibility/sub-pixel cull expressed robustly.
  if (composed.map_rect(region).intersect(device_rect).empty()) {
    return true;
  }

  const double scale = composed.max_scale();
  if (!(scale > 0.0) || !std::isfinite(scale)) {
    return true;
  }
  const int temp_width = static_cast<int>(std::ceil(region.width() * scale));
  const int temp_height = static_cast<int>(std::ceil(region.height() * scale));
  if (temp_width <= 0 || temp_height <= 0) {
    // Sub-pixel cull (doc 04) -- also the guaranteed termination of a <1x Droste
    // cycle, which bottoms out here after finitely many turns (doc 05:61-65).
    return true;
  }

  expected<std::unique_ptr<Surface>, SurfaceError> temp_result =
      backend.make_surface(temp_width, temp_height, target.format());
  if (!temp_result.has_value()) {
    return true; // backend cannot store the working format: cull (doc 09)
  }
  Surface& temp = **temp_result;
  backend.clear(temp, 0.0F, 0.0F, 0.0F, 0.0F);

  // Retime the descent (doc 11:66-71, 217-218; Decision D2): remap the request's
  // time instant through this edge's time map to child-local time --
  // `(request.time - in) * rate + offset` -- exactly as the flat walk
  // (`tile_planning.cpp:405`) and the audio twin (`mix_child_layer`) do. The map is
  // re-derived from this ONE edge every descent and never accumulated down the tree
  // (doc 11:45-48, 185-191; doc 04:113-114); nested recursion composes boundaries,
  // in exact rational arithmetic rounded once at the leaf. The identity default map
  // (rate 1/1, in/offset 0) returns `request.time` unchanged, so a still layer is
  // byte-exact. A negative rate remaps a visual instant cleanly (reverse playback):
  // unlike the audio twin's `num <= 0` cull -- an audio-stream-direction concern
  // (doc 12:118) -- a visual instant has no direction, so reverse is first-class
  // (Decision D4). Only `evaluate` overflow culls (Decision D5).
  const expected<Time, TimeError> local_time = layer.time_map.evaluate(request.time);
  if (!local_time.has_value()) {
    return true; // unrepresentable local time: draw nothing (doc 11:52-56, D5)
  }

  // The sub-request carries the outer request's snapshot, exactness, and deadline
  // VERBATIM (doc 05:93-101, constraint 2) -- never reset, recomputed, or
  // sub-budgeted per level. Only region/scale/target and now the retimed `time` are
  // the layer's own.
  const RenderRequest sub{
      region, scale, *local_time, request.snapshot, temp, request.exactness, request.deadline};

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
    //
    // NOT exact: unlike every cull above, this placeholder is TRANSIENT -- the
    // dispatched render lands in the cache and a later pass composes the real
    // pixels. Reporting `true` here would let the caller cache this pass's
    // missing-layer tile as a fresh exact hit, which the offline driver's
    // `Exactness::Exact` second pass would then serve instead of re-rendering --
    // permanently freezing the deferred layer out of the frame.
    done->cancel();
    return false;
  }
  const std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return true; // budget-exceeded / failed pull: final placeholder for this layer
  }
  const RenderResult result = **settled;

  // temp pixel (i, j) covers local (region origin + (i, j) / achieved): map temp
  // space through content-local space to device space (mirrors render_layer).
  const Affine temp_to_dst = compose(
      composed, compose(Affine::translation(region.x0, region.y0),
                        Affine::scaling(1.0 / result.achieved_scale, 1.0 / result.achieved_scale)));
  backend.composite(target, temp, temp_to_dst, layer.opacity);

  // Fold the pulled layer's OWN exactness (doc 09): a child served from a coarse
  // rung composites its pixels but leaves the composition inexact.
  return result.exact;
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

  // The nesting boundary is a conversion point (doc 07 rule 4). The child's
  // layers are the CHILD composition's layers, and rule 2 puts all compositing in
  // the composition's own working space -- so they blend in the child's declared
  // working space, and it is the child's COMPOSED OUTPUT that converts, exactly
  // once, into the parent's. Concretely: on a mismatch, compose into a
  // child-tagged intermediate the size of the target (every per-layer temp then
  // follows the intermediate's tag for free -- `compose_child_layer` already
  // allocates at its target's format), then convert that once at the end.
  //
  // Converting each per-layer temp into the PARENT's format instead would be a
  // smaller change and is wrong: it blends the child's layers in the parent's
  // space (contradicting rule 2 -- a child declaring the 8-bit sRGB fast mode is
  // ASKING for its layers to blend there, artifacts and all) and costs one
  // conversion per layer instead of one per render.
  //
  // Homogeneous trees pay nothing (doc 07:34-35): equal tags take exactly the
  // pre-conversion path -- no intermediate, no extra clear, no conversion, the
  // request's target composed into directly.
  std::unique_ptr<Surface> intermediate;
  Surface* composed_into = &target;
  if (!(comp->working_space == target.format())) {
    expected<std::unique_ptr<Surface>, SurfaceError> boundary =
        backend.make_surface(target.width(), target.height(), comp->working_space);
    if (!boundary.has_value()) {
      // The backend cannot store the child's declared working space (errors as
      // values, doc 09:55-60). Honest empty pixels over the already-cleared
      // target -- the same placeholder the unresolved child above and the
      // unstorable per-layer temp take. Not a `GraphDiagnostic`: that lives in
      // the sibling `compositor`, an L4->L4 edge doc 17:41-44 forbids.
      return RenderResult{request.scale, true};
    }
    intermediate = std::move(*boundary);
    composed_into = intermediate.get();
    backend.clear(*composed_into, 0.0F, 0.0F, 0.0F, 0.0F);
  }

  // Synthetic viewport (doc 05:24): the camera maps child-composition-local
  // coordinates to device (target) pixels, derived from the request's
  // region-to-surface mapping -- device = scale * (local - region.origin). The
  // intermediate is target-sized, so the device geometry is the same either way.
  const Affine camera = compose(Affine::scaling(request.scale, request.scale),
                                Affine::translation(-request.region.x0, -request.region.y0));
  const Rect device_rect =
      Rect::from_size(static_cast<double>(target.width()), static_cast<double>(target.height()));

  // Bottom-to-top over the child's members at the pinned version (doc 02/05:71-75)
  // -- membership read from the frozen `DocRoot`, so a Droste scene sees the same
  // revisions on every visit within the frame.
  // The composed output is exact only if EVERY child layer contributed exactly --
  // doc 09's honest-exactness AND fold, the same one `PullServiceImpl::pull` runs
  // across a region's covering tiles (`region_exact`). A layer whose pull deferred
  // to a worker leaves this render a transient placeholder, so the tile it produces
  // must NOT be cached as fresh-exact.
  bool exact = true;
  d_doc->for_each_layer_in(d_child, [&](ObjectId layer_id) {
    const LayerRecord* layer = d_doc->find_layer(layer_id);
    if (layer == nullptr) {
      return;
    }
    // Composite FIRST, fold second: every layer must be composed regardless of an
    // already-false `exact`, so the call can never sit on the right of a `&&`.
    const bool layer_exact =
        compose_child_layer(*layer, camera, device_rect, request, backend, *composed_into);
    exact = exact && layer_exact;
  });

  // The composed output converts ONCE into the parent's working space (doc 07
  // rule 4) -- same geometry, replacing every target pixel. A deferred layer left
  // its region premultiplied-transparent in the intermediate, and (0,0,0,0)
  // converts to premultiplied transparent in every format, so the async
  // placeholder stays a placeholder across the boundary.
  if (intermediate != nullptr) {
    backend.convert(target, *intermediate);
  }

  return RenderResult{request.scale, exact};
}

// --- audio (the synthetic monitor; "the aggregate revision covers audio", 12:208)

AudioFacet* NestedContent::audio() { return &d_audio_facet; }

std::optional<TimeRange> NestedContent::NestedAudioFacet::audio_extent() const {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
  d_owner->ensure_memo();
  return d_owner->d_memo.audio_extent;
}

Stability NestedContent::NestedAudioFacet::audio_stability() const {
  const std::lock_guard<std::recursive_mutex> lock(memo_mutex());
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

  // Spatial branch (doc 12:167-206). The identical branch to the L4 `mix_layer` twin
  // (`mix.cpp`), keyed off the L3 contract's `request.spatial` context (Decision D1)
  // and NEVER the L4 `MixPolicy` enum -- so a nested subtree pulled by the mixer
  // spatializes and accumulates its own contributors, and the SUB-AUDIBLE CULL
  // terminates a Droste scene at its natural depth (doc 12:200-206). Absent => Flat
  // (byte-identical). The walk stays duplicated between L3 and L4 exactly as the Flat
  // and visual walks are (doc 17:41 Decision).
  const bool spatial_mode = request.spatial.has_value();
  std::optional<Spatialization> child_spatial;
  float edge_atten = 1.0F;
  SpatialPanGains pan;
  if (spatial_mode) {
    const Spatialization& sp = *request.spatial;
    const Affine composed = compose(sp.listener, layer.transform);
    edge_atten = spatial_edge_atten(layer.transform);
    if (sp.accum_atten * edge_atten < sp.sub_audible) {
      return; // sub-audible: not pulled, not descended (recursion terminator, D4)
    }
    pan = spatial_pan_gains(composed, sp.viewport_w);
    child_spatial = Spatialization{composed, sp.viewport_w, sp.viewport_h,
                                   sp.accum_atten * edge_atten, sp.sub_audible};
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
      child_spatial,     // the descended Spatial context (nullopt in Flat mode)
  };

  // Pull through the injected service, NEVER `af->render_audio` directly (doc 13
  // operator rule, audio twin): block-cache serve, worker dispatch, and the
  // recursion-depth backstop are the service's, exactly as the visual descent
  // relies on `pull` (constraint 2, 7).
  auto done = std::make_shared<AudioCompletion>();
  d_pull->pull_audio(content, child_req, done);
  if (!done->settled()) {
    // The service deferred to a worker (a miss): this pass mixes silence for the
    // layer (doc 05:50-52), exactly as the visual descent shows the placeholder --
    // and, exactly as the visual descent does (`compose_child_layer` returns false),
    // it marks the composed block INEXACT. The silence is TRANSIENT: the child's
    // samples land in the block cache and a later pass must mix them, which an
    // exact-flagged block would prevent forever (doc 13:122-144).
    done->cancel();
    exact = false;
    return;
  }
  const std::optional<expected<AudioResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return; // budget-exceeded / unavailable pull (a Droste backstop): silence
  }
  const AudioResult cr = **settled;

  // Below-rate reconstruction (kinds.nested_audio_resampling). A child that bottoms
  // out below the composed rate conveyed only `achieved_rate` of genuine
  // information in the caller-sized discovery block -- a baseline nearest/hold.
  // Rather than read that hold, re-request the child's GENUINE native samples over
  // the same child-local window at its native rate (a second, block-cache-served
  // pull) and band-limit-reconstruct them up to `child_rate` with the deterministic
  // `arbc::media` windowed-sinc polyphase kernel -- exactly `frames` samples that
  // then feed the UNCHANGED 1:1 additive mix below. A rate-honoring child (every
  // homogeneous reference scene) skips this entirely and keeps its byte-exact 1:1
  // placement, so the pre-existing goldens reproduce (Constraint 3). The kernel
  // improves the sample VALUES only; the achieved_rate/exact honesty math below is
  // untouched, never fabricated (doc 12:24-25,100-104; Constraints 2,4,5,6).
  const float* mix_src = child_buf.data();
  std::vector<float> resampled_buf;
  if (cr.achieved_rate > 0 && cr.achieved_rate < child_rate) {
    const std::uint32_t native_rate = cr.achieved_rate;
    const std::int64_t fpf_native =
        Time::flicks_per_second / static_cast<std::int64_t>(native_rate);
    // Native frames spanning the same child-local window at the native rate. Output
    // frame n reads native position `n * native_rate / child_rate` (< frames), so
    // this count covers every in-range tap; taps past the block read as zero (the
    // kernel's defined edge convention). One rounding at the leaf is the kernel's
    // (doc 11:216-234) -- the rate is never accumulated across depth.
    const std::uint32_t native_frames = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(frames) * native_rate / child_rate + 1);
    std::vector<float> native_buf(static_cast<std::size_t>(native_frames) * in_ch, 0.0F);
    AudioBlock native_block{native_buf.data(), native_frames, child_layout, native_rate};
    const AudioRequest native_req{
        TimeRange{*child_start, Time{child_start->flicks +
                                     static_cast<std::int64_t>(native_frames) * fpf_native}},
        native_rate,
        child_layout,
        native_block,
        request.exactness, // carried verbatim (Constraint 9)
        request.snapshot,  // carried verbatim (Constraint 9)
        child_spatial,     // same context as the discovery pull (leaves ignore it)
    };
    auto native_done = std::make_shared<AudioCompletion>();
    d_pull->pull_audio(content, native_req, native_done);
    if (native_done->settled()) {
      const std::optional<expected<AudioResult, RenderError>> native_settled = native_done->take();
      if (native_settled.has_value() && native_settled->has_value()) {
        resampled_buf.assign(static_cast<std::size_t>(frames) * in_ch, 0.0F);
        AudioBlock resampled_block{resampled_buf.data(), frames, child_layout, child_rate};
        resample_audio(native_block, resampled_block);
        mix_src = resampled_buf.data();
      }
    } else {
      native_done->cancel(); // a deferred native pull: keep the baseline block
    }
  }

  // The mix. Absent `request.spatial` => the Flat additive path (byte-identical);
  // present => the Spatial pan/attenuation branch, identical to the L4 `mix_layer`
  // twin (`mix.cpp`), so a nested subtree spatializes on the same footing as the root.
  const float gain = static_cast<float>(layer.gain);
  if (spatial_mode) {
    // Spatial mix (doc 12:167-206): mono-collapse to a point source, attenuate once
    // by this layer's per-edge composed scale (D2), place by the square-root
    // constant-power pan (D3) with the grouping `((gain * edge) * pan[c]) * m`; mono
    // output panwise is attenuation only.
    const float ge = gain * edge_atten;
    for (std::uint32_t f = 0; f < frames; ++f) {
      float m = 0.0F;
      if (in_ch == 1) {
        m = mix_src[f];
      } else {
        m = 0.5F * (mix_src[static_cast<std::size_t>(f) * in_ch] +
                    mix_src[static_cast<std::size_t>(f) * in_ch + 1]);
      }
      if (out_ch == 2) {
        request.target.samples[static_cast<std::size_t>(f) * 2] += (ge * pan.gl) * m;
        request.target.samples[static_cast<std::size_t>(f) * 2 + 1] += (ge * pan.gr) * m;
      } else {
        for (std::uint32_t c = 0; c < out_ch; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * out_ch + c] += ge * m;
        }
      }
    }
  } else {
    // Additive Flat-mode mix (doc 12:129-130): contribution = gain * child, summed
    // into the target, remixed to the request layout. Placement is 1:1 -- `mix_src`
    // carries exactly `frames` samples at `child_rate` (the honoring child's block,
    // or the reconstructed block above).
    for (std::uint32_t f = 0; f < frames; ++f) {
      for (std::uint32_t c = 0; c < out_ch; ++c) {
        float s = 0.0F;
        if (in_ch == out_ch) {
          s = mix_src[static_cast<std::size_t>(f) * in_ch + c];
        } else if (in_ch == 1) {
          s = mix_src[f]; // mono child -> every request channel
        } else {
          // stereo child -> mono request: average the channels (baseline downmix).
          s = 0.5F * (mix_src[static_cast<std::size_t>(f) * 2] +
                      mix_src[static_cast<std::size_t>(f) * 2 + 1]);
        }
        request.target.samples[static_cast<std::size_t>(f) * out_ch + c] += gain * s;
      }
    }
  }

  // achieved_rate / exact honesty (Constraint 6). A child honoring the composed
  // rate keeps the boundary at the request rate; a below-rate child (whose samples
  // are now band-limit-reconstructed above, not held) still lowers the aggregate
  // and marks it inexact -- the windowed-sinc reconstruction improves the sample
  // VALUES' fidelity but creates no information, so it must never raise
  // achieved_rate toward the request rate nor report exact (doc 12 rate-honesty).
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
