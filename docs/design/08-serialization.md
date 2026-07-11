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

```jsonc
{
  "arbc": { "format": 1 },                 // format major version
  "composition": {
    "working_space": { "primaries": "srgb", "transfer": "linear",
                       "format": "rgba16f" },      // doc 07; omitted = default
    "canvas": [0, 0, 1920, 1080],                  // optional hint (doc 01)
    "layers": [                                     // bottom to top
      {
        "kind": "org.arbc.raster",                 // registry id (doc 03)
        "kind_version": "1.2",                     // producer's plugin version
        "transform": [1, 0, 0, 1, 100.5, 20],      // a b c d tx ty (doc 04)
        "opacity": 1.0,
        "visible": true,
        "name": "backdrop",                        // authoring metadata
        "params": { "source": "assets/bg.png" }    // kind-owned, opaque to core
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
   kind's `params` never carries it. `compositions` references may form
   cycles (A embeds B embeds A): a Droste scene serializes directly — the
   depth budget that bounds its *rendering* (doc 05) has no bearing on its
   *representation*. The kind-owned `params.ref` URI form remains the
   reference to an **external** project file (Principle 3, doc 05); the
   table is for in-document children. On load, a `composition` id absent
   from the table is a serialization error surfaced as a value, like a
   dangling `$ref`.

## Deliberately not in the format

- **Camera/viewport state** — a viewer concern, not scene data; anchored
  cameras (doc 04) serialize separately if a host wants bookmarks.
- **Cached/rendered pixels** — the format stores *how to render*, never
  results.
- **Animation** — out of v1 scope (doc 00); when it comes, it lands as
  time-varying values inside placement and `params` under a new format
  major, not as a bolt-on track section.

## Dependency note

This is the first place the "minimal vetted deps" policy (doc 10) bites: the
core needs a JSON reader/writer. Candidates evaluated in doc 10; the
requirement here is: order-preserving-optional, exact round-trip of unknown
content, no exceptions across the plugin boundary (errors as values at the
API), and unproblematic vendoring.
