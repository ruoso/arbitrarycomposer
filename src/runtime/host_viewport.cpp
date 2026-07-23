#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp> // KindBridge, settle_external_loads
#include <arbc/runtime/host_viewport.hpp>

#include "document_access.hpp" // HostViewportDocumentAccess (the Model& behind a Document)

#include <chrono>
#include <optional>
#include <ratio>
#include <utility>
#include <vector>

namespace arbc {
namespace {

// The injected real-elapsed duration expressed as `Time` flicks. `Time` counts
// integer flicks (1/705'600'000 s, `time.hpp:8-14`); a period-`flicks_per_second`
// duration cast is the exact, wall-clock-free conversion of a monotonic-clock
// delta the free-run transport advance consumes (Constraint 4/8).
Time elapsed_flicks(std::chrono::steady_clock::duration delta) {
  using flick_duration =
      std::chrono::duration<std::int64_t, std::ratio<1, Time::flicks_per_second>>;
  return Time{std::chrono::duration_cast<flick_duration>(delta).count()};
}

// Fill in the settle hook the `Document` itself implies, leaving an explicitly-set one alone
// (Decision 3: the host's own policy wins, so the seam `async_external_load` landed is not
// thrown away). With no bridge or no registry there is nothing to settle WITH -- a
// programmatically-built document has no external references -- and the viewport installs no
// hook at all, so an idle step still costs exactly nothing.
HostViewport::Config derive_document_config(Document& doc, HostViewport::DocumentBinding binding,
                                            HostViewport::Config config) {
  if (!config.settle_external_loads && binding.bridge != nullptr && binding.registry != nullptr) {
    config.settle_external_loads = [&doc, bridge = binding.bridge, registry = binding.registry] {
      return arbc::settle_external_loads(doc, *bridge, *registry);
    };
  }
  // The readiness probe travels with the settle hook (issue #13): a step that declines to
  // publish still owes the host the count of what it declined to install, and the document
  // answers that lock-free from any thread. Derived whenever the document can answer -- which
  // is unconditional, unlike the settle hook's need for a bridge and a registry -- so a host
  // that supplies a bespoke settle hook against a `Document` still gets an honest count.
  if (!config.external_loads_ready) {
    config.external_loads_ready = [&doc] { return doc.external_loads_ready(); };
  }
  return config;
}

} // namespace

HostViewport::HostViewport(InteractiveRenderer& renderer, Model& model, ContentResolver resolve,
                           Backend& backend, SurfacePool& pool, TileCache& cache, Surface& target,
                           Clock clock, Config config)
    : d_renderer(renderer), d_model(model), d_resolve(std::move(resolve)), d_backend(backend),
      d_pool(pool), d_cache(cache), d_target(target),
      d_clock(clock ? std::move(clock) : Clock{[] { return std::chrono::steady_clock::now(); }}),
      d_budget(config.budget), d_viewport(config.viewport), d_transport(config.transport_start),
      d_settle_loads(std::move(config.settle_external_loads)),
      d_loads_ready(std::move(config.external_loads_ready)) {
  // Subscribe to model damage for this object's lifetime (Constraint 5, RAII). Two
  // mutually exclusive attach paths (`runtime.damage_router` Constraint 4): with a
  // router supplied, register `&d_sink` with it and hold the move-only
  // `Registration` (the router owns the single `set_damage_sink` slot and fans out
  // to this sink unchanged, `damage.hpp:74-78`); without one, keep the direct
  // single-slot install this object has always used.
  if (config.router != nullptr) {
    d_router = config.router;
    d_registration = d_router->register_sink(d_sink);
  } else {
    d_model.set_damage_sink(&d_sink);
  }
  // Seed the viewport anchor to the document's root composition when the host did
  // not pin one explicitly (compositor.root_composition_frame_walk, doc 05:28-36,
  // Decision 2): the frame walk is composition-scoped, so an unset (invalid)
  // anchor would draw nothing. Lowest-id-wins root, sourced once from the current
  // revision; the per-frame rebase re-picks the anchor as the user zooms.
  if (!d_viewport.anchor.valid()) {
    const DocStatePtr seed = d_model.current();
    ObjectId root_id{};
    const CompositionRecord* root_rec = nullptr;
    if ((*seed).find_first_composition(root_id, root_rec)) {
      d_viewport.anchor = root_id;
    }
  }
}

// The `Document&` binding (runtime.host_viewport_document_binding): derive the resolver, the
// settle hook and the `Model&` from the document, then DELEGATE. Everything below this line --
// the damage-sink install (`Document::set_damage_sink` forwards to exactly the same
// `Model::set_damage_sink` this reaches), the router branch, the whole of `step()` -- is the
// one code path the `Model&` constructor has always run, unchanged.
//
// It additionally RETAINS the document (`runtime.interactive_binder_wiring` Decision 3): the
// fourth seam a document supplies -- the content graph each frame binds -- is the one that
// cannot be reduced to a `std::function`, because `bind_operators` walks
// `Document::for_each_content`. A delegating constructor may not carry mem-initializers, so
// the retention is a body assignment.
HostViewport::HostViewport(InteractiveRenderer& renderer, Document& doc, DocumentBinding binding,
                           Backend& backend, SurfacePool& pool, TileCache& cache, Surface& target,
                           Clock clock, Config config)
    : HostViewport(
          renderer, HostViewportDocumentAccess::model(doc),
          [&doc](ObjectId id) { return doc.resolve(id); }, backend, pool, cache, target,
          std::move(clock), derive_document_config(doc, binding, std::move(config))) {
  d_document = &doc;
  // Hand the settle hook to the DOCUMENT as well (issue #13). `step()` runs it only when the
  // step is itself the writer thread; this is what covers the other case, without asking the
  // host for anything: the document runs it on the writer thread, immediately before the next
  // edit that host makes. So a host that never reads `StepOutcome::external_loads_ready`
  // still installs its arrivals -- at its next edit rather than at its next frame -- and the
  // report is a latency optimization for an IDLE document, not an obligation. Cleared in the
  // destructor; the install is counted, so N viewports over one document is not an
  // out-of-order teardown hazard.
  if (d_settle_loads) {
    doc.set_external_load_settler(d_settle_loads);
    d_settle_owner = &doc;
  }
}

HostViewport::~HostViewport() {
  // Router path: `d_registration`'s destructor unregisters from the router (which
  // outlives this viewport). Direct path: release the model's single slot.
  if (d_router == nullptr) {
    d_model.set_damage_sink(nullptr);
  }
  // Release the auto-settle install (issue #13): the hook captures this viewport's binding, so
  // it must not outlive the viewport. The document decrements; the slot survives while another
  // viewport still holds an install.
  if (d_settle_owner != nullptr) {
    d_settle_owner->set_external_load_settler(nullptr);
  }
}

HostViewport::StepOutcome HostViewport::step() {
  StepOutcome outcome;

  // 0. Settle any external children whose bytes arrived since the last step
  //    (`runtime.async_external_load` Decision 7). This runs BEFORE the pin and before the
  //    damage drain, and both orderings are load-bearing: the install publishes a NEW
  //    revision, so a pin taken first would render the stale one, and it flushes damage
  //    naming the embedding content into `d_sink`, which step 3 drains -- so the frame that
  //    settles an arrival is the frame that composites it, and the placeholder is replaced
  //    live. This IS doc 02 step 1: an arrival is damage, and this is where damage is
  //    collected.
  //
  //    ...but ONLY on the document's writer thread (issue #13). The settle is the one
  //    structural PUBLISH in an otherwise read-only, pin-based frame: it opens a model
  //    transaction, installs the arrived child and commits. Frame planning is
  //    render-thread-confined by design (doc 02 § Threading model), so on a host whose render
  //    loop is not its writer thread, publishing here would give the document a SECOND writer
  //    identity -- which doc 15 § Thread rules forbids outright, and which a host-side mutex
  //    cannot repair (it re-covers accesses, not identity: the lock-free growth path, the
  //    relaxed `high_water`, the writer-thread checkpoint seal are all written against one
  //    mutator). The host cannot fix it from outside either, because this call site is the
  //    library's.
  //
  //    So the viewport ASKS. Same thread as the writer -- the single-threaded host, and every
  //    driver in this tree -- and the settle runs exactly where and when it always did,
  //    byte-identically, with the placeholder still replaced inside the very frame that
  //    observes the arrival. A different thread, and the step publishes nothing at all and
  //    reports the owed install through `StepOutcome::external_loads_ready`; the host settles
  //    on its writer thread, that commit's damage reaches `d_sink` through the ordinary
  //    seam, and the NEXT step composites the arrival. One frame later, one writer.
  if (d_settle_loads && d_model.on_writer_thread()) {
    d_external_loads_settled += d_settle_loads();
  } else if (d_settle_loads && d_loads_ready) {
    outcome.external_loads_ready = d_loads_ready();
  }

  // 1. Sample `composition_time` from the playhead policy (Decision 5). Audio-
  //    mastered: chase the lock-free snapshot, never advance the transport (the
  //    device monitor is its sole mutator). Free-run: advance the owned transport
  //    by the injected real elapsed and sample `position()`.
  Time composition_time;
  if (d_playhead_source) {
    composition_time = d_playhead_source();
  } else {
    const std::chrono::steady_clock::time_point now = d_clock();
    const Time elapsed =
        d_prev_instant.has_value() ? elapsed_flicks(now - *d_prev_instant) : Time::zero();
    d_prev_instant = now;
    const expected<Time, TimeError> advanced = d_transport.advance(elapsed);
    ++d_transport_advances;
    composition_time = advanced.has_value() ? *advanced : d_transport.position();
  }

  // 2. Pin the current version and apply the pure rebase across frames (Decisions
  //    2 & 4). A zoom-in re-anchor is already applied by `rebase`; push the
  //    ancestor + descent edge onto the anchor path. A zoom-out (`need ==
  //    zoom_out`) walks the runtime-held path: pop the top and rebuild the camera
  //    by inverting the stored edge, restoring the original `(anchor, matrix)`.
  const DocStatePtr state = d_model.current();
  const RebaseResult rebased = rebase(*state, d_viewport);
  outcome.need = rebased.need;
  if (rebased.event.occurred) {
    d_anchor_path.push_back(AnchorFrame{rebased.event.from, rebased.edge});
    d_viewport = rebased.viewport;
    outcome.reanchor = rebased.event;
    ++d_reanchor_events;
  } else if (rebased.need == RebaseNeed::zoom_out && !d_anchor_path.empty()) {
    const AnchorFrame frame = d_anchor_path.back();
    // Invert the stored descent edge to re-anchor upward. A degenerate edge culls
    // rather than propagating NaNs (Constraint 9, doc 04:116-117) -- leave the
    // viewport untouched and keep the frame on the path.
    if (const std::optional<Affine> inv = frame.edge.inverse(); inv.has_value()) {
      const ObjectId from = d_viewport.anchor;
      d_anchor_path.pop_back();
      d_viewport.camera = reanchor_camera(d_viewport.camera, *inv);
      d_viewport.anchor = frame.anchor;
      outcome.reanchor = Reanchor{true, from, frame.anchor};
      ++d_reanchor_events;
    }
  }

  // 3. Drain accumulated model damage and decide whether to render. A step with no
  //    pending damage, no owed follow-up, a still scene AND nothing in flight issues
  //    zero `render_frame` invocations (Constraint 7, doc 01:140).
  //
  //    The in-flight term is load-bearing since `runtime.interactive_worker_count_default`
  //    made the shipped pool non-zero. A frame that dispatches a leaf miss to a worker and
  //    then reaches its deadline returns having composited a DEGRADED tile, with the real
  //    render still running and no follow-up scheduled (the arrival has not settled, so
  //    there is no arrival damage to schedule one from -- that is what `pending()` is
  //    for). Without this term a host whose scene then goes still -- a paused playhead, an
  //    unmoved camera -- would issue no further frame, and the tile the worker is at that
  //    moment painting would never be reaped or composited: the viewport would sit on the
  //    degraded frame until something unrelated moved. At `worker_count == 0` the case did
  //    not exist (`submit` IS the render, so nothing is ever left in flight), which is
  //    exactly why the term was not needed before and is not optional now.
  //
  //    It does not weaken `02-architecture#idle-viewport-issues-no-frames`: a genuinely
  //    idle viewport has an EMPTY refinement queue, so the early-out still fires and a
  //    still scene still costs nothing.
  //
  //    The `d_rendered_once` gate is the BOOTSTRAP frame (doc 01:116-123 -- binding is
  //    the host's single wiring step; doc 02 § "A camera change is device damage"): a
  //    viewport bound to a document whose commits all predate the sink install drains
  //    no damage, but it has never shown the scene, so its first step must reach
  //    `render_frame` -- whose first-frame path plans the whole viewport as the
  //    degenerate case of "no previous mapping". A never-rendered viewport is not
  //    still, it is stale; idleness is measured AFTER the first composite.
  std::vector<Damage> damage = d_sink.drain();
  const bool scene_moved = d_rendered_once && (d_viewport.camera != d_last_render_camera ||
                                               d_viewport.anchor != d_last_render_anchor ||
                                               composition_time != d_last_render_time);
  const bool work_in_flight = !d_renderer.pending().tiles.empty();
  if (d_rendered_once && damage.empty() && !d_follow_up_owed && !scene_moved && !work_in_flight) {
    return outcome; // idle: a still scene costs nothing
  }

  // 4. Hand the frame the document + the pin it is compositing, so it can bind its
  //    operators to its own frame-local pull service (`runtime.interactive_binder_wiring`,
  //    doc 13). This viewport holds the pin (step 2) but no `PullService` -- the only one
  //    that exists is `render_frame`'s frame-local one -- so the BIND happens there and
  //    this is the plumbing. Null document (the `Model&` constructor) => a default
  //    `FrameBinding` => no binding, byte-for-byte today's frame.
  FrameBinding frame_binding;
  if (d_document != nullptr) {
    frame_binding = FrameBinding{d_document, state};
  }
  const InteractiveRenderer::FrameOutcome frame =
      d_renderer.render_frame(*state, d_resolve, d_viewport, d_cache, d_backend, d_pool, d_target,
                              damage, composition_time, d_budget, frame_binding);
  ++d_frames_issued;
  d_follow_up_owed = frame.schedule_follow_up;
  d_rendered_once = true;
  d_last_render_camera = d_viewport.camera;
  d_last_render_anchor = d_viewport.anchor;
  d_last_render_time = composition_time;
  outcome.schedule_follow_up = frame.schedule_follow_up;
  return outcome;
}

} // namespace arbc
