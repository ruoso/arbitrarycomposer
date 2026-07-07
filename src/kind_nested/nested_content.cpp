#include <arbc/kind_nested/nested_content.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace arbc {

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
  // order `inputs()` exposes) plus their embedding transforms.
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
  });

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
    d_memo.bounds =
        unbounded ? std::optional<Rect>{} : std::optional<Rect>{acc.value_or(Rect{0.0, 0.0, 0.0, 0.0})};
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
  const RenderRequest sub{region,           scale, request.time,      request.snapshot,
                          temp,             request.exactness,        request.deadline};

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

} // namespace arbc
