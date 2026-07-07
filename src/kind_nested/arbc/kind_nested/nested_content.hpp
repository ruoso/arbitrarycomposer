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
// Scope (doc 05 machinery): the VISUAL facet, HOMOGENEOUS working-space trees
// ("homogeneous trees pay nothing", doc 07:34-35), two-level caching (the
// service's leaf cache plus the parent's cache of nested's composed result),
// cycle/Droste termination, and snapshot consistency. The audio facet
// (`kinds.nested_audio`) and heterogeneous working-space conversion
// (`kinds.nested_working_space_conversion`) are deferred follow-ups whose
// prerequisites (`contract.audio_facet`; a `Backend` conversion operation) are
// not yet landed.
//
// Levelization (doc 17:59): the kind depends only on `contract`. It re-expresses
// the compositor's per-layer cull/compose loop directly (it may not name the
// sibling `compositor`, an L4->L4 edge) and reuses only the L3 `PullService`
// interface for the heavy machinery.
class NestedContent final : public Content {
public:
  // Wrap a reference to child composition `child`. Unattached until `attach`;
  // metadata and render assert attachment (the injected services are the whole
  // engine).
  explicit NestedContent(ObjectId child);

  // Inject the render-time services (mirrors the raster attach seam):
  //  - `pull`     the shared PullService every child render travels through;
  //  - `backend`  the L2 seam nested clears/allocates/composites through;
  //  - `resolver` id->Content* over the pinned model's content ids;
  //  - `doc`      the pinned document version nested reads membership from
  //               (`for_each_layer_in`), so a Droste scene is self-consistent
  //               within a frame (doc 05:71-75). Re-attaching a newer pin after
  //               an edit re-keys the memoized metadata (doc 05:15-16).
  // The `DocRoot` and services must outlive this content.
  void attach(PullService& pull, Backend& backend, NestedResolver resolver, const DocRoot& doc);

  // --- description methods (composed + memoized, doc 05:8-37, doc 13:91-92) ---
  // Memoized on the pinned document's revision (the aggregate-revision proxy
  // visible at the contract level); recomputed only when it changes.
  std::optional<Rect> bounds() const override;
  Stability stability() const override;
  std::optional<TimeRange> time_extent() const override;

  // Compose the child through the synthetic viewport (doc 05:24). Clears the
  // target, then bottom-to-top pulls each visible child layer's content through
  // the injected `PullService` and source-over composites the settled result.
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override;

  // Nested's per-layer descent runs synchronously on the calling frame thread;
  // only leaf renders are dispatched to workers by the `PullService`. `render`
  // touches only per-call temps, the request's thread-confined target, and the
  // (immutable-after-attach) injected services -- so it is safe to invoke
  // concurrently for distinct requests (doc contract, content.hpp:262-275).
  bool render_thread_safe() const override { return true; }

  // --- operator graph (doc 13:39-67) ---
  // The child composition's member-layer contents, in bottom-to-top order, so
  // the core folds them for aggregate revision, cycle detection, and damage
  // routing. Non-empty (nested is a non-leaf operator).
  std::span<const ContentRef> inputs() const override;
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
    bool valid{false};
    std::optional<Rect> bounds{};
    Stability stability{Stability::Static};
    std::optional<TimeRange> time_extent{};
    std::vector<ChildInput> inputs{};
    std::vector<ContentRef> input_refs{}; // stable storage backing `inputs()`
  };

  // Recompute the memo if the pinned revision moved (WRITER for the memo; guarded
  // by `d_mutex`). Requires attachment.
  void ensure_memo() const;

  // One child layer, re-expressing the compositor's per-layer predicate loop
  // (Decision: only the thin loop is duplicated, never the heavy machinery),
  // pulling `content` through the injected service and compositing the result.
  void compose_child_layer(const LayerRecord& layer, const Affine& camera, const Rect& device_rect,
                           const RenderRequest& request, Backend& backend, Surface& target) const;

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
};

} // namespace arbc
