#pragma once

#include <arbc/base/expected.hpp> // expected/unexpected (doc 10: errors as values)
#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/model/records.hpp> // StateHandle (L3->model edge, doc 17:53,68-72)
#include <arbc/surface/surface.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace arbc {

// Cacheability of content output (docs 01/11).
enum class Stability {
  Static, // time-invariant; same request -> same pixels until damage
  Timed,  // deterministic function of request time
  Live,   // non-deterministic; cache only within a frame
};

// The two request disciplines (doc 03:12-13,48). `BestEffort` (interactive)
// renders may answer async, degrade, and observe a deadline; `Exact` (offline)
// renders must be faithful, may take unbounded time, and ignore the deadline.
enum class Exactness { BestEffort, Exact };

// Why an asynchronous render failed to produce pixels (doc 03:65). A
// contract-local enum in the per-component errors-as-values idiom
// (doc 10; cf. SurfaceError, PoolError, RefError) -- widened as real async
// content lands. `fail` resolves the provisional doc-03 `Error` name to this.
enum class RenderError {
  ContentFailed,       // the content could not render (internal error)
  ResourceUnavailable, // a resource the render needs is not available
};

// A monotonic wall-clock instant: the compute budget for a `BestEffort`
// render. Distinct from `base::Time`, which is content-local media time in
// flicks (a position on the content's timeline, not a budget); a deadline is a
// point on `std::chrono::steady_clock`, the standard monotonic clock a frame
// loop reads. The contract carries the *value only* -- reading the clock and
// enforcing the deadline is runtime policy (doc 17:39-41), so this type
// exposes no `now()`/`expired()`. `none()` (`time_point::max()`) means "no
// deadline": the default, and the mandatory value for `Exact` requests
// (doc 03:49). Trivially copyable, so `RenderRequest` stays a cheap by-value
// descriptor.
struct Deadline {
  std::chrono::steady_clock::time_point at{std::chrono::steady_clock::time_point::max()};

  static constexpr Deadline none() { return {}; }
  constexpr bool is_none() const { return at == std::chrono::steady_clock::time_point::max(); }

  friend constexpr bool operator==(const Deadline&, const Deadline&) = default;
};

// What the compositor wants rendered (doc 03): a region of content-local
// space, at a scale (device pixels per local unit), at a local time, over a
// pinned content-state snapshot. `snapshot` is the "revision fence" of the
// doc-03 sketch (03:50), resolved by doc 14's purity refinement to the
// content's captured `StateHandle` (14:181-187): the frozen state the request
// renders. It is an index-only, trivially-copyable slab handle (no refcount
// touch, no allocation, no store identity), so the request stays a cheap
// by-value descriptor built per render call. Defaults to `k_state_none`
// (`has_state() == false`): unpinned content renders live. Runtime binding
// (`model.content_binding`) populates non-none handles; the walking-skeleton
// compositor supplies the default.
struct RenderRequest {
  Rect region;
  double scale{1.0};
  Time time;
  StateHandle snapshot{};
  Surface& target;
  Exactness exactness{Exactness::BestEffort};
  Deadline deadline{};
};

struct RenderResult {
  double achieved_scale{1.0};
  bool exact{true};
};

// A thread-safe, one-shot completion handle (doc 03:62-67). The renderer
// settles it EXACTLY ONCE via `complete(RenderResult)` or `fail(RenderError)`
// (mutually exclusive; a second settle -- or a settle after `take()` -- is
// silently ignored, never UB); the caller drains the single settlement with
// the non-blocking `take()`. Inline and off-thread settlements both flow
// through this same primitive, which is what makes the compositor's "one code
// path" real (doc 03:117-121): a returned-inline `RenderResult` is folded in
// via `complete` and read back through `take` exactly as a deferred render is.
//
// Thread safety: `complete`/`fail` may run on a renderer thread while
// `cancel`/`cancelled`/`take`/`settled` run on the caller (compositor/runtime)
// thread. The settlement state is published with release/acquire ordering
// after the payload is written, so a `take()` that observes a settlement never
// reads a torn payload. `cancelled()` is an ADVISORY cooperative flag: `cancel`
// makes it observe `true` but does NOT prevent a later `complete`/`fail`
// (doc 03:66,122-123) -- it only tells a long render it *may* abandon work.
// How the caller is *woken* on completion (condvar/eventfd) is runtime policy
// and is out of scope (doc 17:39-41).
class RenderCompletion {
public:
  RenderCompletion() = default;
  RenderCompletion(const RenderCompletion&) = delete;
  RenderCompletion& operator=(const RenderCompletion&) = delete;

  // --- renderer side ---
  void complete(RenderResult result);
  void fail(RenderError error);
  bool cancelled() const noexcept;

  // --- caller side ---
  void cancel() noexcept;
  bool settled() const noexcept;
  // The single settlement, or `nullopt` if not yet settled (non-blocking).
  // Yields the settlement at most once; subsequent calls return `nullopt`.
  std::optional<expected<RenderResult, RenderError>> take();

private:
  // pending -> claimed (payload being written) -> published (readable) ->
  // taken. The claimed intermediate keeps a racing `take()` from reading a
  // half-written payload: `published` is release-stored only after the payload
  // write, and `take()`/`settled()` acquire it.
  enum State : int { k_pending, k_claimed, k_published, k_taken };

  // Claim the single settle slot and publish `settlement`; returns whether
  // this caller won (a second settle loses the CAS and is ignored).
  bool try_settle(expected<RenderResult, RenderError> settlement);

  std::atomic<int> d_state{k_pending};
  std::atomic<bool> d_cancelled{false};
  std::optional<expected<RenderResult, RenderError>> d_payload;
};

// A non-owning graph edge to a `Content` input (doc 13:48-52, Decision 1).
// Input edges are core-owned structure (doc 13:142), so an operator is a
// non-owning observer of its inputs -- a raw non-owning pointer states exactly
// that lifetime relationship. This resolves the provisional doc-13 `ContentRef`
// type name to the project's existing `Content*` idiom (the compositor already
// names content as `Content* content = resolve(layer.content)`), so it is
// trivially copyable and keeps `inputs()` and the pull seam allocation-free.
class Content;
using ContentRef = Content*;

// The layer contract (doc 03). Walking-skeleton subset: the audio and editable
// facets land with their systems. The operator-graph members below are
// null/identity defaults, so leaf content is behaviourally unchanged.
class Content {
public:
  Content(const Content&) = delete;
  Content& operator=(const Content&) = delete;
  virtual ~Content();

  virtual std::optional<Rect> bounds() const = 0;
  virtual Stability stability() const = 0;
  // Render `request.region` into `request.target`. For content with editable
  // state, `render` must be a PURE function of `(request.snapshot, region,
  // scale, time)` (docs 03:138-140, 14:181-187): two calls with an identical
  // `RenderRequest` yield byte-identical target pixels, and `snapshot` is a
  // genuine input -- requests differing only in `snapshot` may yield different
  // pixels, since the handle names the frozen state to interpret. This purity
  // is what lets a cache key of (revision, region, scale, time) honestly
  // identify the pixels and lets render workers read frozen state while the
  // writer keeps editing (doc 14:159-162).
  //
  // One entry point, sync and async unified (doc 03:80-84,117-121). Return a
  // `RenderResult` to settle INLINE (synchronously); return `nullopt` to
  // answer ASYNCHRONOUSLY -- the content stores `done` and settles later via
  // `done->complete(result)` or `done->fail(error)`. The caller drives both
  // the inline value and the deferred settlement through the same
  // `RenderCompletion`, so there is exactly one settle path.
  //
  // Discipline (doc 03:12-13,124-127), orthogonal to the snapshot purity above:
  // a `BestEffort` (`request.exactness`) render MAY answer async, degrade
  // (report `achieved_scale < request.scale`, `exact == false`), or observe
  // `request.deadline`; an `Exact` render MUST be faithful -- it may take
  // unbounded time, reports `achieved_scale`/`exact` honestly, and does not
  // consult the deadline (`Exact` requests carry `Deadline::none()`).
  virtual std::optional<RenderResult> render(const RenderRequest& request,
                                             std::shared_ptr<RenderCompletion> done) = 0;

  // --- operator graph (doc 13:39-67) ---
  // The operator's input edges, visible to the core for aggregate revisions,
  // snapshot consistency, cycle detection, and damage routing (doc 13:48-51).
  // The returned span views the operator's own storage in declared order.
  // Default: an empty span -- leaf content is a graph leaf (doc 13:52).
  virtual std::span<const ContentRef> inputs() const { return {}; }

  // Map damage on input `input`'s given `rect` into damage on this content's
  // output (doc 13:54-57). Default: identity (pass-through-shaped content).
  //
  // Covering requirement (normative, entailed by doc 13:104-107): the returned
  // output rect MUST cover every output pixel whose value can change when the
  // named input's `rect` changes. Over-approximation is sound; under-
  // approximation drops repaint and is a bug. This is the forward reverse of
  // the region pull -- a blur inflates the damage by its radius exactly as it
  // inflates the pulled region. The general property is enforced over
  // arbitrary operators by the public conformance suite.
  virtual Rect map_input_damage(std::size_t input, const Rect& rect) const;

  // The OpenFX-style identity (pass-through) action (doc 13:59-65): if, for
  // this request, this content's output is exactly input N's output (a fade at
  // envelope == 1, a disabled effect), return N so the compositor can serve
  // that input's cached tiles directly -- no render, no copy, no new cache
  // entry. Request-scoped. Default: `nullopt` (never a pass-through).
  virtual std::optional<std::size_t> identity(const RenderRequest& request) const;

protected:
  Content() = default;
};

// The abstract service through which an operator asks the core to render an
// input, instead of calling `input->render()` directly (doc 13:69-89). A pull
// is the same machinery as a compositor-issued request -- cache lookup first,
// worker scheduling, the request's snapshot token respected, its deadline and
// budget inherited -- so it carries the render contract's own `RenderRequest`
// and `RenderCompletion` and adds no new settlement path.
//
// This is the L3 interface only (doc 17:53): pure virtuals and a virtual
// destructor, no state and no cache/worker/scheduling logic. The concrete
// implementation and the attach-time injection that hands a service to content
// are the L4 concern (`compositor.pull_service`, doc 17:56). The audio pull
// (`pull_audio`, doc 13:80) joins this interface with `contract.audio_facet`,
// which owns `AudioRequest`.
class PullService {
public:
  PullService(const PullService&) = delete;
  PullService& operator=(const PullService&) = delete;
  virtual ~PullService() = default;

  // Render `input` for `request`, settling `done` exactly as `Content::render`
  // does -- inline via `done->complete`/`done->fail` or later off-thread.
  virtual void pull(ContentRef input, const RenderRequest& request,
                    std::shared_ptr<RenderCompletion> done) = 0;

protected:
  PullService() = default;
};

} // namespace arbc
