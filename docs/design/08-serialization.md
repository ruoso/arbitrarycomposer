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

## Principles

1. **The core owns placement; kinds own `params`.** The core
   reads/writes everything except `params`, which is handed verbatim to the
   kind's factory (and produced by its serialize hook). The `Content`
   interface (doc 03) grows two members:
   `serialize() -> json` and a registry-side
   `deserialize(json, LoadContext&) -> Content*`. `LoadContext` supplies
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
6. **Operator graphs serialize structurally** (doc 13). Input edges are
   core-owned, so they live in an `inputs` array beside `kind`/`params`
   (mirroring `Content::inputs()`), nested inline for chains; shared
   content goes in an optional document-level `contents` table referenced
   by `{"$ref": id}`. Unknown-kind placeholders preserve their inputs and
   may render input 0 as pass-through — a missing fade plugin degrades to
   an unfaded clip, not a hole.

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
