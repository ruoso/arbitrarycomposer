# 09 — Surfaces and Backends

The surface abstraction is the seam between the compositor core, layer
content, and whatever actually owns pixels (CPU memory today, GPU textures
later). This doc resolves the "content that natively produces textures"
question flagged in doc 03.

## Surface

A `Surface` is an opaque, backend-owned handle to a 2D pixel target, tagged
with (doc 07): pixel format, color space, premultiplication, and size in
device pixels. Public operations:

- allocation/pooling — **by the core only**; content receives targets,
- typed CPU access where the backend supports it
  (`span<Format>()`, checked against the tag; may be unavailable on GPU
  surfaces without an explicit, deliberately-noisy readback),
- backend-internal composite/blit/resample operations consumed by the
  compositor,
- **import/export of external handles** — see below.

## Backend contract

A backend implements: surface allocation and pooling, the composite
operation set (draw surface onto surface under affine transform + opacity,
doc 02 step 5), format/space conversion kernels (doc 07), and capability
flags (CPU access? external import? which handle types? sync primitives?).

- **CPU reference backend (v1):** surfaces are memory buffers; kernels are
  the doc-07 templated loops; everything supports typed access; import is
  wrap-or-copy of caller memory. Exists for correctness, tests, and hosts
  without GPU assumptions.
- **GPU backends (post-v1, design-constraining now):** surfaces are
  textures; composite is draw calls; the frame's step-5 composite becomes a
  render pass. The design rules that keep this reachable: surfaces stay
  opaque handles, pixel readback is explicit and avoidable, all composite
  operations route through the backend (the core never loops over pixels
  itself), and per-frame work is expressible as a command list rather than
  interleaved immediate calls.

One composition tree renders through one backend at a time; mixing backends
(CPU-rendered content composited on GPU) is handled by import at the
boundary, not by multi-backend surfaces.

**Capability descriptor.** A backend advertises the flags above through a
value `BackendCaps` — `cpu_access` (is typed CPU access available?),
`import_handles` (a bitmask over the handle set: CPU memory, GL texture,
Vulkan image, DMA-BUF), and `sync_primitives` — queried through
`Backend::capabilities()`. A backend advertises only what it currently
implements: the CPU reference backend reports `cpu_access` and no import or
sync support until those land. Typed CPU access is gated on capability *and*
tag: a surface hands out a span only when its backend advertises
`cpu_access` and the requested view matches the surface's tag.

**Surface creation is errors-as-values (doc 10).** Allocation returns
`expected<Surface, SurfaceError>`, not a null handle: a backend that cannot
store a requested format yields a `SurfaceError` value (never null, never an
abort), so a capability-negotiating caller sees the reason. The closed,
core-owned format set (doc 07) makes the answer a decision over an
enumerable universe.

## Content-provided surfaces (texture adoption)

Doc 03's contract has the compositor allocate the target and content fill
it. That's right for the common case but forces a copy on content that
already owns a rendered target — a 3D engine's framebuffer, a video
decoder's output, a camera feed. Resolution:

**`RenderResult` gains an optional content-provided surface.** Content may
answer a request either by filling the provided target (default) or by
returning its own `Surface` covering the requested region at the achieved
scale:

```cpp
struct RenderResult {
  double achieved_scale;
  bool   exact;
  std::optional<SurfaceRef> provided;  // instead of the request's target
  // provided != target implies: compositor composites/caches from
  // `provided`; the request's target is returned to the pool untouched.
};
```

- A content-provided surface must carry compatible tags or be convertible;
  the backend converts at composite time if needed (doc 07 rules apply —
  the 3D engine's sRGB8 framebuffer converting into a linear-f16 composite
  is the expected common case, not an error).
- **Lifetime:** `provided` is refcounted with a release callback to the
  content, and is pinned only until the compositor has composited *or*
  copied it into cache. Content that reuses its framebuffer every frame
  (typical engine) marks the surface `transient`: the compositor must
  consume it within the current frame and may not cache it directly (it
  copies if it wants to cache — correct for `Volatile`-stability content
  anyway, which is what live engines declare).
- The external handle enters through **backend import**: GL texture,
  Vulkan image + layout, DMA-BUF, or plain CPU memory, per backend
  capability flags. Import carries an optional **sync primitive** (fence/
  semaphore/EGL sync) the backend waits on before sampling; completion
  hands a release sync back. The CPU backend implements import as
  wrap (when given memory) or copy (when given anything else it can read
  back), so the API path is exercised from day one even though zero-copy
  only pays off on GPU.

This keeps the pull contract intact — the request still names region and
scale, and content that provides its own surface must still honor them —
while removing the forced copy exactly where it hurts.

## Threading note

Backend objects follow the doc-02 model: surface allocation and composite
run on the render thread (or its queue); typed CPU access from workers is
allowed for surfaces handed to render requests (the backend guarantees
those are unaliased until completion). External import/release callbacks
may arrive from content threads; backends must make import thread-safe or
marshal internally — this is a capability the CPU backend implements
trivially and GPU backends must plan for (context sharing/queues).
