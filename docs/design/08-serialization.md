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
   are preserved-and-ignored (same discipline as unknown kinds). Kind-level
   evolution is the plugin's business via `kind_version`.
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
   budget, and composition cycles (Droste, doc 05) ride the nested kind's
   `params` URI (Principle 3) — neither is a `contents`/`$ref` edge.

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
