# kinds.image — `org.arbc.image`, external still image, out-of-lib

## TaskJuggler entry

[`tasks/55-kinds.tji:27-32`](../../55-kinds.tji):

```
task image "org.arbc.image — external still image, out-of-lib" {
  effort 3d
  allocate team
  depends kinds.imageseq_plugin, surfaces.provided_surfaces, serialize.kind_params
  note "The still sibling of org.arbc.imageseq, and the half of the pixel-persistence
        split that stores NOTHING (doc 08 Principle 3). ... Docs 03/08/09/17."
}
```

## Effort estimate

**3d.** Tight but achievable, because the two expensive halves already exist and are
reused rather than rebuilt: the plugin artifact shape (`plugins/imageseq/CMakeLists.txt`,
a MODULE + impl-STATIC pair with a private decode dep) and the resampling kernels
(`src/media/arbc/media/image_resampler.hpp` — Lanczos-3 half-band decimate,
Catmull-Rom bicubic magnify, both header-only and plugin-legal). What is genuinely new
is small: an immutable mip pyramid over a decoded master (~100 lines, no CoW, no
`StateHandle`, no pool — see Decision 4), a URI-only serialize codec in `runtime`, and
one new `Content` accessor.

The estimate assumes the implementer does **not** reimplement raster's tile machinery.
If Decision 4 is reversed, this is a 5d task.

## Inherited dependencies

**Settled (may be relied on):**

- **`kinds.imageseq_plugin`** (`complete 100`) — the out-of-lib plugin template.
  `plugins/imageseq/CMakeLists.txt:14-30` establishes the two-target shape: an
  `arbc-plugin-imageseq-impl` **STATIC** archive (linked by the in-repo tests, so they
  construct content directly without `dlopen`) plus an `arbc-plugin-imageseq` **MODULE**
  carrying only the entry-point TU. Hand-rolled, *never* `arbc_add_component` (which
  folds objects into `libarbc` and would drag the decoder into core). The vendored
  decoder is `plugins/imageseq/third_party/imdec.h` — an stb-shaped C API
  (`imdec_info_from_memory`, `imdec_load_from_memory`, `imdec_free`) with
  `IMDEC_IMPLEMENTATION` in exactly one TU (`plugins/imageseq/imdec.cpp`).
  The registration ABI is the single symbol
  `extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry&)`
  (`src/contract/arbc/contract/plugin.hpp:20`).
- **`surfaces.provided_surfaces`** (`complete 100`) — `arbc::SurfaceRef`
  (`src/surface/arbc/surface/surface_ref.hpp:29-55`): a `std::shared_ptr<Surface>`
  whose **deleter is the content-supplied release callback**, plus a `transient` bool on
  the handle. Carried as `std::optional<SurfaceRef> provided` on `RenderResult`
  (`src/contract/arbc/contract/content.hpp:119`). Consumed through
  `consume_provided(...)` (`src/compositor/arbc/compositor/provided_surface.hpp:36-42`),
  which releases *after* composite/copy, never before.
- **`serialize.kind_params`** (`complete 100`) — `Codec{SerializeFn, DeserializeFn}` and
  the `CodecTable` keyed by reverse-DNS kind id (`src/serialize/arbc/serialize/codec.hpp:40-70`).
  Crucially, `DeserializeFn` already receives a `LoadContext&`
  (`src/serialize/arbc/serialize/deserialize.hpp:46-48`), and the routing entry point
  already receives **both** the `CodecTable` and the `Registry`:
  `content_body_from_json(body, inputs, composition, codecs, registry, ctx, params_residual)`
  (`codec.hpp:109-112`). A `CodecTable::find` miss falls through to `PlaceholderContent`,
  which preserves `kind`/`kind_version`/`params`/`inputs` **verbatim** — "a missing plugin
  must never destroy data."

**Pending (must NOT be assumed):**

- **`packaging.plugin_helper`** (M9) — the shipped `arbc_add_plugin()` CMake helper.
  Not landed; `plugins/imageseq/CMakeLists.txt:6-8` flags it as the eventual replacement
  for the hand-rolled shape. This task hand-rolls again, matching imageseq. Do not block
  on the helper.
- **`runtime.plugin_operator_registration`** (registered, unbuilt) — a general seam for a
  plugin to register a *codec* across the ABI. **This task deliberately does not need it**
  (Decision 2). Do not build it here.
- **`serialize.raster_tile_store`** and **`surfaces.import`** are *downstream* of this task,
  not upstream. They consume the contract this refinement settles; they supply nothing.

## What this task is

Build `org.arbc.image`: a **visual `Static` leaf kind** whose params carry a **URI to an
external encoded image** and which stores **no pixels in the document at all**. It resolves
that URI through the core's existing `LoadContext` asset hook, receives the encoded bytes,
decodes them once into a working-space master, builds an immutable mip pyramid over it,
and serves pulls as a **refcounted, non-transient content-provided surface covering exactly
the requested region at the achieved scale** (doc 09:157-160).

It is deliberately **read-only** — it implements **no `Editable` facet**. That single
omission is what makes non-destructive editing *structural* rather than conventional: you
cannot paint on a photograph, so retouching one *must* stack an editable `org.arbc.raster`
above a referenced `org.arbc.image` (doc 03:256-268).

It ships **outside `libarbc`** as a new `arbc-plugin-image` MODULE artifact, behind doc 17's
codec line, sharing imageseq's already-vendored stb-class decoder rather than vendoring a
second one. `libarbc` stays codec-free.

A missing or unreadable asset resolves to an **unavailable** reference — no pixels, the
authored URI preserved verbatim, the layer re-saving byte-identically — and is **never a
load error** (doc 08:126-134, doc 05:77-83).

## Why it needs to be done

This is the half of the pixel-persistence split that **stores nothing** (doc 08 Principle 3),
and it is the foundation of the image-editor cluster: `serialize.raster_tile_store` and
`surfaces.import` both build on the contract it settles.

Without it, a project must re-store every imported photograph as raster tiles — **~490 MB
against ~32 MB** for a 30-layer 24 MP composition — and no compressor closes that gap,
because after content-addressed dedup photographic tiles are 93% of the bytes and compress
only ~2.1x (sensor noise is incompressible; doc 08 Principle 8, doc 17:226-239). The codec
line is usually argued as a dependency-hygiene rule; here it also turns out to be the thing
that keeps project files small.

It is also the **first production caller of `LoadContext::load_asset`** — the asset hook
`serialize.reader` shipped as an interface and left deliberately unused
(`src/serialize/arbc/serialize/load_context.hpp:25-30`: "a filled-in loader lands with the
kinds that first need it"). That hook has had **no production caller at all** until now.

## Inputs / context

### Governing design-doc sections (normative — doc 16)

- **doc 03:236** — the reference-kinds table row: *"A still image decoded from an external
  asset URI. Read-only — **no `Editable` facet** — and stores no pixels in the document
  (doc 08 Principle 3). Exercises content-provided surfaces (doc 09) and the
  unavailable-reference path (doc 05). Lives outside `libarbc`."*
- **doc 03:256-268** — *"Why `image` is a kind of its own and not a mode of `raster`."*
  They differ in exactly two ways: `raster` is *codec-free and editable*; `image` is
  *codec-carrying and read-only*. Merging them "would drag a decoder into `libarbc` and make
  'is this layer's content recoverable from its source file?' a runtime property rather than
  a static one."
- **doc 03:33-38, :76-78, :98-100** — `Stability::Static` ⇒ `time_extent() == nullopt`;
  facets are null-defaulted virtuals (`editable()`, `audio()`). A read-only kind simply
  leaves `editable()` at its `nullptr` default.
- **doc 08:116-145** — Principle 3. Dedup is keyed on the **resolved** URI; the **authored**
  reference is what round-trips (never absolutised, so a project directory stays relocatable
  and output stays byte-stable). *"An imported image has a file it came from, so it
  serializes as a URI and nothing more."*
- **doc 08:124-134** — the unavailable path: *"A reference that cannot be loaded — no asset
  source installed, a missing or unreadable file, a depth-cap overrun — is reported as
  **unavailable**, not as a read error."* The asymmetry with a dangling in-document id (a
  read error) is deliberate: *"a missing external file is a condition of the environment
  that may resolve later."*
- **doc 08:21-35** — the asset directory. `project.assets/bg.png` is addressed by relative
  URI and *"resolved through the same `LoadContext` asset hook that external nested projects
  use, so one resolution seam serves both."* The corrected example (08:56-64) now has
  `org.arbc.image` with `"params": { "source": "assets/bg.png" }`.
- **doc 09:150-182** — content-provided surfaces. **09:157-160 is the load-bearing line:**
  content may answer a request *"either by filling the provided target (default) or by
  returning its own `Surface` **covering the requested region at the achieved scale**."*
  **09:176-182** — lifetime: refcounted with a release callback, pinned until the compositor
  has composited *or* copied it.
- **doc 09:219-230** — v1 requires the **composition working-space tag** on a provided
  surface; the CPU backend asserts tag agreement in `composite`.
- **doc 17:215-250** — the codec line. **17:241-250 is the load-bearing line:** *"The
  distinction is **what is being parsed**, not whether bytes get smaller."* `zstd` does not
  cross the line because it compresses bytes *we* produced in a container *we* defined and
  parses no foreign format. This is the test that Decision 2 applies to the *serialize* codec.
- **doc 10:29** — Image codecs: *"not core — and no in-lib kind needs one."*
- **doc 05:77-96** — *pending* and *unavailable* differ in exactly one bit and only for the
  loader. *"Both render the placeholder, both keep their `ref`, and both re-save
  byte-identically as the authored URI."*

### Source seams this task extends

- `src/serialize/arbc/serialize/load_context.hpp:31-39` — `class AssetSource`, pure virtual
  `request(std::string_view resolved_uri, std::function<void(std::string_view bytes)> on_ready)`.
  **Empty bytes == absence.** `:62-103` — `LoadContext::resolve` (dedup by resolved
  identity), `resolved_uri`, `set_asset_source`, `load_asset`. With no source installed,
  `src/serialize/load_context.cpp:104` fires `on_ready(std::string_view{})` immediately —
  unavailable, never blocking.
- `src/runtime/filesystem_asset_source.cpp` / `src/runtime/arbc/runtime/filesystem_asset_source.hpp:27`
  — the production `AssetSource`. It **fires `on_ready` INLINE, before `request` returns**.
  This is what makes the synchronous v1 of Decision 5 correct in production.
- `src/runtime/arbc/runtime/builtin_codecs.hpp:95-96` — `nested_codec()` /
  `nested_codec(ExternalCompositionLoader* loader)`. **The closure-injected-dependency
  precedent** Decision 2 copies.
- `src/runtime/document_serialize.cpp:157-165` — `builtin_codecs()`, the table
  (solid/tone/fade/crossfade/nested — note `raster` has **no** codec today).
- `src/serialize/arbc/serialize/codec.hpp:109-112` — `content_body_from_json(..., const
  CodecTable& codecs, const Registry& registry, LoadContext& ctx, ...)`. The routing
  **already holds the `Registry`**, which is what makes Decision 2 need no new plumbing.
- `src/contract/arbc/contract/content.hpp:623` — `virtual std::string_view
  external_composition_ref()`, the read-back channel for a nested child's URI. Decision 3
  mirrors it for assets.
- `src/contract/arbc/contract/registry.hpp:35-40` — `using ContentConfig = std::string_view`
  (**opaque, kind-defined**) and `ContentFactory = std::function<expected<std::unique_ptr<Content>,
  std::string>(ContentConfig)>`. Decision 5 carries the fetched bytes through this.
- `src/media/arbc/media/image_resampler.hpp:59-161` — `lanczos3_half_band()`,
  `decimate_half_band(dst_x, dst_y, fetch)`, `catmull_rom_weights()`,
  `sample_bicubic(x0, y0, fx, fy, fetch)`. Header-only templates over a `Fetch` functor,
  in `media` (L2) — **plugin-legal**, and the same kernels `kinds.raster_resampling_quality`
  landed. Reuse; do not write new filters.
- `src/kind_raster/raster_content.cpp:500-550` — `RasterContent::render`, the reference for
  honest `achieved_scale`: an `Exactness::Exact` request renders at the requested scale
  (bicubic-upsampling past native); a `BestEffort` request **clamps at native** and reports
  `achieved_scale < request.scale` honestly (`exact = (achieved == s)`).
- `plugins/imageseq/imageseq_content.cpp:54-71` — `class FrameSurface final : public Surface`,
  plugin-owned CPU memory (`std::vector<std::byte>`), format `k_working_rgba32f`, *no*
  `Backend` edge. `:158-165` — the decode-to-working-space conversion
  (`PixelTraits<Rgba8Srgb>::decode` → `PixelTraits<Rgba32fLinearPremul>::encode`).
  `:204-206` — `result.provided.emplace(*frame, [frame]{...}, /*transient=*/false)`.

### Predecessor decisions that bind

- **Convert at decode, not at composite.** Settled by precedent (`tasks/parking-lot.md:83`,
  2026-07-12 triage): the image kinds convert `Rgba8Srgb` → `WorkingPixel` →
  `Rgba32fLinearPremul` at decode and hand back a surface tagged `k_working_rgba32f`.
  **No foreign tag reaches the compositor from `org.arbc.image`**, and convert-at-composite
  must not be built on its account.
- **Enforcing tests live under `tests/`, never `plugins/`.** `scripts/check_claims.py:32`
  scans only `src/`, `tests/`, `testing/`.
- **Link order is a house rule:** `arbc-testing` **before** `arbc`
  (`cmake/ArbcComponent.cmake:86-92`).

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced by `scripts/check_levels.py`).** The decode
   dependency must never resolve in `libarbc`, in any `arbc_*` component, or in
   `arbc-testing`. The plugin links the umbrella `arbc` PUBLIC and its decoder PRIVATE.
   Pixels route only through `arbc::media` (`PixelTraits<F>`, `visit_surface`, checked
   `span<F>()`) — **never `backend-cpu` kernels** (L3, forbidden to a plugin), and the plugin
   allocates **no** surfaces through a `Backend` (it owns its own storage, as
   `FrameSurface` does).
2. **No `Editable` facet.** `editable()` stays at its `nullptr` default. This is not an
   omission to be "fixed" later — it is the load-bearing property (doc 03:261-268).
3. **`Stability::Static`.** `time_extent() == nullopt`, `quantize_time` keeps the `nullopt`
   default, `achieved_time` stays `nullopt`, `audio() == nullptr`, `inputs()` empty.
4. **Stores nothing.** The document body carries the authored URI and nothing else. No
   pixels, no tiles, no intrinsic size, no decoded cache — an `org.arbc.image` layer's
   `params` is exactly `{"source": "<authored-uri>"}`.
5. **The authored reference round-trips verbatim** — never absolutised, never rewritten to
   the resolved URI (doc 08:124-126). A document that says `assets/bg.png` saves back saying
   `assets/bg.png`.
6. **Unavailable is never a read error** (doc 08:126-134). A missing/unreadable/undecodable
   asset, or no `AssetSource` installed, yields a content with no pixels; the parent document
   **loads successfully**.
7. **Errors are values; no exception crosses the plugin boundary** (imageseq Constraint 7).
   A corrupt file is a `RenderError::ResourceUnavailable` or an unavailable content — never UB,
   never a throw.
8. **The provided surface carries the composition working-space tag** (doc 09:219-230).
9. **A provided surface covers the requested region at the achieved scale** (doc 09:157-160)
   — never a whole decoded frame. See Decision 1; this is the constraint that bounds every
   cache copy to tile size.

## Decisions

### Decision 1 — The provided surface covers exactly the requested region at the achieved scale, never a whole decoded frame

`render()` returns a `SurfaceRef` sized to `request.target`'s extent, holding the requested
region resampled from the pyramid at the achieved scale. It is **non-transient** and
refcounted; the release callback returns the surface to a small plugin-owned free-list.

**Rationale.** This is what doc 09:157-160 *already says* ("covering the requested region at
the achieved scale"); it is not a new decision so much as a refusal to copy imageseq's
shortcut. `ImageSeqContent::render` (`imageseq_content.cpp:204-206`) hands back the **whole
decoded frame** and sets `achieved_scale = request.scale`. That is harmless for a 2×2 test
fixture and catastrophic for a 24 MP photograph: the compositor tiles, so each of the ~100+
tile pulls would hand over a full-frame surface that the cache then **copies** — at `rgba32f`,
a ~384 MB copy *per cache insert*.

`tasks/parking-lot.md:75` (2026-07-12 triage) flagged exactly this and deferred the call to
this task: *"check whether `org.arbc.image` should return only the requested region at the
requested scale rather than a whole decoded frame — which is what the pull contract already
asks for."* It does. Region-at-achieved-scale bounds every cache copy to one tile.

**Consequence for the parking lot:** that entry ("Zero-copy adoption of a non-transient
provided surface as a cache value") **does not become a leaf.** Its own stated trigger was
"*if* it does hand over whole frames"; it does not, so the copy it worried about never
reaches 384 MB and the cache-layer `TileValue` rework stays unwarranted. The closer should
record this resolution against that entry.

**Rejected — fill `request.target` (raster's idiom, `raster_content.cpp:500-550`).** Strictly
cheaper per pull in v1: one resample write into a core-allocated target, with no pooled
allocation and no cache copy. But it forfeits the role doc 03:236 explicitly assigns this
kind ("Exercises content-provided surfaces"), forfeits doc 09:206-218's zero-copy *inline*
composite path (where the compositor composites directly **from** the provided surface), and
forecloses the resident-tile fast path that makes the provided path strictly better once the
pyramid is tiled. The doc assigns the role; we take it.

**Rejected — hand over the whole decoded frame (imageseq's idiom).** The 384 MB-per-insert
copy above. imageseq's rationale — one decode amortised over many tiles of a *transient,
per-time* frame — does not transfer: a still image decodes exactly once and has no per-time
reuse pressure to amortise.

### Decision 2 — The serialize codec lives in `runtime`; only the *decoder* crosses the codec line

`src/runtime/codec_image.cpp` registers a codec for `org.arbc.image` in `builtin_codecs()`,
even though the *kind* ships out-of-lib. The codec is a closure over the `Registry`:
`image_codec(const Registry* registry)`, mirroring `nested_codec(ExternalCompositionLoader*)`
(`builtin_codecs.hpp:95-96`). It is registered **iff** the runtime's `Registry` holds an
`org.arbc.image` factory — i.e. iff the plugin is loaded.

**Rationale.** This is forced. A plugin *cannot* register a codec today: the entire plugin ABI
is `arbc_plugin_register(Registry&)` (`plugin.hpp:20`), and `Registry` exposes only
`add(id, factory, metadata)`. Exposing codec registration across the ABI would mean putting
`nlohmann::json` in the plugin's link surface — which is precisely what
`serialize.kind_params` Decision 1 *removed* from `contract` (json is linked PRIVATE to
`arbc_serialize`; `codec.hpp` is deliberately not a public header). So the choice is: violate
that, or put the codec in core.

Doc 17:241-250 supplies the test, and it resolves cleanly: *"The distinction is **what is
being parsed**, not whether bytes get smaller."* `zstd` stays in core because it parses no
foreign format. **A serialize codec for `org.arbc.image` parses our own JSON and a URI
string — no image bytes ever touch it.** The decoder — the thing that parses a third-party
format over untrusted input — stays in the plugin. **The codec line is a *decoder* line.**
This carries a doc 17 delta making that explicit, and a doc 00 decision-record bullet, because
it is the general answer for *every* future plugin kind's persistence.

**Missing plugin ⇒ no factory ⇒ no codec registered ⇒ `PlaceholderContent` fall-through**,
which preserves `kind`/`kind_version`/`params` verbatim. A user without the image plugin
opens the document, sees a placeholder, saves, and loses nothing. This needs **zero new
machinery** — it is the existing `CodecTable::find`-miss path.

**Rejected — build `runtime.plugin_operator_registration` (plugin-side codec registration)
first.** It is not a dependency of this task, it is a substantially larger seam (a
json-free codec ABI), and it would gate the image-editor cluster behind it. When it lands,
`codec_image.cpp` can migrate; nothing here forecloses that.

**Rejected — let the plugin do its own file I/O** (imageseq's `make_imageseq_content` uses
raw `std::filesystem`). That would bypass `LoadContext` entirely, duplicating URI resolution,
losing resolved-identity dedup, losing the unavailable path, and contradicting doc 08:33-35's
"one resolution seam serves both." imageseq gets away with it only because it has no codec at
all; that is a gap, not a precedent.

### Decision 3 — A new `Content::external_asset_ref()` accessor is the URI read-back channel

Add to `src/contract/arbc/contract/content.hpp`, beside `external_composition_ref()` (`:623`):

```cpp
// The authored URI of the external ASSET this content references (doc 08 Principle 3),
// or empty for content that references none. The read-back channel the serialize codec
// uses to re-emit `params.source` VERBATIM AS AUTHORED -- never absolutised.
virtual std::string_view external_asset_ref() const { return {}; }
```

**Rationale.** `external_composition_ref()` is scoped to "the URI a child *composition* was
loaded from" and is consumed by the nested codec; an asset URI is a different thing pointing
at a different kind of target. A `std::string_view` names no json and no serialize type, so
`contract` (L2/L3) stays clean. Null-defaulted, so no existing kind changes. This is a doc 03
delta.

**Rejected — reuse `external_composition_ref()`.** It would conflate "I embed a child
composition" with "I reference a decoded asset", and `codec_nested.cpp:114-116` already treats
a body carrying *both* a `composition` and a `params.ref` as `MalformedField` — overloading
the accessor would make that check incoherent.

**Rejected — have the codec cache the authored URI in a side table.** The content already
knows its URI; a side table keyed by content pointer is a lifetime hazard for no gain.

### Decision 4 — An immutable mip pyramid, not raster's tile table

The decoded master is level 0 of a plain `std::vector<LevelSurface>`; each subsequent level is
a half-band decimation of the one above via `media::decimate_half_band`. Plugin-owned
contiguous CPU memory (the `FrameSurface` shape). Built once at construction. **No CoW, no
`StateHandle`, no `ChunkSource`, no pool backing, no versioning.**

**Rationale.** *Read-only ⇒ immutable ⇒ none of raster's machinery is needed.* Raster's
`TileTable` earns its complexity by supporting paint: copy-on-write per touched tile,
incremental mip recompute, capture/restore through `Editable`, pool-backed blobs. An
`org.arbc.image` never mutates a pixel, so every one of those costs buys nothing.

It also **avoids a live tripwire**: `model.cpp:699-706` asserts that a persisted
`StateHandle` is inert, and `kinds.raster_workspace_backing` notes that assertion "is armed to
catch exactly this change." A read-only kind that gratuitously acquired a `StateHandle` could
trip it for no reason.

Because the pyramid is immutable after construction, **`render_thread_safe()` returns `true`**
— render is a pure read. That is a real win over imageseq (which returns `false`, forcing
per-content serialization, because its decoder and LRU are stateful): `org.arbc.image` is a
leaf, so it computes on workers under doc 00:203's "worker dispatch is leaf-only" rule.

**Rejected — reuse `kind-raster`'s `TileTable` as the pixel engine.** Superficially attractive
(mips, bounded scale, `achieved_scale` honesty all for free, already claim-proven) and it
would shrink the diff. But it drags the entire `Editable`/`StateHandle`/CoW/pool-backing
surface into a kind whose defining property is that it has none of it, risks the
persistent-state-walk tripwire above, and would make `render_thread_safe()` a question rather
than an obvious `true`. The shared code that *is* worth reusing — the resampling kernels —
already lives in `media`, which is exactly where a plugin may reach.

### Decision 5 — The core fetches the bytes; the plugin decodes them, receiving them through `ContentConfig`

`deserialize_image(params, inputs, composition, ctx)`:

1. Read `params["source"]` as a JSON string (lenient, like `codec_nested.cpp:107-122`: a
   non-string `source` is treated as absent so it survives the residual round-trip).
2. `ctx.resolve(source)` → `ResolvedRef` (dedup by resolved identity, doc 08:116-122).
3. `ctx.load_asset(ref, on_ready)` → the encoded bytes. **Empty bytes == unavailable.**
4. Look up the plugin's factory: `registry.factory("org.arbc.image")`.
5. Invoke it with a `ContentConfig` framing **the authored URI and the encoded bytes**:
   `"<authored-uri>\n<encoded-bytes>"`, split at the **first** `\n`. Empty bytes after the
   `\n` ⇒ the plugin constructs in the **unavailable** state (URI kept, no pixels).

The plugin decodes, converts to working space, builds the pyramid, and never touches the
filesystem.

**Rationale.** This is the clean split the docs already imply: **core owns URI resolution and
the byte fetch** (one `LoadContext` seam, doc 08:33-35), **the plugin owns bytes → pixels**
(the decoder, behind the codec line). `ContentConfig` is *explicitly* an opaque, kind-defined
`std::string_view` (`registry.hpp:35`), and the CI dual-build plugins already use ad-hoc
framings over it (`"r,g,b,a"`, `"<w>x<h>"` — `dual_build.md:426-437`), so carrying a
length-delimited URI + bytes needs **no ABI change**. A URI cannot contain a raw newline and
`normalize_uri` is purely lexical, so the framing is unambiguous.

**v1 is synchronous.** `FilesystemAssetSource` fires `on_ready` **inline, before `request`
returns** (`filesystem_asset_source.hpp:27`), so in production the bytes are in hand during
deserialize and no pending state arises. A *deferring* `AssetSource` (a future network
source) would leave `on_ready` unfired; v1 treats that as **unavailable**, which is honest and
lossless (ref preserved, re-saves byte-identically). The true *pending* state — mint now,
install pixels later on the writer thread with a revision bump and a damage route — is
deferred to a named follow-up, because it needs the plugin content to accept a
pixels-arrive-later install channel that `Content` does not have today.

**Rejected — pass the resolved URI and let the plugin open the file.** See Decision 2's third
rejection: it bypasses the one resolution seam.

**Rejected — widen `ContentFactory` to take a `LoadContext&`.** That would put a serialize
type in the plugin ABI (`registry.hpp` is `contract`, L2/L3), reintroducing exactly the
layering violation Decision 2 avoids.

### Decision 6 — The vendored decoder is promoted to a shared `plugins/imdec/` target

Move `plugins/imageseq/third_party/imdec.h` + `plugins/imageseq/imdec.cpp` to
`plugins/imdec/`, exposing a STATIC target `arbc-plugin-imdec` (its `third_party` dir PRIVATE).
Both `arbc-plugin-imageseq-impl` and `arbc-plugin-image-impl` link it **PRIVATE**. No `arbc_*`
component, no `arbc` umbrella, and no `arbc-testing` may link it.

**Rationale.** The `.tji` note requires sharing "imageseq's already-vendored stb-class decoder
rather than vendoring a second one", and doc 17:228-229 says the two kinds share "the one
vendored stb-class decoder". A shared vendored dependency is not *imageseq's* — it belongs
beside both consumers.

**Rejected — have `arbc-plugin-image-impl` compile `../imageseq/imdec.cpp` directly.** No
second copy at runtime (separate MODULEs), but it makes `plugins/image` silently depend on
`plugins/imageseq`'s directory layout — a levelization smell that the containment test would
not catch.

### Decision 7 — An unavailable image has empty bounds and renders nothing

An `org.arbc.image` whose asset is missing, unreadable, or undecodable reports **empty
`bounds()`** and renders nothing. It keeps its authored URI verbatim and re-saves
byte-identically.

**Rationale, and the deviation being taken.** Doc 08:130-131 and doc 05:79-80 say an
unavailable reference "renders the placeholder". For `org.arbc.nested` that is coherent: the
embedding content has a known extent to fill. For `org.arbc.image` **there is no extent** —
the intrinsic size is knowable *only by decoding the asset*, and Constraint 4 forbids caching
it in the document (params carry "a URI and nothing more", doc 08:135-137). There is
literally no rectangle over which to draw a placeholder. Inventing one (a fixed 256×256
magenta square) would be a fabricated extent that changes the composition's geometry based on
a *missing* file — strictly worse than drawing nothing.

Empty-bounds is the honest reading: the layer is present, its reference is intact, and it
reappears in full when the file returns. This carries a **doc 08 delta** recording the
refinement of the placeholder rule for asset-referencing leaf kinds with no intrinsic extent.

**Rejected — persist the intrinsic size in `params` so a placeholder can be drawn.** It
directly contradicts doc 08:135-137 ("a URI and nothing more"), and it introduces a field that
can silently disagree with the asset on disk.

## Acceptance criteria

### Conformance suite (mandatory for a content kind — doc 16)

- `tests/image_conformance.t.cpp` runs `arbc::contract_tests(factory)` from
  `<arbc/testing/contract_tests.hpp>`, with `snapshot_sensitive = false` (matching
  `tests/raster_conformance.t.cpp:53-68`). Link order **`arbc-plugin-image-impl`,
  `arbc-testing`, `arbc`, `Catch2::Catch2WithMain`** — `arbc-testing` **before** `arbc`.
- Re-asserts (second `enforces:` tag, **never re-registered**):
  `03-layer-plugin-interface#static-time-invariant`,
  `09-surfaces-and-backends#content-provided-surface-honored`,
  `09-surfaces-and-backends#provided-surface-released-after-consume`,
  `16-sdlc-and-quality#byte-exact-goldens`.

### New claims-register entries (`tests/claims/registry.tsv` + an `enforces:`-tagged test)

| Claim id | What it pins |
|---|---|
| `03-layer-plugin-interface#image-has-no-editable-facet` | `ImageContent::editable()` returns `nullptr`; retouching a photograph requires stacking an `org.arbc.raster` above it. Non-destructive editing is structural (doc 03:261-268). |
| `08-serialization#image-serializes-as-uri-only` | An `org.arbc.image` layer round-trips byte-exact carrying **only** its authored URI; the emitted `params` is exactly `{"source": ...}`; no pixel data enters the document; the **authored** (not absolutised, not resolved) reference is what re-saves (doc 08:124-137). |
| `08-serialization#unavailable-asset-is-not-a-read-error` | A missing / unreadable / undecodable asset, and a `LoadContext` with **no `AssetSource` installed**, all load **successfully**: the content keeps its ref verbatim, reports empty bounds, renders nothing, and re-saves byte-identically (doc 08:126-134, doc 05:77-83). |
| `09-surfaces-and-backends#image-provided-surface-covers-requested-region` | The provided surface's extent equals the requested region at the achieved scale — **never the whole decoded frame** — so a cache insert copies one tile, not one image (doc 09:157-160). Asserted over a fixture materially larger than the tile size, at several rungs and at a non-origin region. |
| `17-internal-components#image-decode-dep-stays-out-of-libarbc` | The decode symbols resolve in `arbc-plugin-image-impl` / the MODULE and **never** in `libarbc` or `arbc-testing` — a symbol byte-scan, mirroring `tests/imageseq_containment.t.cpp`. |
| `03-layer-plugin-interface#image-decodes-once-per-resolved-uri` | **Behavioral counter.** `decodes_issued()` on a plugin-side pyramid cache: N layers whose authored refs resolve to one URI (`bg.png` and `./bg.png`) issue **exactly one** decode; re-rendering an unchanged image issues **zero** further decodes. Never a wall-clock assertion (doc 16:54-62; doc 08:116-122 resolved-identity dedup). |

### Byte-exact goldens (`tests/image_goldens.t.cpp`)

Computed-reference, in-TU (the imageseq precedent — the only checked-in binary data is the
fixture itself). A checked-in fixture under `plugins/image/t/fixtures/` (a small P6 `.ppm`,
decodable by `imdec`), rendered and pinned byte-exactly at:

- **native scale** (achieved == requested, `exact == true`);
- **a downscale rung** (level 1 of the pyramid — pins the `decimate_half_band` path);
- **an upscale, `Exactness::Exact`** (bicubic magnify past native, `achieved == requested`);
- **an upscale, `Exactness::BestEffort`** — `achieved_scale` **clamps at native** and
  `exact == false`, matching `raster_content.cpp:512-514`'s honesty rule.

Tolerances are not used. Byte-exact is the default; doc 16 makes tolerance the justified
exception.

### Round-trip / codec

- A document containing an `org.arbc.image` layer round-trips **byte-exact** through
  save→load→save (re-asserting `08-serialization#document-content-round-trips-byte-exact`).
- **Plugin absent** ⇒ the codec is not registered ⇒ the layer round-trips verbatim as
  `PlaceholderContent`, losing nothing (re-asserts
  `08-serialization#unknown-kind-round-trips-verbatim`).
- An unknown interior field inside `params` survives via the existing `params_residual`
  mechanism (`codec.hpp:99-108`) — the codec author writes nothing for this.

### Coverage & gates

- CI gates **≥90% diff coverage** on changed lines. Tests are part of this task, not a
  follow-up.
- `scripts/gate` must pass: configure, build, ctest, clang-format, `check_levels.py`,
  `check_claims.py`, `check_rt_safety.py`.
- `tj3 project.tjp 2>&1 | grep -iE "error|warning"` must be silent after the `.tji` edit.

### Concurrency

`render_thread_safe() == true` (Decision 4) means the render path runs on worker threads. The
pyramid is immutable after construction, so the render path is a pure read — but the
**pyramid cache** (the `weak_ptr` map keyed by resolved URI, Decision 5 / the
`#image-decodes-once-per-resolved-uri` claim) is touched at construction. Construction happens
on the load/writer thread, never on a worker. Scope a **TSan** driver
(`tests/image_concurrency_stress.t.cpp`, mirroring `tests/imageseq_concurrency_stress.t.cpp`)
asserting: concurrent renders of one `ImageContent` across workers are race-free, and
concurrent *construction* of several contents resolving to one URI issues exactly one decode
with no data race on the cache.

### Deferred follow-ups (closer registers in WBS)

- **`kinds.image_async_pending`** — *2d, M9, depends `!image`.* Give `org.arbc.image` the true
  **pending** state for a deferring `AssetSource` (a network/content-store source that does not
  fire `on_ready` inline). Today such a source resolves to *unavailable* (Decision 5). Mint the
  content immediately, install the decoded pyramid later on the writer thread with a revision
  bump and a damage route naming the image content, so doc 02's *Refine* step replaces the
  empty layer live — reusing `PendingExternalLoads` /
  `ExternalCompositionLoader`'s three-state machine
  (`src/runtime/arbc/runtime/external_composition_loader.hpp:81-95`, where pending vs
  unavailable is decided by *whether `on_ready` fired inside `request()`*, never by empty
  bytes). Requires a pixels-arrive-later install channel on the plugin content. Source:
  this refinement, Decision 5.
- **`kinds.image_master_budget`** — *2d, M9, depends `!image`.* Byte-budgeted eviction of
  decoded pyramids. A 24 MP `rgba32f` master plus mips is ~512 MB resident; a composition
  referencing many distinct photographs will exceed any sane budget. Add an LRU over the
  plugin-side pyramid cache under a configurable byte budget, re-decoding on demand, pinned by
  the existing `decodes_issued()` counter (an evicted-then-re-pulled image issues exactly one
  further decode; a re-pull within budget issues zero). Deliberately **not** in this task: it
  needs the residency counter this task lands as its measuring instrument. Source: this
  refinement, Decision 4.

## Design-doc deltas (ride in the closer's commit — doc 16 same-commit rule)

1. **`docs/design/17-internal-components.md`** ("The codec line", after 17:250) — make explicit
   that the codec line is a **decoder** line, applying the section's own "what is being parsed"
   test (17:246): an out-of-lib kind's **serialize codec** lives in `runtime` (it parses only
   our own JSON and a URI string), while its **decoder** — which parses a third-party format
   over untrusted input — stays in the plugin. Note that the codec is registered only when the
   plugin is loaded, so an absent plugin degrades to `PlaceholderContent` rather than data
   loss. Note the shared `plugins/imdec/` vendored decoder (Decision 6).
2. **`docs/design/03-layer-plugin-interface.md`** (beside `external_composition_ref()`,
   03:118-128) — add `external_asset_ref()` to the `Content` interface (Decision 3).
3. **`docs/design/08-serialization.md`** (Principle 3, at 08:126-134) — record two refinements:
   (a) the core fetches asset bytes through `LoadContext::load_asset` and hands them to the
   kind's `ContentFactory` via the opaque `ContentConfig`; the kind never does file I/O
   (Decision 5); (b) an unavailable **asset** on a leaf kind with **no intrinsic extent**
   renders *empty*, not a placeholder rectangle — there is no extent to draw one over, and
   fabricating one would let a *missing* file change the composition's geometry (Decision 7).
4. **`docs/design/00-overview.md`** — a decision-record bullet: *"The codec line is a decoder
   line. An out-of-lib kind's JSON serialize codec lives in core (it parses only our own
   format), gated on the plugin being loaded; only the decoder ships in the plugin."* This is
   project-shaping: it is the general answer for every future plugin kind's persistence.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-14.

- Plugin artifact `plugins/image/{CMakeLists.txt,image_content.cpp,image_plugin.cpp,arbc/kind_image/image_content.hpp,t/fixtures/photo.ppm}` — `arbc-plugin-image` MODULE + `arbc-plugin-image-impl` STATIC; provided surfaces sized to the requested region at the achieved scale (Decision 1).
- Shared vendored decoder promoted to `plugins/imdec/{CMakeLists.txt,imdec.cpp,third_party/imdec.h}` — `arbc-plugin-imdec` STATIC, linked PRIVATE by both image-impl and imageseq-impl (Decision 6).
- Runtime serialize codec `src/runtime/codec_image.cpp`, registered in `builtin_codecs()` iff the plugin is loaded; no JSON or pixel bytes cross the codec line (Decision 2).
- New `Content::external_asset_ref()` in `src/contract/arbc/contract/content.hpp` — URI read-back channel for the codec (Decision 3).
- `ContentConfig` frame carries authored URI + resolved URI + encoded bytes (`"<authored-uri>\n<resolved-uri>\n<encoded-bytes>"`); the three-part form (vs. Decision 5's two-part sketch) is forced by the `#image-decodes-once-per-resolved-uri` acceptance criterion, which requires keying the pyramid cache on the resolved identity while round-tripping the authored spelling.
- 7 new test files: `tests/image_{conformance,goldens,provided_surface,serialize,content,containment,concurrency_stress}.t.cpp` + `tests/support/image_fixtures.hpp`; 4 byte-exact computed-reference goldens (native / level-1 decimate / Exact magnify / BestEffort clamp-at-native).
- 6 new claims registered in `tests/claims/registry.tsv`: `03-layer-plugin-interface#image-has-no-editable-facet`, `03-layer-plugin-interface#image-decodes-once-per-resolved-uri`, `08-serialization#image-serializes-as-uri-only`, `08-serialization#unavailable-asset-is-not-a-read-error`, `09-surfaces-and-backends#image-provided-surface-covers-requested-region`, `17-internal-components#image-decode-dep-stays-out-of-libarbc`; 4 existing claims re-asserted.
- Design-doc deltas: `docs/design/{00-overview,03-layer-plugin-interface,08-serialization,17-internal-components}.md`.
- Deferred follow-ups registered as WBS leaves: `kinds.image_async_pending` (pending state for deferring AssetSource) and `kinds.image_master_budget` (byte-budgeted pyramid eviction), both M9.
