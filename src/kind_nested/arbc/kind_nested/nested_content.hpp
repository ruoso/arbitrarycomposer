#pragma once

#include <arbc/contract/content.hpp>
#include <arbc/model/model.hpp>
#include <arbc/surface/backend.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace arbc {

// Resolve a model content `ObjectId` to its bound `Content*`. The id->Content
// binding lives in runtime (doc 17: records hold opaque content ids), so nested
// receives it through `attach`; its tests drive it directly, exactly as
// `kinds.raster` drives its sink registration (production auto-wiring is the
// deferred `kinds.nested_runtime_binding`).
using NestedResolver = std::function<Content*(ObjectId)>;

// Reference kind org.arbc.nested (doc 05): one composition embedded inside
// another as ordinary content -- the design's keystone, the proof that the layer
// contract is arbitrary (the compositor itself lives behind it) and that deep
// zoom is structurally infinite. A nested content wraps a reference to a CHILD
// composition; its local space IS the child's composition space (the parent
// layer's transform is the only mapping). It renders by running the child
// through a synthetic viewport -- "rendering is recursion" (doc 05:24) -- pulling
// each child layer's content through the injected `PullService` (so cache-first
// serve, worker dispatch, snapshot/deadline inheritance, aggregate revision,
// cycle budget, and async all come from that one service, never rebuilt) and
// source-over compositing via the `Backend`.
//
// Scope (doc 05 machinery): the VISUAL facet over BOTH homogeneous and
// HETEROGENEOUS working-space trees, two-level caching (the service's leaf cache
// plus the parent's cache of nested's composed result), cycle/Droste
// termination, and snapshot consistency. The nesting boundary is a conversion
// point (doc 07 rule 4): a child composition declaring a different working space
// composites its own layers in its OWN space -- into a child-tagged intermediate
// -- and its composed output converts once into the parent's working space
// through `Backend::convert`; a child declaring the parent's space pays nothing
// (no intermediate, no conversion, doc 07:34-35). The AUDIO facet
// (`kinds.nested_audio`, doc 12:202-208) is the 1D-signal twin over the same
// scaffold: `render_audio` re-expresses the per-layer descent for samples exactly
// as `render` does for pixels -- a synthetic MONITOR mixing each audible child
// layer's audio (pulled through `PullService::pull_audio`, time-mapped +
// gain-scaled, additively mixed) the way the synthetic viewport composites its
// pixels; its rate/layout boundary conversion is the deferred
// `kinds.nested_audio_resampling`.
//
// Levelization (doc 17:59): the kind depends only on `contract`. It re-expresses
// the compositor's per-layer cull/compose loop directly (it may not name the
// sibling `compositor`, an L4->L4 edge) and reuses only the L3 `PullService`
// interface for the heavy machinery.
class NestedContent final : public Content {
public:
  // Wrap a reference to child composition `child`. Unattached until `attach`, which
  // injects the whole engine: `render` asserts attachment. The DESCRIPTION methods do
  // not -- an unattached content has no snapshot to read child membership from, so it
  // describes as the empty placeholder (no bounds, Static, no inputs). The runtime
  // relies on that: it builds the `PullService`'s identity map by walking `inputs()`
  // over the pinned graph, and that service is an input to `bind_operators`.
  explicit NestedContent(ObjectId child);

  // Inject the render-time services (mirrors the raster attach seam):
  //  - `pull`     the shared PullService every child render travels through;
  //  - `backend`  the L2 seam nested clears/allocates/composites through;
  //  - `resolver` id->Content* over the pinned model's content ids;
  //  - `doc`      the pinned document version nested reads membership from
  //               (`for_each_layer_in`), so a Droste scene is self-consistent
  //               within a frame (doc 05:71-75). Re-attaching a newer pin after
  //               an edit re-keys the memoized metadata (doc 05:15-16).
  // The `DocRoot` and services must outlive this content (the runtime binder's
  // scope owns the pin for exactly that reason).
  //
  // Re-keying is CONDITIONAL on the snapshot actually being new
  // (`kinds.nested_runtime_binding` Decision 3): re-injecting the very same
  // snapshot the memo was computed against leaves the memo valid. The production
  // drivers bind per frame against a pin taken once per export
  // (`offline_sequence.cpp`), so an unconditional re-key would make
  // `metadata_recomputes()` grow linearly with frame count and break
  // `05-recursive-composition#nested-metadata-memoized-on-aggregate-revision` on
  // the driver path. A newer pin (a different `DocRoot`, or the same address at a
  // different revision) still re-keys, so doc 05:15-16 is preserved.
  void attach(PullService& pull, Backend& backend, NestedResolver resolver, const DocRoot& doc);

  // Teardown twin of `attach` (`kinds.nested_runtime_binding` Constraint 7,
  // mirroring `FadeContent::detach`): clear every borrowed pointer so no render
  // after the runtime's binding scope ends dereferences a released service or a
  // dropped snapshot. Safe on a never-attached content and safe to call twice.
  //
  // It deliberately does NOT invalidate the memo: the memoized metadata is a pure
  // function of the child's aggregate revision, so preserving it across a
  // detach/re-attach cycle at the SAME pin is what makes the per-frame re-bind
  // free (Decision 3/4). A metadata query while detached answers from that memo
  // rather than dereferencing the released snapshot.
  void detach() noexcept;

  // Whether a live binding is currently borrowed (all four services set).
  // Observability for the runtime binding's teardown assertion; adds no
  // dependency, changes no behavior.
  bool attached() const noexcept;

  // --- description methods (composed + memoized, doc 05:8-37, doc 13:91-92) ---
  // Memoized on the pinned document's revision (the aggregate-revision proxy
  // visible at the contract level); recomputed only when it changes.
  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;

  // Compose the child through the synthetic viewport (doc 05:24). Clears the
  // target, then bottom-to-top pulls each visible child layer's content through
  // the injected `PullService` and source-over composites the settled result --
  // into the target itself when the child's working space is the target's, or
  // into a child-tagged intermediate that converts once into the target when it
  // is not (the doc 07 rule 4 nesting boundary).
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  // Nested's per-layer descent runs synchronously on the calling frame thread;
  // only leaf renders are dispatched to workers by the `PullService`. `render`
  // touches only per-call temps, the request's thread-confined target, and the
  // (immutable-after-attach) injected services -- so it is safe to invoke
  // concurrently for distinct requests (doc contract, content.hpp:262-275).
  //
  // `true` is about NESTED's own state, and is NOT a licence to render it on a
  // worker: the descent re-enters `PullService::pull`, which reads and writes the
  // frame-thread-confined `TileCache` (`worker_pool.hpp:37-40`, "workers never
  // touch the cache"). Dispatching a nested (or any non-leaf) content to a worker
  // races the driver's own cache probes -- so the driver keeps the operator
  // descent on its own thread and fans out only leaves
  // (`offline_sequence.cpp`'s `RenderDispatch`). Declaring `false` here would NOT
  // fix that: the pool's serialization gate excludes concurrent renders of the
  // same content, not concurrent access from the driver thread.
  bool render_thread_safe() const override { return true; }

  // --- audio facet (doc 12:202-208, the recursion reference proof) ------------
  // Nested always exposes the audio facet (like any nesting boundary is a
  // synthetic monitor); a child composition with no audio content simply mixes to
  // silence, exactly as a child with no visible content composites to nothing.
  AudioFacet* audio() override;

  // --- operator graph (doc 13:39-67) ---
  // The child composition's member-layer contents, in bottom-to-top order, so
  // the core folds them for aggregate revision, cycle detection, and damage
  // routing. Non-empty (nested is a non-leaf operator).
  std::span<const ContentRef> inputs() const override;
  // The child composition itself, surfaced to the core as graph structure (doc 08
  // Principle 7): the serializer walks it to reach in-document child compositions and
  // re-derives the core-owned `"composition"` reference from it on every save. Nested is
  // the one kind that answers non-null.
  ObjectId composition_ref() const override { return d_child; }
  // Map an input layer's damage through that layer's embedding transform
  // (covering / over-approximating, content.hpp:293-301).
  Rect map_input_damage(std::size_t input, const Rect& rect) const override;
  // Conservative: nested never claims request-scoped pass-through in the walking
  // skeleton (always faithful -- `check_operator_identity_faithful`).
  std::optional<std::size_t> identity(const RenderRequest& request) const override;

  // Behavioral-counter surface (doc 16:54-62, mirrors RasterStore's counters):
  // the number of metadata recomputations, witnessing the aggregate-revision
  // memoization (zero recomputes across repeated queries at a stable revision).
  std::uint64_t metadata_recomputes() const noexcept;

  ObjectId child() const noexcept { return d_child; }

  static constexpr const char* kind_id = "org.arbc.nested";

private:
  // One child layer's contribution to nested's metadata + input graph.
  struct ChildInput {
    ContentRef content{nullptr};
    Affine transform{};
    double opacity{1.0};
  };

  // Composed metadata + input edges, valid for one document revision.
  struct Memo {
    std::uint64_t revision{0};
    // The snapshot this memo was computed against. Carried by the MEMO, not read
    // back off `d_doc`, because `detach` clears `d_doc` while the memo survives: a
    // per-frame detach/re-attach cycle at the same pin must still recognise the
    // snapshot as unchanged (Decision 3).
    const DocRoot* doc{nullptr};
    bool valid{false};
    std::optional<Rect> bounds{};
    Stability stability{Stability::Static};
    std::optional<TimeRange> time_extent{};
    // Audio metadata aggregated from the reachable child-layer audio facets and
    // memoized on the SAME aggregate revision as the visual metadata (doc
    // 12:208, "the aggregate revision covers audio damage since it is the same
    // revision space"): Static iff every reachable audible child audio facet is
    // Static; the time-mapped union of reachable child extents (nullopt if any
    // reachable child audio is Static-unbounded).
    Stability audio_stability{Stability::Static};
    std::optional<TimeRange> audio_extent{};
    std::vector<ChildInput> inputs{};
    std::vector<ContentRef> input_refs{}; // stable storage backing `inputs()`
  };

  // Recompute the memo if the pinned revision moved (WRITER for the memo; guarded
  // by `d_mutex`). Unattached, it answers from the memo instead: the surviving one
  // when detached, else the empty placeholder (keyed to no snapshot, so `attach`
  // re-keys it).
  void ensure_memo() const;

  // One child layer, re-expressing the compositor's per-layer predicate loop
  // (Decision: only the thin loop is duplicated, never the heavy machinery),
  // pulling `content` through the injected service and compositing the result.
  //
  // Returns whether this layer contributed its EXACT pixels, which `render` folds
  // across the child's layers into its own `RenderResult::exact` (doc 09's honest-
  // exactness AND fold, exactly as `PullServiceImpl::pull` folds `region_exact`
  // across a region's covering tiles). A culled, unresolved, or failed layer
  // contributes its designed placeholder -- a FINAL answer, so still exact. A
  // layer whose pull was DEFERRED to a worker is the one transient case: this pass
  // shows a placeholder that a later pass will replace, so it is NOT exact and the
  // caller must not cache the composed tile as if it were.
  bool compose_child_layer(const LayerRecord& layer, const Affine& camera, const Rect& device_rect,
                           const RenderRequest& request, Backend& backend, Surface& target) const;

  // One child layer's audio contribution, the audio twin of `compose_child_layer`:
  // time-map the request window into child-local time at the composed varispeed
  // rate, pull the child's audio through the injected service, and additively mix
  // the settled block into `request.target` scaled by the layer's `gain` and
  // remixed to the request layout. Folds this layer's honesty into the running
  // `achieved`/`exact` aggregate. Skips (contributes silence) a layer that is
  // inaudible, has zero gain, resolves to no content, exposes no audio facet, is
  // culled by its span, or whose child pull does not settle inline.
  void mix_child_layer(const LayerRecord& layer, const CompositionRecord& comp,
                       const AudioRequest& request, std::uint32_t& achieved, bool& exact) const;

  // The audio facet nested exposes from `audio()` (doc 12:63-70), mirroring
  // tone's inner `ToneFacet`. Holds only a back-pointer to its owner; every query
  // reads the owner's immutable-after-attach services and the pinned snapshot, so
  // it inherits `render_thread_safe()`'s argument (doc 12:154-164, audio renders
  // ahead on workers, never the device callback). The metadata methods read the
  // shared aggregate-revision memo; `render_audio` re-walks the pinned membership
  // exactly as `render` does.
  class NestedAudioFacet final : public AudioFacet {
  public:
    explicit NestedAudioFacet(NestedContent* owner) : d_owner(owner) {}

    std::optional<TimeRange> audio_extent() const override;
    Stability audio_stability() const override;
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion> done) override;

  private:
    NestedContent* d_owner;
  };

  ObjectId d_child;
  PullService* d_pull{nullptr};
  Backend* d_backend{nullptr};
  NestedResolver d_resolver{};
  const DocRoot* d_doc{nullptr};

  // Recursive so a cyclic graph (a Droste self-embedding) that re-queries this
  // same content's metadata while `ensure_memo` is mid-computation does not
  // self-deadlock: the memo is marked valid up front, so the re-entrant query
  // short-circuits to the in-progress (neutral) value -- each node visited once
  // (doc 05:54-75 graph-walk-bounds-cycles).
  mutable std::recursive_mutex d_mutex;
  mutable Memo d_memo;
  mutable std::uint64_t d_metadata_recomputes{0};

  // The audio facet handed out by `audio()`. A stable-address member (pointer
  // identity across calls -- the #audio-facet-optional contract), constructed
  // with a back-pointer to this content.
  NestedAudioFacet d_audio_facet{this};
};

} // namespace arbc
