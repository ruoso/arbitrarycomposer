#pragma once

#include <arbc/base/ids.hpp>                      // ObjectId
#include <arbc/base/time.hpp>                     // Time
#include <arbc/base/transform.hpp>                // Affine
#include <arbc/compositor/anchored_viewports.hpp> // rebase, Reanchor, RebaseNeed, reanchor_camera
#include <arbc/compositor/compositor.hpp> // Viewport, ContentResolver, Backend, SurfacePool, Surface
#include <arbc/compositor/counters.hpp>   // TileCache
#include <arbc/model/damage.hpp>          // DamageSink, Damage, damage_add
#include <arbc/model/model.hpp>           // Model, DocRoot, DocStatePtr
#include <arbc/runtime/damage_router.hpp> // DamageRouter (optional fan-out attach)
#include <arbc/runtime/interactive.hpp>   // InteractiveRenderer
#include <arbc/runtime/transport.hpp>     // Transport

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// The host-facing per-viewport object for `arbc::runtime` (L5, doc 17:60): the
// object a host application drives, turning the stateless one-frame
// `InteractiveRenderer` (`interactive.hpp`) into a live viewport. It COMPOSES
// existing seams -- it does not re-implement the render loop, the rebase math, the
// transport, or audio-clock mastering (refinement Decision 1). It owns the
// cross-frame state doc 04's anchored-camera strategy "leaks into the public API"
// (doc 04:81-84) and that `interactive` / the pure `rebase()` deliberately defer
// here (`interactive.hpp:70,89-91`, `anchored_viewports.hpp:28-30,101-107`):
//
//   1. the anchored camera as the `(anchor node, matrix)` pair (doc 04:81-84),
//      carried across frames together with an anchor-path STACK so the viewport
//      re-anchors both inward (zoom in) and outward (zoom out, Decision 2);
//   2. an owned `Transport` and the policy that decides whether it FREE-RUNS on
//      the injected real clock or CHASES an audio master via a `playhead_source`
//      (Decision 5, doc 01:103-106);
//   3. a `DamageSink` accumulator installed on the `Model` for this object's
//      lifetime (RAII), draining accumulated model damage into `render_frame`
//      each step (doc 01:134-141).
//
// `step()` drives one `InteractiveRenderer::render_frame` pass, surfacing the
// re-anchor event and the follow-up-owed decision as a poll-style return value
// (Decision 3, matching `FrameOutcome`); the host owns re-invocation. It exposes
// the seams a monitor attaches to: `transport()` (a `DeviceMonitor`/`ExportMonitor`
// binds to it) and `camera_source()` (wired into `DeviceMonitorConfig::camera_source`
// for Spatial camera-follow, Constraint 6). No new monitor class -- the audio
// monitors already master/follow (doc 12).
//
// Concurrency: single-owner value state exactly like the camera/transport
// (`transport.hpp:25-30`). The ONLY cross-thread read is the audio master's
// existing lock-free `playhead_snapshot()`, sampled through the injected
// `playhead_source`; this object adds no new shared mutable state and carries no
// new TSan obligation (Decision 5, doc 16 concurrency policy).

namespace arbc {

// The document-side collaborators the `Document&` constructor binds against. Forward-declared,
// not included: `document_serialize.hpp` (which names `KindBridge` and the settle free function)
// is included only by `host_viewport.cpp`, so this header's weight does not regress. All three
// are inside `runtime`'s existing closure -- `Document`/`KindBridge` are `runtime` itself and
// `Registry` is `contract` -- so the binding adds NO levelization edge (doc 17:24,:60).
class Document;
class KindBridge;
class Registry;

class HostViewport {
public:
  // The injected wall-clock source -- the SAME kind the renderer uses
  // (Constraint 4/8). Empty selects `steady_clock::now`; a test hands a fake clock
  // so the free-run transport advance is deterministic and wall-clock-free.
  using Clock = InteractiveRenderer::Clock;

  // Initial per-viewport state.
  struct Config {
    Viewport viewport{};                // initial device rect + camera + anchor
    Time transport_start{Time::zero()}; // the owned transport's starting instant
    std::chrono::steady_clock::duration budget{
        std::chrono::milliseconds(16)}; // per-frame compute budget
    // Optional fan-out attach (refinement Decision 2, Constraint 4). When set, the
    // viewport registers its `DamageAccumulator` with the router (holding the RAII
    // `Registration`) and does NOT touch `Model::set_damage_sink`; when null, it
    // keeps today's direct single-slot install. The two paths are mutually
    // exclusive by construction -- the router must outlive every viewport that
    // registers with it.
    DamageRouter* router{nullptr};

    // The external-arrival settle hook (`runtime.async_external_load` Decision 7): the bytes
    // behind a nested content's `params.ref` may arrive from a deferring `AssetSource` long
    // after the document loaded, and installing them is a WRITER-THREAD publish. `step()`
    // calls this FIRST, ahead of the pin and the damage drain, because an arrival IS damage
    // and this is doc 02 step 1 -- the install's own commit flushes damage naming the
    // embedding content into `d_sink`, which the same step then drains, so the newly-arrived
    // child is composited by the very frame that settled it.
    //
    // A `std::function` rather than a `Document*` because the settle needs the document's
    // `KindBridge` and `Registry` too, and this object deliberately holds neither a
    // `Document` nor a codec table (it takes a `Model&` + a `ContentResolver`). Empty -- the
    // default -- means a host with no external references pays nothing at all.
    //
    // A host that owns a `Document` does not fill this in: the `Document&` constructor DERIVES
    // it from `(doc, DocumentBinding)` (runtime.host_viewport_document_binding). This stays as
    // the ESCAPE HATCH -- set explicitly, it wins over the derived hook, so a host with a
    // bespoke settle policy keeps one.
    std::function<std::size_t()> settle_external_loads;
  };

  // What one `step()` reports back to the host (Decision 3, poll-style value): the
  // follow-up-owed decision the renderer produced this step, the re-anchor event
  // (`occurred == false` when no re-anchor happened), and the conditioning `need`
  // the pure rebase observed.
  struct StepOutcome {
    bool schedule_follow_up{false};
    Reanchor reanchor{};
    RebaseNeed need{RebaseNeed::none};
  };

  // What a `Document`-bound viewport needs BEYOND the document itself, and nothing more: the
  // codec/kind bridge and the plugin registry the external-arrival settle consumes
  // (`settle_external_loads(doc, bridge, registry)`, `document_serialize.hpp:180`). Both
  // non-null => the settle hook is derived from the document; a default `DocumentBinding{}` --
  // the right shape for a programmatically-built document with no external references --
  // derives none, so such a host still "pays nothing at all" (`Config::settle_external_loads`,
  // below). Pointers, not references, because absence is a real and common state.
  //
  // A separate parameter rather than two `Config` fields (Decision 3): they are meaningless on
  // the `Model&` path, and a `Config` carrying fields one documented constructor silently
  // ignores is a trap.
  struct DocumentBinding {
    KindBridge* bridge{nullptr};
    const Registry* registry{nullptr};
  };

  // Binds the collaborators (references, not owned -- the interactive renderer, the
  // model whose damage this viewport subscribes to, and the caller-persisted render
  // surfaces) and installs the damage sink on `model` for this object's lifetime.
  // `resolve` serves `render_frame`'s content-vtable binding (`document.hpp`).
  //
  // This is the UNIT-TEST seam, and it stays: a test standing up a bare `Model` wants none of
  // a `Document`'s journal, editable binding, or pending-loads queue. A host that owns a
  // `Document` uses the constructor below instead.
  HostViewport(InteractiveRenderer& renderer, Model& model, ContentResolver resolve,
               Backend& backend, SurfacePool& pool, TileCache& cache, Surface& target, Clock clock,
               Config config);

  // Bind this viewport against the `Document` a host actually owns (doc 01 § Viewport,
  // runtime.host_viewport_document_binding). The document supplies the three seams the host
  // would otherwise hand-assemble -- and, having no `Model&` of its own, could not:
  //
  //   * the `ContentResolver`, as `doc.resolve` (the id->Content vtable binding, doc 17:66-72);
  //   * the damage-sink install, on the document's model -- so a commit's damage reaches this
  //     viewport's frame with no `set_damage_sink` call by the host at all. `Config::router`
  //     still selects the fan-out path, exactly as on the `Model&` constructor: with a router
  //     set the viewport registers with IT and never touches the document's single sink slot;
  //   * the external-arrival settle hook, as `settle_external_loads(doc, *binding.bridge,
  //     *binding.registry)`, run at the top of every `step()` (doc 02 step 1: an arrival IS
  //     damage). Derived only when `binding` carries both; an explicitly-set
  //     `Config::settle_external_loads` WINS over the derived one, so a host with a bespoke
  //     settle policy keeps its escape hatch.
  //
  // It DELEGATES to the `Model&` constructor above -- there is no `Document` member, no mode
  // flag, and exactly one code path through a frame (Constraint 2). `doc` must outlive this
  // viewport: the derived resolver and settle hook capture it, the same lifetime contract the
  // other reference collaborators already carry.
  HostViewport(InteractiveRenderer& renderer, Document& doc, DocumentBinding binding,
               Backend& backend, SurfacePool& pool, TileCache& cache, Surface& target, Clock clock,
               Config config);
  ~HostViewport();

  HostViewport(const HostViewport&) = delete;
  HostViewport& operator=(const HostViewport&) = delete;
  HostViewport(HostViewport&&) = delete;
  HostViewport& operator=(HostViewport&&) = delete;

  // --- The anchored camera as `(anchor node, matrix)` (Constraint 1) -----------
  const Viewport& viewport() const noexcept { return d_viewport; }
  const Affine& camera() const noexcept { return d_viewport.camera; }
  ObjectId anchor() const noexcept { return d_viewport.anchor; }
  // Pan/zoom/rotate mutate the camera MATRIX; the anchor changes only via
  // re-anchoring (doc 04:81-84). The new matrix maps the current anchor's local
  // space -> device pixels; the next `step()` rebases it if it left the band.
  void set_camera(const Affine& camera) noexcept { d_viewport.camera = camera; }

  // --- Transport + monitor seams (Constraint 6) --------------------------------
  Transport& transport() noexcept { return d_transport; }
  const Transport& transport() const noexcept { return d_transport; }
  // A live source of the current camera for `DeviceMonitorConfig::camera_source`
  // (Spatial camera-follow, doc 12): returns an L0 `Affine`, so the monitor stays
  // free of any dependency on the L4 compositor `Viewport` (`device_monitor.hpp:74-78`).
  std::function<Affine()> camera_source() {
    return [this] { return d_viewport.camera; };
  }
  // Install (or clear, with an empty function) the audio-mastered playhead source.
  // When SET, `step()` derives `composition_time` from it and NEVER advances the
  // owned transport -- the device monitor is the sole transport mutator (video
  // chases audio, Decision 5, doc 01:103-106). When empty, the viewport free-runs.
  void set_playhead_source(std::function<Time()> source) {
    d_playhead_source = std::move(source);
    d_prev_instant.reset(); // re-baseline the free-run clock on the next free-run step
  }

  // Drive one interactive frame (doc 02:49-71). Samples `composition_time` from the
  // playhead policy, applies the pure `rebase()` result across frames (Decisions 2
  // & 4), drains accumulated model damage into `render_frame`, and honors
  // `FrameOutcome::schedule_follow_up`. Issues ZERO `render_frame` invocations when
  // there is no pending damage, no owed follow-up, and the scene has not moved
  // (Constraint 7, doc 01:140). Returns the poll-style outcome.
  StepOutcome step();

  // --- Behavioral counters (doc 16:54-62; wall-clock-free) ---------------------
  std::uint64_t frames_issued() const noexcept { return d_frames_issued; }
  std::uint64_t transport_advances() const noexcept { return d_transport_advances; }
  std::uint64_t reanchor_events() const noexcept { return d_reanchor_events; }
  // External children installed by the settle hook across every step (zero without one).
  std::uint64_t external_loads_settled() const noexcept { return d_external_loads_settled; }
  // The current anchor-path depth (zoom-in pushes, zoom-out pops); exposed for
  // tests/host observability of the re-anchor stack.
  std::size_t anchor_depth() const noexcept { return d_anchor_path.size(); }
  // The `composition_time` the last issued frame rendered at (Constraint 4): the
  // free-run transport position, or the audio master's chased snapshot. `nullopt`
  // before the first issued frame.
  std::optional<Time> last_frame_time() const noexcept {
    return d_rendered_once ? std::optional<Time>(d_last_render_time) : std::nullopt;
  }

private:
  // The accumulator this viewport installs as the model's `DamageSink`: a commit
  // flushes its unioned damage here (once per commit, `damage.hpp:74-78`); `step`
  // drains it into the frame plan. Single-owner, drained on the driving thread.
  class DamageAccumulator final : public DamageSink {
  public:
    void flush(const std::vector<Damage>& damage) override {
      for (const Damage& d : damage) {
        damage_add(d_set, d);
      }
    }
    std::vector<Damage> drain() {
      std::vector<Damage> out;
      out.swap(d_set);
      return out;
    }

  private:
    std::vector<Damage> d_set;
  };

  // One entry of the runtime-held anchor path (Decision 2): the ancestor anchor a
  // zoom-in descended FROM and the descent edge it crossed (NEW-anchor-local ->
  // ancestor-local). Zoom-out pops the top and rebuilds the camera by inverting the
  // stored edge, restoring the original `(anchor, matrix)`.
  struct AnchorFrame {
    ObjectId anchor{};
    Affine edge{};
  };

  InteractiveRenderer& d_renderer;
  Model& d_model;
  ContentResolver d_resolve;
  Backend& d_backend;
  SurfacePool& d_pool;
  TileCache& d_cache;
  Surface& d_target;
  Clock d_clock;
  std::chrono::steady_clock::duration d_budget;

  Viewport d_viewport;                    // the persistent `(anchor, camera)` pair
  Transport d_transport;                  // the owned per-viewport playback clock
  std::vector<AnchorFrame> d_anchor_path; // the re-anchor stack (zoom-out pops)
  DamageAccumulator d_sink;        // installed on d_model / d_router for this object's lifetime
  DamageRouter* d_router{nullptr}; // set => registered via router; null => direct model slot
  DamageRouter::Registration
      d_registration; // the RAII fan-out registration (inert when d_router null)
  std::function<Time()> d_playhead_source;     // set => audio-mastered chase; empty => free-run
  std::function<std::size_t()> d_settle_loads; // set => drive external arrivals each step

  std::optional<std::chrono::steady_clock::time_point> d_prev_instant; // last free-run clock sample

  // The scene state at the last issued frame, so an unmoved-and-undamaged step
  // issues zero frames (Constraint 7).
  bool d_rendered_once{false};
  Affine d_last_render_camera{};
  ObjectId d_last_render_anchor{};
  Time d_last_render_time{};
  bool d_follow_up_owed{false};

  std::uint64_t d_frames_issued{0};
  std::uint64_t d_transport_advances{0};
  std::uint64_t d_reanchor_events{0};
  std::uint64_t d_external_loads_settled{0};
};

} // namespace arbc
