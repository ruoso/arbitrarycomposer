#pragma once

#include <arbc/base/expected.hpp> // expected/unexpected (doc 10: errors as values)
#include <arbc/base/geometry.hpp>
#include <arbc/base/rational_time.hpp> // Rational (PlaybackHint rate)
#include <arbc/base/time.hpp>
#include <arbc/model/records.hpp> // StateHandle (L3->model edge, doc 17:53,68-72)
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_ref.hpp> // SurfaceRef (content-provided surface, doc 09)

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace arbc {

// Cacheability of content output (docs 01/11): stability governs how the tile
// cache keys a content's output on the playback (time) axis (doc 11:138-143).
// `Static` ignores `request.time` and reports no `achieved_time`, so it adds no
// time dimension to the cache key -- a still grows no cache across a clock
// advance. `Timed` reports the quantized `achieved_time` it actually rendered
// and is cacheable per achieved_time. `Live` is non-deterministic, cacheable
// only within a single frame/snapshot.
enum class Stability {
  Static, // time-invariant; ignores request.time, reports no achieved_time
  Timed,  // deterministic function of request time; reports the quantized
          //  achieved_time it rendered, cacheable per achieved_time (doc 11)
  Live,   // non-deterministic; cacheable only within a frame/snapshot
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
  // The local media time actually rendered, if the content quantized the
  // requested `time` to a native instant -- a 24 fps clip asked for `t=0.31 s`
  // renders `7/24 s` and says so (doc 11:110-114). This is the temporal analog
  // of `achieved_scale`. `nullopt` means the requested time was honored exactly
  // or the content is time-invariant (`Static`), mirroring
  // `achieved_scale == request.scale`; `Static` content reports `nullopt`, so
  // it contributes no time dimension to the tile-cache key (doc 11:138-143).
  // `std::optional<Time>` over the trivially copyable `Time` keeps
  // `RenderResult` a cheap by-value descriptor -- no allocation or atomic.
  std::optional<Time> achieved_time{};
  // The content's OWN surface, answering the request in place of filling
  // `request.target` (doc 09:87-100). Absent (`nullopt`) is the default and the
  // overwhelming-majority case: the content filled the target the ordinary way,
  // so `RenderResult` stays the cheap trivial-copy descriptor above -- the
  // shared_ptr atomic in `SurfaceRef` is paid ONLY when a surface is genuinely
  // adopted. Present implies (doc 09:97-98): the compositor composites/caches
  // from `provided` instead of the target, honoring the request's region and
  // scale, and returns the untouched target to the pool. The surface must carry
  // the composition working-space tag (v1; cross-tag convert-at-composite is
  // gated on a multi-format backend, doc 09:102-105). `SurfaceRef::transient`
  // marks a framebuffer the content reuses every frame: consume-within-frame,
  // copy-to-cache, never retained (doc 09:106-112).
  std::optional<SurfaceRef> provided{};
};

// A playback advisory issued to decoder-backed content (doc 11:160-178): the
// transport-derived `(direction, rate, horizon)` triple that lets a decoder
// pre-roll sequentially (seeking is expensive, decoding forward is cheap).
// `direction` is the sign of the playback rate (`+1` forward, `-1` reverse, `0`
// for a paused/zero-rate transport -- the *empty* hint); `rate` is the
// transport's exact rational playback rate; `horizon` is the content-time
// lookahead window (`|rate|` x a runtime real-time window, exact rational). A
// named struct rather than three loose scalars: it is extensible (audio may add a
// quantum count) and mirrors how `RenderRequest` bundles inputs. It is a
// `contract`-level value carrying only `base` scalars -- the hint *issued to
// `Content`* is a contract concept, but the cache (which cannot name contract
// types) receives the triple unpacked into `base` scalars by the runtime
// (doc 11:175-178). Trivially copyable, so it stays a cheap by-value descriptor.
struct PlaybackHint {
  int direction{0};
  Rational rate{};
  Time horizon{};
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

// The editable-state facet (doc 03:98, doc 14:110-123). A content with mutable,
// undoable state (a raster's pixel buffer) implements this and returns it from
// `Content::editable()`; leaf and live content omit it (the null default). The
// three operations are the capture discipline doc 14 mandates, all over the same
// opaque `model::StateHandle` a render request pins (doc 14:181-187), so
// `render(snapshot = h)` renders exactly the state `capture()` froze:
//   - `capture()`: snapshot the current edited state into a `StateHandle`. MUST
//     be O(small) -- cheap enough to call once per gesture -- realized by
//     structural sharing: a paint stroke copies only the tiles it touched, so
//     capture copies O(touched tiles), not O(document) (doc 14:110-116,164-171).
//   - `restore(h)`: adopt a prior captured state (the undo/redo path), emitting
//     damage for what changed (doc 14:117-119).
//   - `state_cost(h)`: the byte cost of a captured state, for journal memory
//     budgeting (doc 14:120-122).
// The L3 interface only (doc 17:53): pure virtuals and a virtual destructor, no
// state. `org.arbc.raster` is the first and reference implementer (doc 14:164).
class Editable {
public:
  Editable(const Editable&) = delete;
  Editable& operator=(const Editable&) = delete;
  virtual ~Editable() = default;

  virtual StateHandle capture() = 0;
  virtual void restore(StateHandle state) = 0;
  virtual std::size_t state_cost(StateHandle state) const = 0;

protected:
  Editable() = default;
};

// The layer contract (doc 03). Walking-skeleton subset: the audio facet lands
// with its system. The operator-graph members below are null/identity defaults,
// so leaf content is behaviourally unchanged.
class Content {
public:
  Content(const Content&) = delete;
  Content& operator=(const Content&) = delete;
  virtual ~Content();

  virtual std::optional<Rect> bounds() const = 0;
  virtual Stability stability() const = 0;
  // The temporal analog of `bounds()` (doc 03:77-78, doc 11:67-79): the local
  // media-time range over which this content varies or exists, or `nullopt` for
  // time-invariant (`Static`) content -- exactly as `bounds()` returns
  // `nullopt` for unbounded content. A pure virtual, deliberately grouped with
  // the description methods rather than null-defaulted like the operator-graph
  // members below: every author must consciously declare a content's temporal
  // extent, because a silent `nullopt` default would misclassify an un-migrated
  // `Timed` content as time-invariant and let its tiles be served stale across
  // a clock advance. The common `Static` case is a one-line
  // `return std::nullopt;`.
  virtual std::optional<TimeRange> time_extent() const = 0;
  // The render-free grid query (doc 11:115-129): the native-grid instant a
  // `render(time = t)` would resolve to, computed WITHOUT rendering, or `nullopt`
  // for content that honors any time exactly (or is `Static`). A 24 fps `Timed`
  // clip returns `floor(t * 24) / 24`. This is the *defaulted opposite* of
  // `time_extent()` above: `time_extent()` is a non-defaulted pure virtual
  // because a silent default would misclassify a `Timed` content as timeless and
  // serve it stale; `quantize_time`'s `nullopt` default is *safe* -- it means
  // "use the requested time as-is", today's exact behaviour, sound for every
  // content (an un-migrated `Timed` content simply coalesces nothing, never
  // renders wrong pixels). So it is null-defaulted like the operator-graph
  // members below, and only content that can quantize opts in.
  //
  // Contract (conformance-tested, doc 11:124-126): when `quantize_time(t)` has a
  // value it MUST equal `render(time = t).achieved_time`, and it MUST be
  // idempotent (`quantize_time(*quantize_time(t)) == quantize_time(t)`). This is
  // what lets the compositor form the native-instant tile key at plan time,
  // BEFORE rendering, and trust the render to land on that key -- so every
  // requested instant in one native frame period collapses to a single key and a
  // sub-frame clock advance issues zero renders (achieved-time coalescing). Pure
  // and const: a query on immutable content, no cross-frame state.
  virtual std::optional<Time> quantize_time(Time /*t*/) const { return std::nullopt; }
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

  // Render-concurrency declaration (doc 02:126-130, doc 03:131-139). Default
  // `true`: `render` is internally thread-safe, so the worker pool may render
  // this content's tiles concurrently with each other. A content whose renderer
  // is NOT internally thread-safe (a stateful decoder, a single-context engine)
  // overrides this to `false`; the core then funnels that content's requests
  // through a per-content serialization queue so at most one runs at a time,
  // rather than forcing every author to add their own lock. Orthogonal to the
  // sync/async axis of `render`: externally-async content returns `nullopt` and
  // settles `done` later, occupying no worker regardless of this flag. The
  // *declaration* lives here on the contract so the core routes without a
  // downcast; the *mechanism* (the pool + per-content queue) is runtime policy
  // (`runtime.threading`, doc 17:60). Default keeps every existing content
  // byte-identical.
  virtual bool render_thread_safe() const { return true; }

  // Playback advisory (doc 11:160-178): each frame the runtime issues the
  // transport-derived `(direction, rate, horizon)` hint so decoder-backed content
  // can PRE-ROLL sequentially (seeking is expensive, decoding forward cheap). The
  // default is a NO-OP returning `void`: the hint is purely advisory -- it changes
  // no pixels and no cache correctness, and solicits no answer -- so content that
  // ignores it is byte-identical whether or not a hint is issued (determinism
  // stays owned by `quantize_time`/`achieved_time`, not by hints). Non-const: a
  // decoder mutates its own pre-roll state on receipt (precisely the
  // `render_thread_safe() == false` stateful path); render purity (a pure function
  // of the pinned snapshot, above) is unaffected because the hint feeds pre-roll,
  // not the rendered pixels. Only decoder-backed `Timed` content overrides it;
  // every existing content keeps the null default.
  virtual void playback_hint(const PlaybackHint& /*hint*/) {}

  // The editable-state facet, or `nullptr` for non-editable (leaf/live) content
  // (doc 03:98, "Live content omits"). A content that returns non-null promises
  // its `render` is a pure function of the pinned `snapshot` handle the facet's
  // `capture()` produces (doc 14:181-187). `org.arbc.raster` is the reference
  // implementer (doc 14:164-171); every walking-skeleton kind keeps the default.
  virtual Editable* editable() { return nullptr; }

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
