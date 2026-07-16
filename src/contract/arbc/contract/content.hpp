#pragma once

#include <arbc/arbc_api.h>
#include <arbc/base/expected.hpp> // expected/unexpected (doc 10: errors as values)
#include <arbc/base/geometry.hpp>
#include <arbc/base/rational_time.hpp> // Rational (PlaybackHint rate)
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>    // Affine (the spatialization listener, doc 12:167-206)
#include <arbc/media/audio_block.hpp> // ChannelLayout/AudioBlock (audio facet vocabulary, doc 17:50)
#include <arbc/model/records.hpp>     // StateHandle (L3->model edge, doc 17:53,68-72)
#include <arbc/surface/surface.hpp>
#include <arbc/surface/surface_ref.hpp> // SurfaceRef (content-provided surface, doc 09)

#include <atomic>
#include <bit> // std::bit_cast (the byte-exact spatial-context digest, doc 12:249-254)
#include <chrono>
#include <cmath> // std::sqrt (the byte-exact constant-power pan law, doc 12:191-199)
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

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

// A thread-safe, one-shot completion handle (doc 03:62-67), generic over its
// settled payload `Result`. The renderer settles it EXACTLY ONCE via
// `complete(Result)` or `fail(RenderError)` (mutually exclusive; a second
// settle -- or a settle after `take()` -- is silently ignored, never UB); the
// caller drains the single settlement with the non-blocking `take()`. Inline
// and off-thread settlements both flow through this same primitive, which is
// what makes the compositor's "one code path" real (doc 03:117-121): a
// returned-inline `Result` is folded in via `complete` and read back through
// `take` exactly as a deferred render is.
//
// One template body, two facets (doc 12 Decision 3): the render facet
// instantiates `Completion<RenderResult>` (`RenderCompletion`) and the audio
// facet `Completion<AudioResult>` (`AudioCompletion`), so the subtle
// release/acquire + single-settle CAS logic is written and TSan-verified once
// and both facets reuse it. `RenderError` is the shared failure type.
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
template <class Result> class Completion {
public:
  Completion() = default;
  Completion(const Completion&) = delete;
  Completion& operator=(const Completion&) = delete;

  // --- renderer side ---
  void complete(Result result);
  void fail(RenderError error);
  bool cancelled() const noexcept;

  // --- caller side ---
  void cancel() noexcept;
  bool settled() const noexcept;
  // Has it settled with a RESULT (rather than a `fail`), and is that result
  // still here to be taken? A non-consuming peek at the settlement's KIND, for
  // a caller that must distinguish "the render finished and its payload is
  // waiting for you" from "the render failed, and taking it yields nothing" --
  // without taking it. `false` for pending, for failed, and for already-taken.
  // The compositor's dispatch gate needs exactly this: a settled-but-undrained
  // success is a render that must not be re-issued, a settled-via-fail is one
  // that must be (`compositor/refinement.hpp`, `tile_in_flight`).
  bool settled_ok() const noexcept;
  // The single settlement, or `nullopt` if not yet settled (non-blocking).
  // Yields the settlement at most once; subsequent calls return `nullopt`.
  std::optional<expected<Result, RenderError>> take();

private:
  // pending -> claimed (payload being written) -> published (readable) ->
  // taken. The claimed intermediate keeps a racing `take()` from reading a
  // half-written payload: `published` is release-stored only after the payload
  // write, and `take()`/`settled()` acquire it.
  enum State : int { k_pending, k_claimed, k_published, k_taken };

  // Claim the single settle slot and publish `settlement`; returns whether
  // this caller won (a second settle loses the CAS and is ignored).
  bool try_settle(expected<Result, RenderError> settlement);

  std::atomic<int> d_state{k_pending};
  std::atomic<bool> d_cancelled{false};
  std::optional<expected<Result, RenderError>> d_payload;
};

// The render facet's completion: the one-shot settlement of a `RenderResult`.
// An alias for the shared template above, so every existing call site is
// byte-unchanged (doc 12 Decision 3).
using RenderCompletion = Completion<RenderResult>;

// A non-owning graph edge to a `Content` input (doc 13:48-52, Decision 1).
// Input edges are core-owned structure (doc 13:142), so an operator is a
// non-owning observer of its inputs -- a raw non-owning pointer states exactly
// that lifetime relationship. This resolves the provisional doc-13 `ContentRef`
// type name to the project's existing `Content*` idiom (the compositor already
// names content as `Content* content = resolve(layer.content)`), so it is
// trivially copyable and keeps `inputs()` and the pull seam allocation-free.
class Content;
using ContentRef = Content*;

// What the audio pull answers with (doc 12:58-61): the temporal-resolution twin
// of `RenderResult`. `achieved_rate` is the native rate the facet actually
// produced (< `request.sample_rate` when a `BestEffort` lookahead render
// degrades, the temporal analog of `achieved_scale`); `exact` is false when the
// block is not a faithful answer to the request. Trivially copyable -- no
// allocation, no atomic -- so `render_audio` stays a cheap by-value answer.
struct AudioResult {
  std::uint32_t achieved_rate{0};
  bool exact{true};
};

// The audio completion: the one-shot settlement of an `AudioResult` through the
// shared `Completion<Result>` primitive (doc 12 Decision 3). `render_audio` and
// `PullService::pull_audio` settle it exactly as `render` settles a
// `RenderCompletion` -- inline or later off-thread, one code path.
using AudioCompletion = Completion<AudioResult>;

// The default sub-audible attenuation threshold (doc 12:200-206, Decision D4):
// 2^-12 (~ -72 dBFS), below single-contributor audibility. A Spatial subtree whose
// accumulated attenuation falls below this contributes nothing and is not descended,
// so a scale-1/2 Droste chain terminates by depth 12 -- far inside the doc-05 depth
// budget (max_depth = 64). Carried in the `Spatialization` context, so a monitor can
// tune it without a code change; the depth budget remains the hard backstop.
inline constexpr float k_sub_audible_atten = 1.0F / 4096.0F;

// The optional Spatial mix context threaded on the audio request (doc 12:167-206,
// Decision D1): the mechanism that carries the composed transform into the mix,
// INCLUDING across the pull boundary into nested contributors, so a nested
// composition spatializes on the same footing as the root. Absent on an
// `AudioRequest` => Flat (a byte-identical no-op; an ambient host pays nothing);
// present => Spatial. Trivially copyable (an `Affine` + scalars) so the request stays
// a cheap by-value descriptor. It is the ONLY carrier the L3 nested walk reads --
// never the L4 `MixPolicy` enum -- which is what lets an L3 nested contributor and
// the L4 engine spatialize without either naming the other (doc 17:41).
struct Spatialization {
  // The listener transform: this composition's local frame -> viewport pixels
  // (the audio twin of the visual `Viewport::camera`), composed per edge on descent
  // -- `compose(listener, embedding)` for a nested child, never accumulated wrong
  // (doc 04). A layer's composed viewport position is `compose(listener, transform)`.
  Affine listener{};
  // The viewport extent for pan normalization (doc 12:191-199); `viewport_h` is
  // carried for symmetry with the visual viewport but the v1 pan is x-only.
  double viewport_w{0.0};
  double viewport_h{0.0};
  // The running product of edge attenuations from the camera down to THIS frame
  // (doc 12:183-190): threaded purely for the sub-audible cull decision -- the
  // per-layer sample attenuation is each layer's own `edge` (applied once), so the
  // product down the chain is the full composed scale with no double counting.
  float accum_atten{1.0F};
  // The sub-audible cull threshold (doc 12:200-206).
  float sub_audible{k_sub_audible_atten};
};

// A layer's per-edge Spatial attenuation (doc 12:183-190, Decision D2):
// `clamp(max_scale(transform), 0, 1)`, applied once per layer exactly as `gain` is,
// so amplification is capped at unity (Spatial never exceeds Flat loudness) and the
// product down a nesting chain equals the full composed scale (for scales <= 1).
// A pure, byte-exact function of the layer transform's coefficients -- shared by the
// L4 `mix_layer` and the L3 `NestedContent::mix_child_layer` walk sites so the two
// duplicated walks spatialize byte-identically (doc 17:41 Decision).
inline float spatial_edge_atten(const Affine& transform) {
  const double s = transform.max_scale();
  if (s <= 0.0) {
    return 0.0F;
  }
  if (s >= 1.0) {
    return 1.0F;
  }
  return static_cast<float>(s);
}

// The square-root constant-power pan gains (doc 12:191-199, Decision D3): a layer at
// composed viewport x-position `p` (normalized to [-1, 1] about `viewport_w / 2`,
// clamped) gets `t = (p + 1) / 2`, `gL = sqrt(1 - t)`, `gR = sqrt(t)`. `sqrt` is
// IEEE-754 correctly-rounded and platform-stable, so the gains are byte-exact and
// goldenable (unlike a `sin`/`cos` law). A degenerate viewport centers the source.
struct SpatialPanGains {
  float gl{1.0F};
  float gr{1.0F};
};
inline SpatialPanGains spatial_pan_gains(const Affine& composed, double viewport_w) {
  const double x = composed.apply(Vec2{0.0, 0.0}).x;
  double p = viewport_w > 0.0 ? (2.0 * x / viewport_w - 1.0) : 0.0;
  if (p < -1.0) {
    p = -1.0;
  }
  if (p > 1.0) {
    p = 1.0;
  }
  const double t = (p + 1.0) / 2.0;
  return SpatialPanGains{static_cast<float>(std::sqrt(1.0 - t)), static_cast<float>(std::sqrt(t))};
}

// Reduce a Spatialization to a stable 64-bit block-cache-key digest
// (doc 12:249-254, spatial_blockkey_disambiguation Decisions D1-D3). A nested
// composition's `render_audio` output *depends on* the spatial context (the
// listener drives pan/edge-attenuation of its children; `accum_atten`/`sub_audible`
// drive the sub-audible cull, which *changes content*), so two distinct contexts
// over the same `(content, revision, block index, rate)` must key to distinct cache
// slots. The digest folds the EXACT bit patterns of every field of the whole struct
// -- the six `Affine` listener coefficients, both viewport extents, and the two
// float scalars -- an over-key (D2): two distinct contexts never share a slot, at
// the accepted cost that two contexts which happen to render identically get two
// slots (doc 12:252 "caching matters less"). It is byte-exact and platform-stable
// (`std::bit_cast`, no float tolerance, no formatting) so goldens are stable
// (doc 16, Constraint 5), and it is a pure function -- the cache stays spatially
// agnostic, carrying the result as an opaque scalar (doc 17 levelization, D1). A
// 64-bit digest matches the project's key-hash discipline; a ~2^-64 collision
// degrades to exactly the pre-fix single-slot case, never a crash (D3).
inline std::uint64_t spatial_context_digest(const Spatialization& sp) {
  // The project's Boost-style 64-bit mixer (mirroring `cache::detail::key_hash_combine`
  // -- replicated here because `contract` may not depend on the same-level `cache`,
  // doc 17:40-44), run over `std::bit_cast` field bit patterns for good dispersion.
  auto mix = [](std::uint64_t seed, std::uint64_t value) noexcept -> std::uint64_t {
    return seed ^ (value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2));
  };
  std::uint64_t h =
      0x100000001b3ULL; // nonzero seed: an all-zero-field context still digests nonzero
  h = mix(h, std::bit_cast<std::uint64_t>(sp.listener.a));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.listener.b));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.listener.c));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.listener.d));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.listener.tx));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.listener.ty));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.viewport_w));
  h = mix(h, std::bit_cast<std::uint64_t>(sp.viewport_h));
  h = mix(h, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(sp.accum_atten)));
  h = mix(h, static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(sp.sub_audible)));
  // Guarantee the "zero exactly when Flat" invariant is exact: a present context that
  // happens to mix to zero (a ~2^-64 event) is nudged to a fixed nonzero sentinel so it
  // never aliases the Flat digest (0). Deterministic, so goldens stay stable.
  return h != 0 ? h : 0x9e3779b97f4a7c15ULL;
}

// The zero-when-Flat carrier: the digest of the optional Spatial context a `BlockKey`
// is built under -- `0` exactly when absent (Flat), so a Flat/leaf-host scene keys
// byte-identically to the pre-digest key (Constraint 1). The single shared call every
// L4 `BlockKey`-build site folds in (spatial_blockkey_disambiguation D4).
inline std::uint64_t spatial_context_digest(const std::optional<Spatialization>& sp) {
  return sp.has_value() ? spatial_context_digest(*sp) : std::uint64_t{0};
}

// What the mix engine wants rendered (doc 12:49-56): the 1D-signal twin of
// `RenderRequest` -- a time window at a working sample rate and channel layout,
// over a pinned content-state snapshot, into a caller-owned block. It stays a
// cheap by-value descriptor (no allocation, no refcount, no atomic on the
// request path): `target` is a non-owning `AudioBlock&` the caller
// zero-initializes and owns (doc 12:54); `layout` is a by-value `ChannelLayout`;
// and `snapshot` is the same index-only `model::StateHandle` a `RenderRequest`
// pins (doc 12 Decision 2, doc 14). Audio and video share one content object and
// one revision space, so a clip's samples pin the SAME frozen state as its
// pixels -- they cannot drift under editing. Audio never carries a `Deadline`:
// it renders ahead (lookahead), not on a deadline (doc 12:33-34), so there is no
// deadline field. `Exactness` reuses the render discipline -- `Exact` (offline
// export) requests are faithful, `BestEffort` (interactive lookahead) may
// degrade the achieved rate.
struct AudioRequest {
  TimeRange window;
  std::uint32_t sample_rate{0};
  ChannelLayout layout{ChannelLayout::Stereo};
  AudioBlock& target;
  Exactness exactness{Exactness::BestEffort};
  StateHandle snapshot{};
  // The optional Spatial mix context (doc 12:167-206, Decision D1). Trailing and
  // defaulted so every existing 6-field aggregate init stays valid and the Flat path
  // is byte-identical: absent => Flat, present => Spatial (the branch keys off this
  // field, never the L4 `MixPolicy` enum, so an L3 nested contributor spatializes on
  // the same footing as the root without seeing the engine's policy type).
  std::optional<Spatialization> spatial{};
};

// The optional audio facet (doc 12:63-70): a content's 1D-signal contract, the
// audio twin of `Content::render` and the visual description methods. A content
// with audio (a tone, a video clip's sound, a synth) implements this and returns
// it from `Content::audio()`; purely visual content omits it (the `nullptr`
// default) and costs the audio engine nothing (doc 12:73-77). The shape mirrors
// `Editable`: pure virtuals, a virtual destructor, non-copyable, a protected
// default ctor -- the L3 interface only (doc 17:53), no state and no mix
// machinery (that is `arbc::audio-engine`, L4). Because audio and video are two
// facets of the SAME content object, its audio and pixels can never drift under
// editing (doc 12:37-41).
class ARBC_API AudioFacet {
public:
  AudioFacet(const AudioFacet&) = delete;
  AudioFacet& operator=(const AudioFacet&) = delete;
  virtual ~AudioFacet() = default;

  // The local media-time range over which this content's audio varies or
  // exists, or `nullopt` for time-invariant (`Static`) audio -- the 1D-signal
  // twin of `Content::time_extent()` (doc 12:26-29). A tone is `Static` and
  // reports `nullopt`; most audio is `Timed`.
  virtual std::optional<TimeRange> audio_extent() const = 0;
  // The audio stability (doc 12:26-29): `Static` (a tone, ignores the window),
  // `Timed` (deterministic per window -- the common case), or `Live` (a
  // microphone). The same `Stability` enum the visual facet reports.
  virtual Stability audio_stability() const = 0;
  // The constant processing latency the facet introduces (doc 12:182-188),
  // honored by the L4 lookahead scheduler's pre-roll. The contract carries the
  // *value only* (default `Time::zero()`, no latency); enforcing it is runtime
  // policy, exactly as `Deadline` carries a value the runtime enforces.
  virtual Time latency() const { return Time::zero(); }
  // Render `request.window` into `request.target`, one code path sync and async
  // (doc 12:139-159): return an `AudioResult` to settle INLINE, or return
  // `nullopt` and settle later via `done->complete(result)`/`done->fail(error)`
  // -- the identical discipline as `Content::render`. Audio renders AHEAD, never
  // against a deadline; `Exact` (offline export) requests must be faithful,
  // `BestEffort` (interactive lookahead) may report `achieved_rate <
  // request.sample_rate` / `exact == false`.
  virtual std::optional<AudioResult> render_audio(const AudioRequest& request,
                                                  std::shared_ptr<AudioCompletion> done) = 0;

protected:
  AudioFacet() = default;
};

// The editable-state facet (doc 03:98, doc 14:110-123). A content with mutable,
// undoable state (a raster's pixel buffer) implements this and returns it from
// `Content::editable()`; leaf and live content omit it (the null default). The
// operations are the capture discipline doc 14 mandates, all over the same
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
//   - `retain(h)` / `release(h)`: the handle's reference lifecycle (doc
//     14:173-176). The runtime binding drives these off the model's
//     `StateRefSink` seam -- retain when a published version pins the handle,
//     release when that record is reclaimed -- so "a pinned version pins content
//     state too" and "version GC releases unreferenced state handles by
//     refcount" both come true. WRITER/DRAIN-THREAD ONLY.
// The L3 interface only (doc 17:53): pure virtuals and a virtual destructor, no
// state. `org.arbc.raster` is the first and reference implementer (doc 14:164).
class ARBC_API Editable {
public:
  Editable(const Editable&) = delete;
  Editable& operator=(const Editable&) = delete;
  virtual ~Editable() = default;

  virtual StateHandle capture() = 0;
  virtual void restore(StateHandle state) = 0;
  virtual std::size_t state_cost(StateHandle state) const = 0;
  virtual void retain(StateHandle state) = 0;
  virtual void release(StateHandle state) = 0;

protected:
  Editable() = default;
};

// The layer contract (doc 03). Walking-skeleton subset: the audio facet lands
// with its system. The operator-graph members below are null/identity defaults,
// so leaf content is behaviourally unchanged.
class ARBC_API Content {
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

  // The audio facet, or `nullptr` for purely visual content (doc 12:46,73-77).
  // The exact twin of `editable()`: a null-default discovery virtual the audio
  // engine probes to find a content's audio, allocation-free. A content that
  // overrides it returns its `AudioFacet` (a video clip, a tone, a synth);
  // every existing visual-only kind keeps the `nullptr` default and the audio
  // engine descends no audio path into it, costing it nothing. Because audio and
  // video are two facets of one content object, a clip's samples and pixels can
  // never drift under editing (doc 12:37-41).
  virtual AudioFacet* audio() { return nullptr; }

  // --- operator graph (doc 13:39-67) ---
  // The operator's input edges, visible to the core for aggregate revisions,
  // snapshot consistency, cycle detection, and damage routing (doc 13:48-51).
  // The returned span views the operator's own storage in declared order.
  // Default: an empty span -- leaf content is a graph leaf (doc 13:52).
  virtual std::span<const ContentRef> inputs() const { return {}; }

  // A nested content's child composition (doc 05), the exact mirror of `inputs()`:
  // core-visible graph structure, but naming an `ObjectId` in the document model
  // rather than a `ContentRef`, so it rides its own null-default discovery virtual
  // beside `editable()` / `audio()` / `inputs()`. The serializer reads it to reach
  // every child composition and to emit the core-owned `"composition"` reference
  // (doc 08 Principle 7) -- so the reference is re-derived from graph structure on
  // every save and never rides a kind's `params`. Default: `ObjectId{}`, "not a
  // composition reference" -- the answer for every kind but nested.
  virtual ObjectId composition_ref() const { return ObjectId{}; }

  // The authored URI a nested content's child composition was LOADED FROM, when
  // that child came from an external project file (doc 05:47-61). The child is by
  // then an ordinary composition in this document's model -- `composition_ref()`
  // names it and every downstream system sees an in-document child -- but it is not
  // this document's DATA. The kind answers the authored reference here so the
  // serializer names the child by that URI instead of hoisting the other document's
  // contents into this one's `contents` table (doc 08 Principle 3, Principle 7's
  // third corollary): a content answering non-empty emits neither an `inputs` array
  // nor a `composition` field, and the write traversal descends neither. The URI
  // itself rides the kind's `params`, the one thing the core does not own.
  //
  // Empty means "the child, if any, is document-local" -- the default, and the
  // answer for every kind but nested. Read-only discovery, never a write channel:
  // the core cannot set it, exactly as it cannot set `composition_ref()`.
  virtual std::string_view external_composition_ref() const { return {}; }

  // The authored URI of the external ASSET this content references -- an encoded
  // image, not a child composition (doc 08 Principle 3). This is the read-back
  // channel the kind's serialize codec uses to re-emit `params.source` VERBATIM AS
  // AUTHORED: never absolutised, never rewritten to the resolved URI, so a project
  // directory stays relocatable and `save(load(bytes)) == bytes` (doc 08:124-137).
  // Whether the asset actually loaded is invisible here -- an unavailable reference
  // is preserved exactly as a present one is, which is what makes a missing file a
  // condition of the environment rather than data loss (doc 08:126-134).
  //
  // Distinct from `external_composition_ref()` on purpose: an asset and a child
  // composition are different targets reached by different seams, and conflating
  // them would make the nested codec's "a body carrying BOTH a `composition` and a
  // `params.ref` is malformed" check incoherent. Named in `string_view`, so
  // `contract` still names no serialize type and no JSON.
  //
  // Empty means "this content references no external asset" -- the default, and the
  // answer for every kind but `org.arbc.image`. Read-only discovery, never a write
  // channel, exactly as `external_composition_ref()` is.
  virtual std::string_view external_asset_ref() const { return {}; }

  // Install the encoded bytes of the external asset `external_asset_ref()` names,
  // arriving LATE -- after construction, on the WRITER THREAD, because the
  // `AssetSource` deferred rather than answering inside `request()` (doc 08
  // Principle 3). Returns true iff the content is now available. The resolved URI
  // is deliberately not a parameter: the content received it at construction, so
  // the core cannot hand it the bytes of an asset it does not reference.
  //
  // An external asset has three load states, not two, and they are split by WHETHER
  // THE SOURCE ANSWERED, never by the bytes being empty: a source that answers with
  // empty bytes (or none installed at all, which fires the continuation inline) is
  // UNAVAILABLE; a source that has not answered yet is PENDING, and this is the
  // channel its bytes arrive through. A kind that does not override it has no
  // late-install channel, and its references are resolved-or-unavailable.
  //
  // A kind that DOES override it MUST publish the decoded EXTENT -- the geometry the
  // compositor culls on -- ATOMICALLY and AT MOST ONCE, so a worker rendering an
  // earlier pinned revision observes either the pre-arrival state or the final one,
  // never a partial install, and never a REVERSION to the pre-arrival state once the
  // asset has arrived. An asset whose extent could revert would cull itself out of
  // the composition and simply vanish.
  //
  // The decoded PIXELS carry no such obligation. A kind may treat them as BUDGETED
  // DERIVED DATA -- evictable under a byte budget and re-derivable BYTE-IDENTICALLY
  // from a retained encoded source -- so their history may legally be non-null ->
  // null -> non-null. `render_thread_safe() == true` then rests on IMMUTABLE VALUES
  // PLUS OWNING PINS: an evictable pixel store must hand a render an owning hold for
  // the duration of the call, so a concurrent eviction can never free what the render
  // is reading, and the value it holds is immutable for its whole life. That is what
  // buys the flag -- not monotonicity of any one pointer, which is what an earlier
  // form of this contract leaned on. A kind that does NOT evict keeps the simpler
  // reading (publish once, never replace), and both are sound for the same reason: a
  // worker never observes a partial or torn state.
  //
  // `org.arbc.image` takes the second form: its `PyramidCache` owns the decoded
  // pyramids, bounds them by a byte budget, and hands each render a residency pin.
  //
  // Undecodable bytes are a `false` return, never a throw -- errors are values across
  // the plugin boundary (doc 03:177-180).
  virtual bool install_asset(std::string_view /*encoded*/) { return false; }

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
class ARBC_API PullService {
public:
  PullService(const PullService&) = delete;
  PullService& operator=(const PullService&) = delete;
  virtual ~PullService() = default;

  // Render `input` for `request`, settling `done` exactly as `Content::render`
  // does -- inline via `done->complete`/`done->fail` or later off-thread.
  virtual void pull(ContentRef input, const RenderRequest& request,
                    std::shared_ptr<RenderCompletion> done) = 0;

  // Render `input`'s audio for `request`, settling `done` exactly as `pull`
  // settles a render (doc 12 Decision 5, doc 13:80). DEFAULTED, not pure: the
  // audio pull lands its stable signature now while leaving every existing
  // `PullService` implementer -- all of which predate audio -- byte-identical.
  // The one concrete override is `compositor`'s `PullServiceImpl::pull_audio`
  // (doc 17:56): `pull_audio` shares `pull`'s cache-first / worker-dispatch /
  // budget machinery, and the sole concrete `PullService` must answer both, so
  // the mix engine (`arbc::audio-engine`, an L4 peer that cannot inherit the
  // pure-virtual `pull`) calls this seam rather than subclassing it. The default
  // settles `done` as `unexpected(RenderError::ResourceUnavailable)` exactly
  // once: a service with no audio pull answers "no audio" safely and never hangs
  // the caller.
  virtual void pull_audio(ContentRef input, const AudioRequest& request,
                          std::shared_ptr<AudioCompletion> done);

protected:
  PullService() = default;
};

} // namespace arbc
