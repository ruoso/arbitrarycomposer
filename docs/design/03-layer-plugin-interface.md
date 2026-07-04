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

  // --- optional facets ---
  virtual AudioFacet* audio() { return nullptr; }  // audio capability
                                                   //  (doc 12)

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

  // --- change notification (outbound) ---
  // The core connects this on attach; content calls damage() when it changes.
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
  through their concrete types (a `RasterContent` has `set_image(...)`), not
  through the interface. The interface only needs mutation to be *visible*
  (damage + revision), not *expressible*. A generic property/parameter system
  can layer on later for editors and serialization without touching the
  render contract.

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
explicitly deferred; the async render path is the natural seam for it later.

## Registry

The core keeps a `Registry` mapping kind identifiers (reverse-DNS strings,
e.g. `org.arbc.raster`) to factories plus metadata (human name, version,
capability flags). The registry is what a future serialization format
references, so kind identifiers are part of the persistent contract from day
one even though serialization itself is deferred.

## Reference implementations (shipped with core)

| Kind id | Purpose |
| --- | --- |
| `org.arbc.solid` | Minimal sync content; the "hello world" plugin example. |
| `org.arbc.raster` | Exercises finite bounds, bounded scale range, tiling/mip pyramid, `achieved_scale < requested`. |
| `org.arbc.imageseq` | Timed visual content (doc 11); exercises `time`, `achieved_time`, spans, temporal prefetch. |
| `org.arbc.tone` | Audio-only content (doc 12); the "hello world" of the audio facet. |
| `org.arbc.fade` | Single-input operator (doc 13); both facets, `identity()` pass-through, `Timed`-over-`Static` aggregation. |
| `org.arbc.crossfade` | Two-input operator (doc 13); extent union, the temporal transition primitive. |
| `org.arbc.nested` | Recursive composition (doc 05); an operator over a child composition — exercises snapshots, async, cache layering, and both facets. |

Together these three cover every interesting branch of the contract, which
is the real reason they live in the core repo.
