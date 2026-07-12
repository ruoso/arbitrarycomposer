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

**The conversion operation.** `Backend::convert(dst, src)` rewrites `src`'s
pixels into `dst`'s tag triple: same geometry, position-for-position,
replacing every destination pixel. It blends nothing and takes neither a
transform nor an opacity — it is a transcode, and the compositing operation
set stays separate from it. Conversion routes format → premultiplied linear
working float → format (doc 07:104-108), so the CPU backend needs 2N codecs
rather than N×N kernels, and one variant dispatch per operation resolves the
source/destination pair. It is infallible: the closed, core-owned format set
makes the dispatch total, and the only unsupported-format failure — *can this
backend store that tag at all?* — is already surfaced as a value by
`make_surface`. A caller therefore holds two live surfaces before it can
call `convert`, and by then the answer is yes. This is the operation doc 07
rule 4's nesting boundary uses, and the one the import and display-out edges
will reuse.

**The clip-scoped operations.** Clearing and compositing each come in a
second form that carries a device-space (destination-space) clip rect and
writes no pixel outside it: `clear_rect(dst, device_rect, rgba)` and
`composite_clipped(dst, src, src_to_dst, opacity, device_clip)`. They exist
because damage-gated rendering repaints a *region* of a persisted target
(doc 02 step 5): the region must be cleared before it is re-composited, and
the composites must not spill past it, or source-over lands twice on the
pixels beyond the clear. The clip is intersected with the destination's
bounds and is half-open; an empty clip is a no-op, and a clip covering the
whole destination is byte-identical to the unclipped operation — which is
how the unclipped `clear`/`composite` are defined, so a backend carries one
kernel per operation, not two. This is a scissor rect: it is the shape a GPU
backend's command list wants, and it keeps the "core never loops over
pixels" rule intact — the core computes the region, the backend honors it.

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

**Surface pooling is a core-owned reuse layer.** Reconciling the two
statements above ("pooling by the core only," "a backend implements …
pooling"): the backend owns *allocation* (`make_surface`); the core owns the
*recycling policy* over it. Recycling is a core-owned `SurfacePool` that
composes over `make_surface`, not a per-backend virtual. The compositor
render path *acquires* a temp target from the pool — keyed by size +
`SurfaceFormat` — and the acquired RAII handle *releases* it when the frame
is done; release returns the surface to the pool's free list instead of
freeing it, so a subsequent same-key request reuses it rather than
reallocating (this is what "returned to the pool untouched," below, returns
it *to*). `acquire` is errors-as-values like `make_surface`, forwarding its
`SurfaceError` on a free-list miss. Recycled surfaces carry **undefined
contents** — a caller must clear or fully overwrite before reading (the
compositor already clears each temp). The pool is render-thread-confined
(see the Threading note): acquire/release run only where allocation runs, so
it needs no locking. A backend may still specialize allocation behind
`make_surface` (a GPU transient allocator, say) without changing this seam.

**Test doubles are part of the contract, and the contract ships them.** A
pure-virtual `Backend` means every added operation breaks every double in
the tree, and the repair is a dead stub — a line answering the compiler, not
documenting a behavior, unreachable by construction, and exactly the code
doc 16's diff-coverage gate is right to reject. The `surface` component
therefore ships the doubles alongside the contract, as header-only public
headers under `arbc/surface/testing/`, in namespace `arbc::testing`. There
are two, because doubles come in two shapes and the distinction is
load-bearing:

- **`StubBackend`** — every operation defaulted: no capabilities, no
  storage (`make_surface` yields `UnsupportedFormat`; errors as values, and
  a stub that has not been told how to allocate cannot honestly claim to),
  and no-op pixel operations. The base for a double whose test drives *no*
  real pixels.
- **`ForwardingBackend`** — a decorator over an injected `Backend&` that
  delegates *every* operation to it and adds no pixel behavior of its own.
  The base for a double that observes or perturbs a real backend (counting
  allocations, refusing one format). `CountingBackend` derives from it and
  tallies one call per operation — the behavioral counters of doc 16:54-62,
  which is what most doubles in the tree actually want.

A double derives from the base matching its shape and overrides **only** the
operations its test drives. The distinction matters because the two bases
absorb a *new* `Backend` operation differently and each is correct for its
shape: the stub no-ops it (a test that never drives it cannot notice), while
the forwarder delegates it (a decorator that silently no-oped an operation
its inner backend implements would produce wrong pixels quietly, rather than
failing loudly — the worse outcome, and the reason a decorator must never
inherit no-op defaults). A companion `StubSurface` — an abstract-`Surface`
implementation with no pixel storage — lets a double allocate without a real
backend, so a `surface`-level (L2) test can exercise the contract without
reaching for the CPU backend (L3).

These headers are in-library test support, distinct from the `arbc-testing`
static library (doc 17), which is the *content* conformance suite that
plugin authors link. They cost `libarbc` nothing (header-only, no objects)
and are public because an out-of-lib backend implementor needs them for the
same reason the in-tree tests do.

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

**Realization addendum (`surfaces.provided_surfaces`).** The contract above
is fully specified; the v1 implementation pins three points it left implicit:

1. **`provided` is a `SurfaceRef`.** The handle is a `std::shared_ptr<Surface>`
   whose deleter *is* the release callback to the content (the shared control
   block is the thread-safe atomic refcount the Threading note requires), plus
   the `transient` bool. The flag lives on the handle, not on `RenderResult`,
   since it is the *surface* whose lifetime it bounds. `RenderResult.provided`
   is a `std::optional<SurfaceRef>` — `nullopt` by default, so the common
   target-filling render stays a trivial by-value copy paying no atomic.
2. **The compositor v1 branch: composite inline, copy to cache.** At every
   `RenderResult`-consumption site a single helper honors `provided`: on the
   inline (non-cache) composite it composites *directly from* the provided
   surface (zero copy); on the cache path it **copies** the provided surface
   into a cache-owned `unique_ptr<Surface>` (a source-over blit over a freshly
   cleared destination) — for **both** transient and non-transient providers —
   so the tile cache stays entirely oblivious that a surface was ever provided.
   The `SurfaceRef` is released the instant the compositor has composited or
   copied it, never before, and is never stored in a structure outliving the
   frame. Zero-copy *adoption* of a non-transient provided surface as a cache
   value (holding the `SurfaceRef` in the cache rather than copying) is a
   parking-lot item: its payoff is real only on a GPU backend where the copy is
   a measured cost, and it forces a cache-layer change not yet warranted.
3. **v1 requires a working-space tag on a *provided* surface.** The
   tag-compatibility clause above (a differently-tagged provided surface
   converting **at composite time**) is latent until a backend advertises a
   second storable format: the CPU backend asserts tag agreement in
   `composite`, so v1 requires the provided surface to carry the composition
   working-space tag. Note this is narrower than it looks — explicit
   conversion *is* available (`Backend::convert`, above), and the nesting
   boundary uses it (doc 07 rule 4, `kinds.nested_working_space_conversion`);
   what remains deferred is folding the conversion *into* `composite` so a
   mismatched source needs no explicit step. That lands with the
   multi-format/GPU backend work (`color.kernels` / GPU backends) — a
   parking-lot item tied to that capability, not a standalone task.

## Threading note

Backend objects follow the doc-02 model: surface allocation and composite
run on the render thread (or its queue); typed CPU access from workers is
allowed for surfaces handed to render requests (the backend guarantees
those are unaliased until completion). External import/release callbacks
may arrive from content threads; backends must make import thread-safe or
marshal internally — this is a capability the CPU backend implements
trivially and GPU backends must plan for (context sharing/queues).
