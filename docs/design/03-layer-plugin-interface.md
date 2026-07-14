# 03 — The Layer Interface and Plugin Strategy

The layer interface is the load-bearing abstraction of the whole library.
This doc sketches it in C++ to make the contract concrete; names and
signatures are provisional.

## Design forces

1. **Pull-based**: the compositor asks for pixels; content never pushes them.
2. **Resolution in the request**: every request says what scale to render at.
   This is the mechanism of resolution independence.
3. **Two request disciplines**: interactive (deadline, best-effort, may
   answer async or decline) and exact (no deadline, must be faithful).
4. **Async-capable but sync-friendly**: trivial content (solid color) should
   not pay async ceremony; heavyweight content (3D engine, network) must not
   block a frame.
5. **Content is passive between requests** except for one outbound channel:
   damage notification.

## Sketch

```cpp
namespace arbc {

// Local-space geometry. double throughout — see doc 04.
struct Rect { double x0, y0, x1, y1; };

struct ScaleRange {
  double min = 0.0;                                   // 0 = no lower bound
  double max = std::numeric_limits<double>::infinity();
};

enum class Stability {
  Static,     // time-invariant; same request -> same pixels until damage
  Timed,      // deterministic function of request time (video, baked anim);
              //  cacheable per time — see doc 11
  Live,       // non-deterministic; cache only within a frame/snapshot
};

// What the compositor wants rendered.
struct RenderRequest {
  Rect     region;        // in content-local space
  double   scale;         // device pixels per local unit (the larger singular
                          //  value of the composed mapping; see doc 01)
  Time     time;          // content-local time, composed through the layer's
                          //  time map (doc 11); Static content ignores it
  Surface& target;        // backend surface sized ceil(region * scale)
  Exactness exactness;    // BestEffort (interactive) | Exact (offline)
  Deadline  deadline;     // meaningful only for BestEffort
  const SnapshotToken* snapshot; // revision fence, for nested compositions
};

struct RenderResult {
  double achieved_scale;  // == request.scale, or less (e.g. raster at native)
  bool   exact;           // faithful at achieved_scale?
  std::optional<Time> achieved_time;  // local time actually rendered, if
                          //  quantized (nearest source video frame — the
                          //  temporal achieved_scale; doc 11)
  std::optional<SurfaceRef> provided; // content-provided surface (doc 09)
};

class RenderCompletion {   // handed to async renderers
public:
  void complete(RenderResult);
  void fail(Error);
  bool cancelled() const;  // compositor lost interest (view moved on)
};

class Content {
public:
  virtual ~Content() = default;

  // --- description ---
  virtual std::optional<Rect> bounds() const = 0;  // nullopt = unbounded
  virtual ScaleRange scale_range() const = 0;
  virtual Stability stability() const = 0;
  virtual std::optional<TimeRange> time_extent() const = 0;
                          // temporal bounds() — nullopt for Static (doc 11)

  // --- rendering: implement one, the other has a default ---
  // Synchronous path. Return the result, or nullopt meaning
  // "I answered asynchronously / will complete via `done`".
  virtual std::optional<RenderResult>
  render(const RenderRequest&, std::shared_ptr<RenderCompletion> done) = 0;

  // Render concurrency declaration (doc 02:126-130). Default true: `render`
  // may run concurrently with itself on the worker pool. Return false when the
  // renderer is not internally thread-safe (a stateful decoder, a
  // single-context engine); the core then serializes that content's requests
  // through a per-content queue so at most one runs at a time. Orthogonal to
  // the sync/async axis above — externally-async content returns nullopt and
  // settles `done` later, occupying no worker regardless of this flag.
  virtual bool render_thread_safe() const { return true; }

  // --- optional facets ---
  virtual AudioFacet* audio() { return nullptr; }    // audio capability
                                                     //  (doc 12)
  virtual Editable* editable() { return nullptr; }   // document-state
                                                     //  editing (doc 14);
                                                     //  Live content omits

  // --- operator graph (doc 13) ---
  // Content may consume other content (effects, nested compositions).
  // Inputs are core-visible: aggregate revisions, damage routing, cycle
  // detection. Operators pull inputs through the PullService, never
  // directly.
  virtual std::span<const ContentRef> inputs() const { return {}; }
  virtual Rect map_input_damage(size_t input, const Rect&) const;
                                          // default: identity
  virtual std::optional<size_t> identity(const RenderRequest&) const;
                                          // pass-through short-circuit

  // A nested content's child composition (doc 05) is core-visible graph
  // structure exactly as inputs() is, but it names an ObjectId in the
  // document model rather than a ContentRef, so it rides its own
  // null-default accessor. The serializer reads it to reach child
  // compositions and to emit the core-owned "composition" reference
  // (doc 08 Principle 7). ObjectId{} means "not a composition reference" —
  // the default, and the answer for every kind but nested.
  virtual ObjectId composition_ref() const { return ObjectId{}; }

  // When a nested content's child composition was loaded from an external
  // project file (doc 05), the child is an ordinary composition in this
  // document's model — but it is not this document's DATA. The kind answers
  // the authored reference here so the serializer knows to name the child by
  // that URI instead of emitting it into the compositions table (doc 08
  // Principle 3). Empty means "the child, if any, is document-local" — the
  // default, and the answer for every kind but nested.
  virtual std::string_view external_composition_ref() const { return {}; }

  // The authored URI of the external ASSET this content references — an
  // encoded image, not a child composition (doc 08 Principle 3). The
  // serializer reads it back to re-emit `params.source` VERBATIM AS AUTHORED,
  // never absolutised and never rewritten to the resolved URI, so a project
  // directory stays relocatable and the bytes stay stable. Empty means "this
  // content references no external asset" — the default, and the answer for
  // every kind but image. Distinct from `external_composition_ref` because an
  // asset and a child composition are different targets reached by different
  // seams; conflating them would make doc 08's "a body carrying both a
  // `composition` and a `params.ref` is malformed" check incoherent.
  virtual std::string_view external_asset_ref() const { return {}; }

  // Install the encoded bytes of the external asset above, arriving LATE —
  // after construction, on the WRITER THREAD, because the AssetSource deferred
  // rather than answering inside request() (doc 08 Principle 3's three load
  // states). Returns true iff the content is now available. Undecodable bytes
  // are a `false` return, never a throw. A kind that does not override it has
  // no late-install channel and its references are resolved-or-unavailable.
  //
  // A kind that DOES override it must publish the decoded EXTENT — the geometry
  // the compositor culls on — ATOMICALLY and AT MOST ONCE, so a worker rendering
  // an earlier pinned revision observes either the pre-arrival state or the final
  // one, never a partial install, and never a REVERSION once the asset arrived.
  // An asset whose extent could revert would cull itself out of the composition
  // and vanish.
  //
  // The decoded PIXELS carry no such obligation. A kind may treat them as
  // BUDGETED DERIVED DATA — evictable under a byte budget, re-derivable
  // BYTE-IDENTICALLY from a retained encoded source (doc 15 § Memory
  // populations) — so their history may legally be non-null → null → non-null.
  // render_thread_safe() == true then rests on IMMUTABLE VALUES + OWNING PINS:
  // an evictable pixel store hands each render an owning residency pin (doc
  // 02 § Tile cache), so a concurrent eviction can never free what the render is
  // reading, and the value it holds is immutable for its whole life. THAT is what
  // buys the flag — not monotonicity of any one pointer, which is what an earlier
  // form of this contract leaned on (and which itself replaced an even older
  // "immutable after construction" argument). A kind that does NOT evict keeps
  // the simpler reading — publish once, never replace — and both are sound for
  // the same reason: a worker never observes a partial or torn state.
  // `org.arbc.image` takes the second form.
  virtual bool install_asset(std::string_view encoded) { return false; }

  // --- change notification (outbound) ---
  // The core connects this on attach; content calls damage() when it changes.
  // An editable content's state sinks (doc 14's Editable facet) bind the same
  // way — the runtime registers them onto the live Model/Journal on instantiate
  // and clears them on release.
  void set_damage_sink(DamageSink);

protected:
  void damage(std::optional<Rect> region);  // nullopt = everything
};

} // namespace arbc
```

Points worth calling out:

- **One render entry point, sync and async unified.** A solid-color layer
  fills the surface and returns a `RenderResult` inline. A 3D-engine layer
  kicks its own pipeline, stores `done`, returns `nullopt`, and completes
  later. The compositor treats "returned inline" as an immediately-completed
  async request; there is one code path.
- **Content declares its render concurrency (doc 02:126-130).**
  `render_thread_safe()` defaults to true — the worker pool may render a
  thread-safe content's tiles in parallel. Content whose renderer is not
  internally thread-safe returns false and the core funnels its requests
  through a per-content serialization queue (one in-flight render at a time),
  rather than forcing every author to add their own lock. The *mechanism* —
  the worker pool and the per-content queue — is runtime policy (doc 02's
  threading model, doc 17:60, `runtime.threading`); the *declaration* lives
  here on the contract so the core can route without a downcast.
- **`cancelled()`** lets long renders abandon work when the user has zoomed
  elsewhere. Cooperative, best-effort.
- **`Exact` requests may take unbounded time but must be faithful.** A
  content implementation that cannot honor exactness at the requested scale
  reports `achieved_scale`/`exact=false` honestly and the offline renderer
  surfaces that to the host (e.g. "raster layer X limited output to 0.4×").
- **The target surface is allocated by the compositor**, not the content.
  This keeps surface formats, pooling, and backend ownership in the core.
  Content that inherently produces its own texture (3D engine, video
  decoder) may instead answer with a **content-provided surface** — an
  optional field on `RenderResult` — rather than filling the target; the
  adoption, lifetime, and sync rules are in doc 09.
- **Parameters and editing**: content objects are constructed and mutated
  through their concrete types (a `RasterContent` has `paint(...)`), not
  through the interface — but mutation follows the transactional discipline
  of doc 14 (mutations take a transaction; editable kinds implement the
  `Editable` facet with cheap structurally-shared state capture). The
  render contract itself only needs mutation to be *visible* (damage +
  revision) and rendering to be pure over the pinned state.

## Plugin mechanism

Two stages, deliberately:

**Stage 1 (v1): C++ interface, in-process, same-toolchain.**
A layer kind is a library exposing factory functions returning
`std::unique_ptr<arbc::Content>`. "Plugin" initially just means "not compiled
into the core" — link-time or `dlopen` with a single
`extern "C" arbc_plugin_register(Registry&)` entry point. This is the
standard C++ ecosystem tradeoff (compiler/ABI coupling, as with Qt or
OpenFX hosts compiled per-platform) and it is acceptable while the interface
is still moving.

**Stage 2 (post-v1): stable C ABI shim.**
Once the C++ interface stops churning, define a C ABI mirroring it
(vtable-as-struct, versioned, semver-gated capability flags) so plugins
survive compiler and library-version differences. The v1 interface is shaped
with this in mind — hence: no exceptions across the boundary (errors are
values), no STL types in the hot structs (`Rect`, `RenderRequest` are plain
data; the `std::optional`/`shared_ptr` conveniences shown above get C
equivalents), and all allocation ownership one-directional.

Out-of-process isolation (a crashing plugin not taking down the host) is
explicitly deferred; the async render path is the natural seam for it
later, and the file-backed arena model (doc 15) supplies the substrate — a
plugin process can map the document workspace read-only and render from a
pinned version it cannot corrupt.

## Registry

The core keeps a `Registry` mapping kind identifiers (reverse-DNS strings,
e.g. `org.arbc.raster`) to factories plus metadata (human name, version,
capability flags). The registry is what a future serialization format
references, so kind identifiers are part of the persistent contract from day
one even though serialization itself is deferred.

In v1 the metadata a kind advertises is its human name and version; the
capability flags named above are deferred to their first consumer — they
arrive with Stage 2's semver-gated capability negotiation, and the
`KindMetadata` descriptor grows the field then rather than carrying an unused
one now. Kind identifiers are validated only for non-emptiness and
per-registry uniqueness: they are opaque persistent tokens, not structurally
parsed, so reverse-DNS is a collision-avoidance convention rather than an
enforced grammar and a serializer can round-trip an unknown or future-namespace
id losslessly (doc 08). A `Registry` is populated during single-threaded
startup or plugin load and is read-only for the remainder of a session;
concurrent host-side discovery and loading is `runtime`'s concern (doc 17),
not the registry seam's.

## Reference implementations (shipped with core)

| Kind id | Purpose |
| --- | --- |
| `org.arbc.solid` | Minimal sync content; the "hello world" plugin example. |
| `org.arbc.raster` | Exercises finite bounds, bounded scale range, tiling/mip pyramid, `achieved_scale < requested`. **Codec-free and editable**: it accepts decoded buffers, carries the `Editable` facet (doc 14), and its painted pixels are document state (doc 08 Principle 8). |
| `org.arbc.image` | A still image decoded from an external asset URI. Read-only — **no `Editable` facet** — and stores no pixels in the document (doc 08 Principle 3). Exercises content-provided surfaces (doc 09) and the unavailable-reference path (doc 05). Lives outside `libarbc`; see below. |
| `org.arbc.imageseq` | Timed visual content (doc 11); exercises `time`, `achieved_time`, spans, temporal prefetch. |
| `org.arbc.tone` | Audio-only content (doc 12); the "hello world" of the audio facet. |
| `org.arbc.fade` | Single-input operator (doc 13); both facets, `identity()` pass-through, `Timed`-over-`Static` aggregation. |
| `org.arbc.crossfade` | Two-input operator (doc 13); extent union, the temporal transition primitive. |
| `org.arbc.nested` | Recursive composition (doc 05); an operator over a child composition — exercises snapshots, async, cache layering, and both facets. |

Together these cover every interesting branch of the contract, which
is the real reason they live in the core repo.

**`org.arbc.image` and `org.arbc.imageseq` are the exceptions to this section's
heading.** Both live in the core *repo* but ship *outside* `libarbc`, as separate
shared-library artifacts (`arbc-plugin-image`, `arbc-plugin-imageseq`) carrying
their own decode dependency (stb-class, vendored once and shared) — the
resolution of doc 17's "codec line" (a codec must never enter an embedder's link
line, doc 10). They are therefore the permanent, out-of-lib exercise of the
`extern "C" arbc_plugin_register(Registry&)` path above; the other kinds link
into `libarbc` and only exercise that path through the CI-only dual-build
(doc 17). See doc 17 "The codec line".

**Why `image` is a kind of its own and not a mode of `raster`.** They differ in
the two ways that matter most, and collapsing them would forfeit both. `raster`
is *codec-free and editable* — it takes decoded buffers, so it stays inside
`libarbc`, and it carries the `Editable` facet, so its pixels are irreplaceable
document state. `image` is *codec-carrying and read-only* — it decodes a
referenced file, so it must live behind the codec line, and it has no `Editable`
facet, so it never stores a pixel in the project. Merging them would drag a
decoder into `libarbc` and make "is this layer's content recoverable from its
source file?" a runtime property rather than a static one. Keeping them apart is
what lets doc 08 answer that question from the kind id alone — and it is what
makes non-destructive editing *structural*: you retouch a photograph by stacking
an editable `raster` over a referenced `image`, because the `image` cannot be
painted on at all.
