# 08 — Serialization

Status: **v1 deliverable** — the format co-evolves with the in-memory model
while both are still soft, and "recursion into an independent compose
project" only fully means something once projects are files.

## Shape

A composition serializes to a JSON document (`.arbc`). JSON because the
documents are small graphs (bulk assets stay external), diff-ability and
hand-inspection matter during the design phase, and every ecosystem that
might embed the library can read it. A binary profile is a later
optimization, gated on evidence.

Distinct from this: the mmapped per-document *workspace* file (doc 15) — a
same-machine session artifact holding the live arenas for crash recovery
and demand paging. The JSON document is the interchange, archival, and
version-control format; the workspace is the database file to JSON's dump,
and never a substitute for it.

### The asset directory

A document is a **`.arbc` file plus a sibling asset directory**, not a single
container:

```
project.arbc            # the JSON graph: small, diffable, hand-inspectable
project.assets/
  bg.png                # imported encoded images  (org.arbc.image, Principle 3)
  tiles/
    3f/3fa91c….tile     # painted raster tile blobs (org.arbc.raster, Principle 8)
```

Everything inside is addressed by relative URI and resolved through the same
`LoadContext` asset hook that external nested projects use (Principle 3), so
one resolution seam serves both and a project directory stays relocatable.

A directory rather than a single file is the deliberate choice. It is what makes
content-addressed blobs work as *files* — an incremental save writes only the
new tiles and touches nothing else, which a monolithic container cannot do
without rewriting itself. It also keeps the bulk out of the diffable artifact,
which is doc 08's posture from the first line ("bulk assets stay external"), and
it lets a host reuse ordinary filesystem tooling. Bundling the directory into a
branded zip is a plausible later *packaging* convenience for transport; it is
explicitly **not** the storage model, and nothing in the format may come to
depend on single-file-ness.

```jsonc
{
  "arbc": { "format": 1 },                 // format major version
  "composition": {
    "working_space": { "primaries": "srgb", "transfer": "linear",
                       "format": "rgba16f" },      // doc 07; omitted = default
    "canvas": [0, 0, 1920, 1080],                  // optional hint (doc 01)
    "layers": [                                     // bottom to top
      {
        "kind": "org.arbc.image",                  // registry id (doc 03)
        "kind_version": "1.2",                     // producer's plugin version
        "transform": [1, 0, 0, 1, 100.5, 20],      // a b c d tx ty (doc 04)
        "opacity": 1.0,
        "visible": true,
        "name": "backdrop",                        // authoring metadata
        "params": { "source": "assets/bg.png" }    // kind-owned, opaque to core
      },
      {
        "kind": "org.arbc.raster",                 // PAINTED pixels: document state,
        "name": "retouch",                         // not an import (Principle 8)
        "params": { "tiles": "assets/tiles/",      // content-addressed blob store
                    "edge": 256, "format": "rgba16f",
                    "levels": [["3fa91c…", "0091ab…", …]] }
      },
      {
        "kind": "org.arbc.nested",
        "transform": [0.001, 0, 0, 0.001, 400, 300],
        "params": { "ref": "widgets/gauge.arbc" }  // external project (doc 05)
      }
    ]
  }
}
```

The example shows the common fields; the core also serializes a layer's
temporal and audio placement — `span` (its parent-time extent, as an
integer-flick `[start, end]` pair; doc 11), `time_map` (the parent→content-local
time affine; doc 11), `gain` (the additive-mix audio scalar twinning `opacity`;
doc 12), and `audible` (the audio twin of `visible`; doc 12) — each **omitted
when at its still/identity default** (always-present span, identity time map,
unit gain, audible set), exactly as `working_space` is omitted when default.
These are core-owned placement, not `params`.

## Principles

1. **The core owns placement; kinds own `params`.** The core
   reads/writes everything except `params`, which is handed verbatim to a
   kind's codec. Because the JSON type stays private to `arbc::serialize`
   (doc 17 levelization: the `Content` interface lives in `contract`, which
   must not name the JSON library), the two hooks are **serialize-owned
   codecs keyed by kind id**, not JSON-typed methods on the `Content`
   interface: a *serialize* codec that turns a live `Content` into its
   `params` JSON, and a `deserialize(json, LoadContext&) -> Content*` codec
   that turns a `params` object back into a `Content`. Concrete per-kind
   codecs are registered from a layer that can see both the kind's concrete
   type and the JSON library — `runtime` (L5) for built-in kinds, the
   plugin's own translation unit for out-of-tree kinds — so the codec table
   is a serialize-owned seam the routing consults; a kind with no registered
   codec round-trips as a placeholder (Principle 2). `LoadContext` supplies
   base-URI resolution and async asset loading so kinds don't invent their
   own.
2. **Unknown kinds round-trip losslessly.** A file using a plugin the host
   doesn't have loads as a *placeholder content* that preserves the original
   `kind`, `kind_version`, and `params` verbatim, renders as a diagnostic
   placeholder, and re-serializes byte-equivalent (modulo formatting). A
   missing plugin must never destroy data. This also covers version skew:
   a kind that doesn't understand newer `params` may choose placeholder
   behavior over lossy parsing.
3. **References, not embedding.** Assets and nested projects are URIs
   resolved relative to the document. v1 supports relative paths;
   the resolution hook in `LoadContext` is where schemes (http, content
   stores) plug in later. Cross-file sharing (two parents referencing one
   child `.arbc`) deduplicates through the loader by resolved identity, so
   the doc-05 shared-content semantics survive persistence.

   Dedup is keyed on the **resolved** URI; the **authored** reference is what
   round-trips — a document that says `widgets/gauge.arbc` saves back saying
   `widgets/gauge.arbc`, never an absolutised path, so a project directory
   stays relocatable and the output stays byte-stable. A reference that cannot
   be loaded — no asset source installed, a missing or unreadable file, a
   depth-cap overrun — is reported as **unavailable**, not as a read error:
   the embedding content loads with no child, keeps its reference verbatim,
   and renders the placeholder (doc 05). The asymmetry with a dangling
   in-document `composition` id (Principle 7, a read error) is deliberate —
   self-inconsistent bytes are a malformed document, whereas a missing
   external file is a condition of the environment that may resolve later, and
   doc 05 already assigns that state the placeholder.
   **Imported images are references; painted pixels are not.** An *imported*
   image has a file it came from, so it serializes as a URI and nothing more —
   that is `org.arbc.image` (doc 03), which carries the decode dependency and
   therefore lives outside `libarbc`, behind doc 17's codec line. *Painted*
   pixels have no such file: they exist only inside the document, and a
   reference has nothing to point at. They are handled by Principle 8. Keeping
   these two in separate kinds is what makes non-destructive editing structural
   rather than a convention — you retouch by stacking an editable
   `org.arbc.raster` **over** a referenced `org.arbc.image`, and the photograph
   is never copied into the project.
4. **Versioning is boring on purpose.** `arbc.format` is a major version;
   readers reject majors they don't know. Within a major, unknown *fields*
   are preserved-and-ignored (same discipline as unknown kinds) — and that
   holds at **every tier** of the document: envelope, composition, layer,
   content-body siblings, and `params` interiors alike. The reader carries
   unknown sibling fields through verbatim (an opaque stash beside the
   known fields; the JSON type still never crosses out of
   `arbc::serialize`, Principle 1) and the writer re-emits them merged
   into canonical key order (Principle 5); a preserved unknown never
   shadows a known field. "Unknown" means *a key the core does not name* —
   never *a key whose value the core could not use*: a **known** key
   carrying a malformed value stays known, the reader substitutes that
   field's default, and the bad value is not preserved (leniency, not a
   stash). A layer's inline content body shares the layer's JSON object,
   so an unrecognized key there is indistinguishable from an unrecognized
   layer field and is preserved as one, re-emitted at the layer position;
   only a body standing alone in the `contents` table (Principle 6) carries
   unknown *content-body* siblings. The `contents` and `compositions` tables
   are core-owned id-keyed maps, not sibling surfaces: an entry no reference
   reaches is dropped on save — the same canonicalization that renumbers
   hand-authored ids (Principle 6), not data loss. Inside a **known** kind's
   `params` only the codec can say which keys it consumed, so the core
   recovers the remainder by differencing the codec's own re-serialization
   against the input *at load time* (freezing it before any edit, so
   clearing a param never resurrects it); for a kind or `kind_version` this
   build does not know, the placeholder already holds the whole body verbatim
   (Principle 2). Kind-level evolution is otherwise the plugin's business
   via `kind_version`.
5. **Determinism.** Serialization output is canonical (sorted keys, fixed
   number formatting) so documents diff cleanly under version control —
   these files are source artifacts, and VCS-friendliness is a feature.
   Concretely: object keys are emitted in ascending UTF-8 byte order (the
   JSON object's natural `std::map` ordering); numbers use the JSON
   library's platform-independent shortest round-trip decimal serialization
   (locale-independent and deterministic across platforms — never
   `printf`/locale formatting), with integer-valued core scalars (time in
   flicks, canvas extents) carried as JSON integers and fractional
   placement scalars (transform, opacity, gain) as JSON reals; non-finite
   values (NaN, ±Inf) cannot round-trip through JSON and are a
   serialization error surfaced as a value at the API (doc 10's
   errors-as-values), never silently emitted as `null`.
6. **Operator graphs serialize structurally** (doc 13). Input edges are
   core-owned, so they live in an `inputs` array beside `kind`/`params`
   (mirroring `Content::inputs()`), nested inline for chains; shared
   content goes in an optional document-level `contents` table referenced
   by `{"$ref": id}`. Unknown-kind placeholders preserve their inputs and
   may render input 0 as pass-through — a missing fade plugin degrades to
   an unfaded clip, not a hole. Concretely: the `inputs` array is
   **order-significant** (slot *i* is input index *i*, the index
   `identity()`/`map_input_damage` name) and **omitted when empty** (leaf
   content), like the other omit-at-default core-owned fields. A `{"$ref":
   id}` may stand in for the inline body in **any** `inputs` slot *or* a
   layer's content position. A content is *shared* — and hoisted into
   `contents` — when it is referenced **two or more times** across the
   document's reachable graph; singly-referenced content stays inline.
   `contents` ids are **core-owned, non-semantic handles**, not authoring
   tokens: they are re-derived deterministically on every save from graph
   structure (first-encounter order over the canonical layers-then-inputs
   traversal), so output stays byte-stable and a hand-authored file's
   arbitrary ids are normalized on re-serialization — canonicalization
   (Principle 5), not data loss. On load, an id absent from `contents`, or
   a `{"$ref": id}` that closes an operator-input cycle, is a
   serialization error surfaced as a value (Principle 5), never a partial
   load. v1 `$ref` graphs are **acyclic** (the v1 operators `org.arbc.fade`
   / `org.arbc.crossfade` are; doc 13): operator *feedback* cycles
   (doc 13) are a render-time concern bounded by the compositor's depth
   budget, and composition cycles (Droste, doc 05) ride the `compositions`
   table (Principle 7) or an external `params.ref` URI (Principle 3) —
   neither is a `contents`/`$ref` edge.
7. **Child compositions are document-local by default.** A document holds
   exactly one root `composition`; every *other* composition reachable
   from it — the child a nested content (doc 05) embeds — lives in an
   optional document-level `compositions` table, keyed like `contents` by
   core-owned, non-semantic ids re-derived deterministically on every save
   (first-encounter order over the canonical traversal, Principle 6's
   discipline). A nested content body names its child through a core-owned
   `"composition": id` field beside `kind`/`inputs`/`params` — core-owned
   because the reference is graph structure, exactly like `inputs`; the
   kind's `params` never carries it. The core reads that reference off the
   content's `composition_ref()` accessor (doc 03), the exact mirror of
   `inputs()`, and re-derives the emitted id from graph structure — so the
   reference survives even a kind this build cannot load: an unknown-kind
   placeholder (Principle 2) carries its child reference like any other
   content, and a missing plugin never orphans the composition it embeds.
   The id space covers **every** reachable composition, the root included.
   The root is encountered first, so it always holds ordinal `"0"` — but it
   keeps its canonical home in the root `composition` object and is never a
   key in `compositions`, which therefore runs `"1"`…`"N"`. That is what
   lets a cycle close: `compositions` references may form them (A embeds B
   embeds A; a composition embeds *itself*), and the back-edge to the root
   is spelled `"composition": "0"`. A Droste scene serializes directly —
   the depth budget that bounds its *rendering* (doc 05) has no bearing on
   its *representation*. A composition cycle is not an operator-input cycle:
   the `$ref` graph stays acyclic (Principle 6), and the two are checked
   separately. The kind-owned `params.ref` URI form remains the reference to
   an **external** project file (Principle 3, doc 05); the table is for
   in-document children. On load, a `composition` id absent from the table
   is a serialization error surfaced as a value, like a dangling `$ref`; so
   is a `compositions` entry keyed `"0"`, which would claim the root's
   reserved ordinal (rejecting it beats silently dropping a composition the
   author wrote). A table entry no reference reaches is ignored on load and
   dropped on save — canonicalization, like a renumbered id (Principle 6),
   not data loss.

   A nesting content's **input edges are a projection of its child
   composition, not authored data, and are never persisted.** A nesting kind
   reports the child's member layers through `inputs()` so the core can fold
   aggregate revision, route damage, and build the pull identity map
   (doc 13) — but those edges *are* the child's layers, which the
   `compositions` table already carries in full. Persisting them too would
   emit the same contents twice (hoisting them into `contents` behind
   `$ref`s that describe no authored sharing), make a document's bytes
   depend on whether a render binding happened to be attached when it was
   saved, and turn a legal Droste back-edge into an illegal operator-input
   `$ref` cycle (Principle 6) — an unloadable file for the scene this
   principle exists to round-trip. So: a content that answers a non-null,
   resolvable `composition_ref()` emits **no `inputs` array**, and the write
   traversal does not descend its `inputs()`; the child's contents are
   reached through the composition instead. Loading rebuilds the projection
   from the child composition, so the round-trip is whole. The corollary is a
   rule on kinds: **a kind names its child through `composition_ref()` or
   takes authored `inputs`, never both** — a body carrying both `composition`
   and `inputs` is a serialization error surfaced as a value on load
   (rejecting it beats silently dropping one of the two edge sets the author
   wrote).

   That rule is a rule about *document-local* children. A content that answers
   a non-empty `external_composition_ref()` (doc 03) names its child by that
   URI: the core emits **neither** an `inputs` array **nor** a `composition`
   field for it, and the write traversal descends **neither** its `inputs()`
   nor its child composition — the child's contents belong to the other
   document and must not be copied into this one's `contents` table. The
   reference itself rides the kind's `params` (Principle 3), which is the one
   thing the core does not own. A third corollary on kinds follows: a body
   carrying **both** a core-owned `composition` and a kind's external
   `params.ref` names one child two contradictory ways and is a serialization
   error surfaced as a value on load — rejecting it beats silently preferring
   one.

8. **Painted pixels are source, not results — and they persist as a
   content-addressed tile store.** Principle 3 sends every *imported* asset out
   of the document as a URI. Painted pixels cannot follow it: they have no
   source file, and they are not a cached render either (see "Deliberately not
   in the format" below) — they are the irreplaceable state of an editable
   `org.arbc.raster`. They serialize into the document's **asset directory**.

   The design rule is: **persist the tile table, not the image.** In memory the
   raster kind is already a copy-on-write sparse store — a paint copies only the
   tiles it touches, and untouched tiles stay shared by refcount (doc 14).
   Flattening that to a dense pixel buffer on save throws away exactly the
   sparsity and sharing that make it small, and then pays full price for both.
   So the on-disk form mirrors the in-memory one:

   - **Blobs are keyed by content hash.** Each *distinct* tile is written once,
     as `assets/tiles/<hash>`; the layer's `params` hold the per-level arrays of
     hashes. Identical tiles — the empty ones, the flat ones — collapse to a
     single blob, and dedup falls out across layers *and* across undo versions
     for free. It also makes **saves incremental**: a save writes only the tiles
     that are new, because every untouched tile is already on disk under the
     same name.
   - **Mip levels are not persisted.** They are derived, and a rebuild is
     already proven byte-identical to the incremental recompute
     (`14-data-model-and-editing#paint-mip-recompute-matches-full-rebuild`).
     Rebuild on load.
   - **The storage format is document-carried and is not the working format.**
     A document may composite in `rgba32f` and store `rgba16f`; half-float's
     10-bit mantissa is ample for anything that originated as 8-bit sRGB, at
     half the bytes. Whether that trade is acceptable is a lossy/lossless
     judgment only the user can make, so it is authored, not inferred.
     `rgba16f` is the default.
   - **Compression is per blob: zstd with a byte-shuffle.** The shuffle (group
     all byte-0s, then all byte-1s, …) separates a float's noisy low mantissa
     bytes from its structured exponent and sign planes, which is what lifts
     photographic tiles from 1.5x to 2.1x. zstd rather than LZMA: an interactive
     editor saves incrementally and cannot pay LZMA's speed for the ratio.

   *Why this shape, and not "just compress it".* Measured on a 30-layer, 24 MP
   composition (3 full-bleed photos, 4 cropped, 8 painted/retouch, 6 masks,
   5 gradients, 4 flat fills), each lever applied in turn:

   | | size |
   | --- | --- |
   | dense `rgba32f` tiles, mips persisted | 16.11 GB |
   | mips rebuilt on load | 12.08 GB |
   | content-addressed: distinct tiles only | 2.80 GB |
   | stored at `rgba16f` | 1.40 GB |
   | + zstd with byte-shuffle | 0.49 GB |
   | + photos referenced as `org.arbc.image` | **32 MB** |

   **Compression is the weakest lever that matters** — 2.9x, less than dedup's
   4.3x. The reason is worth stating so nobody re-litigates it: compressibility
   is inversely correlated with how much of the file the content actually
   occupies. Flat fills compress ~2400x, but content-addressing has already
   collapsed them to a handful of blobs, so that ratio applies to nothing.
   Meanwhile **photographic tiles are 93% of the post-dedup bytes and compress
   only 2.1x**, because sensor noise is incompressible by construction. No
   compressor rescues a format that stores imported photographs as raster tiles;
   the only lever that does is not storing them — which is precisely what
   `org.arbc.image` is for, and why the two kinds are split.

   *Graceful degradation.* An `org.arbc.image` layer has no `Editable` facet, so
   it cannot be painted on: retouching means stacking a raster layer above it,
   which is the non-destructive practice this shape assumes. A host that instead
   flattens a photograph *into* a raster layer gets what it asked for — those
   tiles are now document state, they are stored, and they compress 2.1x. That
   is a real cost, honestly paid, and it degrades smoothly rather than falling
   off a cliff.

## Deliberately not in the format

- **Camera/viewport state** — a viewer concern, not scene data; anchored
  cameras (doc 04) serialize separately if a host wants bookmarks.
- **Cached/rendered pixels** — the format stores *how to render*, never
  results. This is a rule about *derived* data, and it is not in tension with
  Principle 8: a painted raster's tiles are not a render of anything, they are
  the irreplaceable input the renderer consumes. The test is whether the bytes
  can be recomputed from something else in the document. A composited frame can,
  and is never stored; a mip level can, and is rebuilt on load; a brush stroke
  cannot, and is stored.
- **Animation** — out of v1 scope (doc 00); when it comes, it lands as
  time-varying values inside placement and `params` under a new format
  major, not as a bolt-on track section.

## Dependency note

This is the first place the "minimal vetted deps" policy (doc 10) bites: the
core needs a JSON reader/writer. Candidates evaluated in doc 10; the
requirement here is: order-preserving-optional, exact round-trip of unknown
content, no exceptions across the plugin boundary (errors as values at the
API), and unproblematic vendoring.
