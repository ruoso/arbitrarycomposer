# 06 — Prior Art

Survey date: 2026-07-04. Method: five parallel research angles (scene
graphs, compositor infrastructure, VFX/video frameworks,
deep-zoom/infinite-canvas projects, vector formats + the Rust ecosystem),
~110 sources fetched, load-bearing claims adversarially verified
(all confirmed; corrections noted inline).

Criteria, mapped to this project's concept:

- **(a)** scene = stack/graph of layers composited into a final view
- **(b)** pluggable layer content *kinds* via a plugin interface (raster,
  vector, live 3D, embedded composition)
- **(c)** arbitrary per-layer transform **plus** automatic view-driven
  re-render at zoom-appropriate resolution (the doc-01 pull contract)
- **(d)** recursive composition (a composition usable as a layer)

## 1. Headline

**The exact combination does not exist as an open source library.** Every
criterion exists individually — several twice over — but no project combines
*library-not-app* + (b) + automatic (c) + first-class (d). The three systems
that come closest each fail on a different axis:

- **Piccolo2D** has the right architecture (a, c-affine, d,
  b-by-subclassing) but is a dormant, CPU-rendered Java/C# toolkit.
- **WebRender** is a real embeddable library with nested pipelines and a
  plugin rasterization seam, but has no retained scene model and its
  layer-kind extensibility is a byte-buffer fallback mechanism, not a
  first-class plugin API.
- **Chromium cc** is the reference implementation of automatic
  raster-scale-on-zoom and heterogeneous layer kinds, but is not extractable
  from Chromium.

The specific gap — a **view-driven resolution negotiation contract between
compositor and layer plugins** (compositor tells each layer "you are
currently on screen at scale S, re-render accordingly") combined with
recursive composition — is genuinely unoccupied. The closest anyone gets is
cc's internal raster-scale heuristics (closed system), Qt's
`levelOfDetailFromTransform` (opt-in query, Qt-bound, no recursion), and
OpenFX's `renderScale` (effects, not layers).

Independent corroborating signals that the niche is real: people still
hand-roll this (the antv "infinite canvas tutorial" builds one from
scratch), the W3C Infinite Canvas Community Group is standardizing
*interchange* (OCIF) precisely because no shared runtime exists, and tldraw
has an open issue (#8307) asking for a first-class zoom-LOD abstraction.

## 2. Closest matches, ranked

### Rank 1 — Piccolo2D (+ Pad++/Jazz lineage) — closest architecture, dead ecosystem

Java/C# ZUI scene-graph toolkit from UMD HCIL (Bederson), BSD.
[piccolo2d.org](https://piccolo2d.org/) ·
[patterns](https://piccolo2d.org/learn/patterns.html) ·
[PCamera javadoc](https://piccolo2d.github.io/doc/piccolo2d.java/release-1.3/core/apidocs/edu/umd/cs/piccolo/PCamera.html)

- **Covers:** (a) `PNode`/`PLayer`/`PCamera` scene graph; (c-affine) every
  node has its own affine transform, immediate-mode vector repaint means
  zoom always re-renders at current scale, and **semantic zoom** is a
  documented pattern (read `paintContext.getScale()` in `paint()`); (d)
  **internal cameras** — `PCamera extends PNode`, so a camera viewing
  another layer set can be embedded as a node ("multiple views, overviews,
  and even embedded views"); (b-partial) any content kind via `PNode`
  subclassing.
- **Missing:** no projective transforms (Java2D affine only); CPU rendering;
  no 3D layer kind; subclassing not a plugin registry; effectively dormant
  since ~2013.
- **Takeaway:** this project is essentially "Piccolo2D reborn as a modern
  C++ library with a formal plugin contract." Study its camera/layer/portal
  model and semantic-zoom pattern closely.

### Rank 2 — WebRender (Mozilla/Servo) — closest *living, embeddable* library

GPU 2D rendering engine, Rust, MPL-2.0; used by Firefox, Servo, and
third-party embedders (e.g., the zng UI toolkit maintains published forks:
`zng-webrender-api`).
[github.com/servo/webrender](https://github.com/servo/webrender)

- **Covers:** (a) display-list scenes per pipeline; (d) **first-class**:
  `DisplayListBuilder::push_iframe()` nests one pipeline inside another,
  arbitrarily deep
  ([iframe example](https://github.com/servo/webrender/blob/main/examples/iframe.rs));
  (c) reference frames take 4×4 transforms (affine **and projective**);
  native items draw at device resolution every frame;
  `RasterSpace::Screen/Local(scale)` and raster roots control cached-picture
  resolution; (b-partial) two real plugin seams: **`BlobImageHandler`**
  (embedder-supplied rasterizer turning arbitrary serialized drawing
  commands into per-tile images — how Firefox feeds SVG into WR) and
  external images/textures (embedder-owned GPU content, i.e., live 3D).
- **Missing:** no retained scene-graph API (embedder rebuilds display lists
  and manages what changed); blob re-raster on zoom is driven by embedder
  hints, not automatic; blob contract is a clunky byte-buffer fallback, not
  a layer-kind plugin API; upstream lives in mozilla-central with lagging
  crates.io releases and API churn.

### Rank 3 — Chromium `cc/` — the design reference for (c), not reusable

Layer-tree compositor.
[How cc Works](https://chromium.googlesource.com/chromium/src/+/master/docs/how_cc_works.md)

- **Covers:** (a) `cc::Layer` trees; (b-closed-set) heterogeneous kinds:
  `PictureLayer` (recorded paint), `TextureLayer` (live GPU/3D/canvas),
  `SurfaceLayer` (another compositor's frame stream = (d), how
  out-of-process iframes compose); (c) **the reference implementation**:
  property trees carry arbitrary 4×4 transforms, and `PictureLayer` "is
  responsible for figuring out which scale(s) the content should be rastered
  at," with per-scale `PictureLayerTiling` and raster-scale-change
  heuristics — exactly re-raster-on-zoom.
- **Missing:** not consumable standalone (only in-tree embedders; drags in
  base/viz/gpu/GN build); no plugin registry for new layer kinds.

### Rank 4 — MLT Framework — best plugin architecture + nested projects, wrong rendering model

C media framework (LGPL-2.1) behind Shotcut/Kdenlive.
[framework docs](https://www.mltframework.org/docs/framework/) ·
[MLT XML](https://www.mltframework.org/docs/mltxml/)

- **Covers:** (a) multitrack/tractor compositing via transitions; (b)
  **everything is a runtime-loaded plugin** (producers/filters/transitions/
  consumers — video, raster, inline SVG via `qimage`, titles, frei0r); (d)
  "A MLT XML doc can be specified as a resource, so XML docs can naturally
  encapsulate other XML docs."
- **Missing:** (c) fails — the affine transition has rotate/shear/scale but
  no projective corner-pin; the pipeline rasterizes at profile resolution
  (verified: `qimage_wrapper.cpp` loads SVG at native size then
  `QImage::scaled()` — no re-rasterization at requested scale); time-based
  video-frame model, no view/zoom concept.

### Rank 5 — deck.gl — best modern pluggable-layer API

GPU visualization framework, TypeScript, MIT (OpenJS Foundation).
[Layer API](https://deck.gl/docs/api-reference/core/layer) ·
[OrthographicView](https://deck.gl/docs/api-reference/core/orthographic-view)

- **Covers:** (a) ordered layer array composited per view; (b) **genuinely
  pluggable** — subclass `Layer` (`initializeState/updateState/draw`),
  existing kinds span raster, vector tiles, and live 3D (`ScenegraphLayer`
  renders glTF scenes); (c-mostly) per-layer 4×4 `modelMatrix`, non-geo
  cartesian `OrthographicView` with `minZoom: -Infinity` /
  `maxZoom: Infinity`, GPU re-render per frame at current zoom, `TileLayer`
  for per-zoom data.
- **Missing:** (d) `CompositeLayer.renderLayers()` gives recursive layer
  *generation*, but no "embed an independent Deck composition with its own
  camera as a transformed layer"; float32 precision eventually caps deep
  zoom; data-visualization-shaped API.

### Rank 6 — ThorVG — closest small C++ library

C++14 vector graphics engine, MIT, ~150 KB core; CPU/OpenGL/WebGPU backends;
used by Godot, LVGL, dotLottie.
[github.com/thorvg/thorvg](https://github.com/thorvg/thorvg)

- **Covers:** (a) retained `Canvas`/`Paint` scene graph; (c-mostly)
  full-matrix `Paint::transform()`, vector content re-rasterizes crisply at
  any zoom; (d) `Scene` is a `Paint`, so scenes nest recursively (**note:**
  the method is `Scene::add()` in v1.0; `push()` was the pre-1.0 name);
  mixed content via `Picture` (SVG, Lottie incl. precomps, PNG/JPEG/WebP).
- **Missing:** (b) loaders are compile-time Meson options, no runtime plugin
  API; no live-render-into-layer delegate (a 3D scene would be pushed as
  repeated pixel buffers); rasters don't gain detail on zoom.

### Rank 7 — Qt (two entries, jointly)

- **QGraphicsScene** — strongest single implementation of (c): per-item
  `QTransform` with **true projective** support (m13/m23/m33), and
  `QStyleOptionGraphicsItem::levelOfDetailFromTransform()` hands each custom
  item its current zoom for LOD/semantic-zoom painting. Missing: recursion
  is a hack (`QGraphicsProxyWidget` embedding a view — officially
  discouraged), subclassing not plugins, Qt-Widgets-bound, raster `QPainter`
  path. [docs](https://doc.qt.io/qt-6/qstyleoptiongraphicsitem.html)
- **Qt Quick scene graph** — strongest (b)+(d): custom `QSGNode`s,
  `QSGRenderNode` (raw GPU commands inline — live 3D),
  `QQuickFramebufferObject`, recursive QML composition,
  `ShaderEffectSource`. Missing: cached layers rasterize at fixed
  `textureSize` (no auto re-render on zoom); welded to the QML runtime.
  [docs](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html)

### Rank 8 — Godot SubViewport / Graphite (functionally complete, but engines/apps)

- **Godot:** `SubViewport` + `ViewportTexture` = heterogeneous content (full
  live 3D world as a 2D texture) + recursive nesting; but it's a game engine
  you adopt wholesale, and viewport resolution is manual
  (`size`/`size_2d_override`), not zoom-derived.
  [docs](https://docs.godotengine.org/en/stable/classes/class_subviewport.html)
- **Graphite** (Apache-2.0, Rust): node-graph engine ("Graphene") where
  **layers are nodes**, raster+vector kinds, infinite pan/zoom without
  pixelation, nested subgraphs — conceptually validates everything including
  (b). But it's an editor application; the `node-graph/` crates are
  unpublished and undocumented for embedding (verified: not on crates.io).
  [graphite.art/features](https://graphite.art/features/)

### Honorable mentions / near-misses

| Project | Notable | Fatal gap |
|---|---|---|
| **vello** (Rust, Apache/MIT) | `Scene::append(&child, Option<Affine>)` = recursive scenes; GPU re-render every frame = resolution-independent | Affine-only; no layer-kind plugin trait; renderer not compositor |
| **libopenshot** (LGPL-3) | `Timeline` inherits `ReaderBase` → nested timelines; keyframed shear + declared perspective corners | Compiled-in readers; rasterizes at project resolution |
| **Lottie / rlottie / lottie-web** | Precomps = portable recursive composition model | Fixed layer schema, playback format not compositor |
| **Rive runtime** (MIT C++) | Nested Artboards, pluggable *renderer* backends | Closed editor, fixed content schema |
| **Flutter layer tree** | `pushTransform(Matrix4)` + pluggable-ish leaves (`addPicture`, `addTexture`) | Engine-internal, no standalone embedding, no scene-in-scene |
| **Clutter** (`ClutterContent`) | Cleanest pluggable-content delegate interface found | Dead as a library (Mutter-internal, GPL) |
| **wlr_scene / Smithay / libweston** | Real compositor libraries | Raster client buffers only; integer positions / 90° transforms; no recursion |
| **Motion Canvas / Revideo** (MIT) | Node-tree scene graph, vector re-render, recursive components | No plugin ABI, fixed render resolution, maintenance risk |
| **OpenSeadragon** (BSD) | Pluggable `TileSource`, deep zoom, multi-image placement | Raster pyramids only, no arbitrary transforms, no recursion |
| **OpenFX** | `kOfxImageEffectPropRenderScale` — standardized resolution-independent plugin rendering | Effect plugins, not a layer/scene model |
| **tldraw** | Best shape-kind plugin API (`ShapeUtil`, any JSX incl. 3D) | **Not open source** (v3+ proprietary license, watermark/key); DOM/React-bound |
| **Remotion** | React components = recursive comps; `deviceScaleFactor` re-render on scale | **Not open source** (company license >3 employees) |

Also checked and clearly not matches: Skia/Cairo (substrates: `SkPicture`
nests and replays resolution-independently under projective `SkMatrix`, but
no scene/layer model), PixiJS/Konva/Fabric/EaselJS/paper.js/two.js (caching
is fixed-resolution and manual — e.g., PixiJS `cacheAsTexture({resolution})`,
Konva `cache({pixelRatio})`), MapLibre/OpenLayers/Leaflet
(map-coordinate-bound), excalidraw (fixed element kinds), GStreamer
compositor (xpos/ypos/alpha only, explicitly no rotation),
Natron/Blender/Olive/rnote/lorien (applications),
piet/lyon/femtovg/tiny-skia/iced/egui (no scene model or no pluggable
content), SVG/PDF (formats proving the document model — nested
`<svg>`/`<use>`, form XObjects — but static).

## 3. Does the combination exist?

**No.** Cross-ecosystem, the four criteria decompose as follows:

- **(a)** ubiquitous.
- **(b)** true runtime plugin interfaces for layer *content kinds* are rare:
  MLT producers (video-domain), deck.gl Layer subclasses (viz-domain),
  WebRender blobs + external textures (fallback-shaped), Clutter's
  `ClutterContent` (dead), tldraw `ShapeUtil` (not OSS). Everyone else is
  subclassing-within-a-framework or a closed set.
- **(c)** automatic view-driven re-render at zoom-appropriate resolution
  exists in exactly two production systems — Chromium cc (unextractable) and
  WebRender (partially, embedder-hinted) — plus opt-in query mechanisms (Qt
  LOD, OpenFX renderScale) and always-redraw vector systems (vello,
  Piccolo2D, Graphite) that get it "for free" for vector content only. **No
  library offers it as a contract for arbitrary/heterogeneous layer
  plugins** (the hard cases: raster pyramids, 3D-scene layers, embedded
  compositions).
- **(d)** exists in many forms (WR iframes, MLT nested XML, libopenshot
  Timeline-as-Reader, Lottie precomps, Rive nested artboards, Piccolo
  internal cameras, Godot SubViewports, vello Scene::append) — but almost
  never *together with* (b) or automatic (c).

The intersection of all four in an embeddable, framework-agnostic library is
an unoccupied niche.

## 4. What to study or reuse

**As rendering backends / content-kind plugins (reuse):**

1. **Skia** (C++) as the vector substrate for the vector layer kind —
   resolution-independent replay for free (`SkPicture` +
   `drawPicture(matrix)`, projective matrices included). **ThorVG** is the
   lightweight C++ alternative and brings SVG+Lottie(precomp) loading with
   recursive `Scene` nesting built in. (**vello** is the equivalent if a
   wgpu-class Rust-interop backend ever materializes.)
2. **resvg/usvg**, **rlottie/ThorVG-lottie** as parsing/playback machinery
   inside SVG/Lottie content-kind plugins.
3. **WebRender** would be the whole-compositor shortcut in a Rust codebase
   (nested pipelines + reference frames already implement (d) and the
   transform half of (c)); noted for completeness — with C++ decided
   (doc 10), it serves as a design reference rather than a dependency.

**As design references (study, don't reuse), keyed to our docs:**

4. **Chromium cc's raster-scale machinery** (`how_cc_works.md`:
   PictureLayerTiling, raster scale-change heuristics, pending/active tree
   pipelining) — the definitive answer to "when and at what scale do I
   re-rasterize a layer on zoom," directly transplantable to the pull
   contract; informs doc 02's request planning and doc 04's scale ladder.
5. **Piccolo2D's camera/portal model and semantic-zoom pattern** — the
   cleanest published design for recursive composition (cameras as nodes)
   and scale-aware layer painting; informs doc 01's viewport anchoring and
   doc 05's recursion. Also read the Jazz/Pad++ papers for the ZUI theory,
   and note it as the strongest precedent that scale-aware rendering is a
   workable plugin obligation.
6. **OpenFX's `renderScale` contract** — prior art for specifying
   resolution-independence as a *plugin obligation*; useful specification
   language for doc 03's contract and the eventual C ABI.
7. **MLT's service/plugin factory and nested-XML project model** — prior art
   for doc 03's registry and doc 08's composition-as-resource;
   **libopenshot's `Timeline : ReaderBase`** — the simplest possible
   recursive-composition type design (make "composition" implement the same
   interface as "layer content"), which is exactly doc 05's move.
8. **Qt's `levelOfDetailFromTransform` + projective `QTransform`** — small,
   proven API shapes for LOD queries, and the reference if doc 04's
   projective door ever opens.
9. **deck.gl's Layer lifecycle**
   (`initializeState/updateState/draw/shouldUpdateState` + CompositeLayer) —
   the best modern ergonomics for the plugin authoring experience.

## 5. Differences to preserve

What this design has that none of the above do — i.e., the reason to build
it: the pull contract with resolution in the request as a *universal plugin
obligation* (not a per-kind special case), interactive and offline
disciplines over one scene model, anchored viewports with rebasing for
structurally unlimited zoom depth, and recursion as an ordinary plugin
implemented against public API.

## Key source index

Piccolo2D [about](https://piccolo2d.org/learn/about.html) ·
[patterns](https://piccolo2d.org/learn/patterns.html) |
WebRender [repo](https://github.com/servo/webrender) ·
[blob.md](https://searchfox.org/mozilla-central/source/gfx/wr/webrender/doc/blob.md) ·
[iframe.rs](https://github.com/servo/webrender/blob/main/examples/iframe.rs) |
Chromium [how_cc_works](https://chromium.googlesource.com/chromium/src/+/master/docs/how_cc_works.md) |
MLT [framework](https://www.mltframework.org/docs/framework/) ·
[XML](https://www.mltframework.org/docs/mltxml/) |
deck.gl [Layer](https://deck.gl/docs/api-reference/core/layer) |
ThorVG [repo](https://github.com/thorvg/thorvg) |
vello [Scene](https://docs.rs/vello/latest/vello/struct.Scene.html) ·
[vision](https://github.com/linebender/vello/blob/main/doc/vision.md) |
Qt [LOD](https://doc.qt.io/qt-6/qstyleoptiongraphicsitem.html) ·
[scene graph](https://doc.qt.io/qt-6/qtquick-visualcanvas-scenegraph.html) |
libopenshot [Timeline](https://openshot.org/files/libopenshot/classopenshot_1_1Timeline.html) |
Graphite [features](https://graphite.art/features/) ·
[Graphene](https://graphite.art/volunteer/guide/graphene/) |
Godot [SubViewport](https://docs.godotengine.org/en/stable/classes/class_subviewport.html) |
OpenFX [docs](https://openfx.readthedocs.io/) |
Lottie [precomps](https://lottiefiles.github.io/lottie-docs/breakdown/precomps/) |
Rive [nested artboards](https://rive.app/docs/editor/fundamentals/nested-artboards) |
tldraw [license](https://tldraw.dev/community/license) (not OSS) |
Remotion [license](https://www.remotion.dev/docs/license) (not OSS)
