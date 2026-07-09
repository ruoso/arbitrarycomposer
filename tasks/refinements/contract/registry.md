# contract.registry — Kind registry

## TaskJuggler entry

Back-link: [`tasks/25-contract.tji:39-43`](../../25-contract.tji) — `task registry`
inside `task contract`.

> note "Reverse-DNS kind ids -> factories + metadata; the persistent identifier
> contract serialization references. Doc 03."

## Effort estimate

**1d** (`tasks/25-contract.tji:40`). This is a completion/hardening pass, not a
fresh landing: the `Registry` class, its `extern "C"` entry point, and a
value-level unit test already shipped early with `kinds.imageseq_plugin`
(commit `d8e8282`, which needed something to register into). Same weight class
as `contract.temporal_fields` (1d) and `contract.snapshot_pins` (1d) — a
contract-surface task that pins already-present behavior with a claim, resolves
one doc/tree divergence with a one-paragraph design-doc delta, and closes the
`Registry`'s open disciplines (id validation, concurrency, metadata scope) as
recorded decisions. Lighter than `contract.async_render` (3d), which changed the
load-bearing `render` signature; heavier than a pure doc edit because it lands a
new claims-register entry and its `enforces:` tag over a strengthened unit test.

## Inherited dependencies

`registry` declares no `depends` of its own, so it inherits the parent
`task contract` edges (`tasks/25-contract.tji:7`):
`depends model.editable_facet, surfaces.capabilities`. Neither bears on the
registry surface directly — the substrate this task actually relies on landed
outside the formal graph.

**Settled (formal `depends`, inherited):**

- `model.editable_facet` — `Editable` facet on `Content`; unrelated to the
  registry beyond sharing the `arbc::contract` component.
- `surfaces.capabilities` — surface capability descriptors; unrelated.

**Sibling substrate (not a formal `depends`, but landed and relied on):**

- `kinds.imageseq_plugin` (`d8e8282`, DONE, `tasks/refinements/kinds/imageseq_plugin.md`)
  landed **this task's actual seam early** — `src/contract/arbc/contract/registry.hpp`,
  `src/contract/registry.cpp`, `src/contract/arbc/contract/plugin.hpp`,
  `src/contract/t/registry.t.cpp`, and the claim
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`
  (`tests/claims/registry.tsv:161`). Its refinement said so explicitly
  (`imageseq_plugin.md:381`: "This task lands the minimal `Registry` +
  `extern "C"` entry-point seam … it does not build the production loader";
  `:395-399` rejected spinning a `contract.registry` task *ahead of* imageseq,
  landing the L3 seam inside the first kind that needs it — "the established
  precedent (`kinds.raster` landed the `Editable` L3 interface the same way)").
  `contract.registry` is the deferred half of that split: it ratifies the seam
  as a first-class contract surface rather than an imageseq side effect.
- `contract.operator_members` (DONE) — establishes that a kind id is a
  *serialization projection* of a live `Content`, not the graph's runtime
  identity (`operator_members.md:289-291`); the registry stores the factory that
  reifies that projection.

**Downstream (this task unblocks / is relied on by):**

- `runtime.plugin_loading` (`tasks/65-runtime.tji:34`, M8) — the production
  host-side loader (explicit host registration API, opt-in `ARBC_PLUGIN_PATH`
  directory scan, error plumbing, concurrent discovery) builds *on top of* this
  seam. Carved out in `registry.hpp:46-50`.
- The serialization stream (`serialize.*`, doc 08) — content records persist as
  `(kind id + params)` and resolve back through the registry's factory; the kind
  id is the stable token those files reference (`03:192-194`, `08:30`).
- Every reference kind (`kinds` stream) and third-party plugin that calls
  `registry.add(kind_id, factory, metadata)`.

## What this task is

Complete the `Registry` as a first-class contract surface: the core's
persistent map from a reverse-DNS **kind identifier** to the **factory** that
constructs a fresh `Content` for that kind plus the **metadata** the kind
advertises (human name, version). The class already exists and works; this task
(a) pins the registry's *own* semantics — id → factory + metadata resolution,
registration-order enumeration, errors-as-values on empty/duplicate ids, and the
promise that a kind id is a stable persistent identifier a serialization format
references — with a dedicated claims-register entry and `enforces:` tag over its
unit test; (b) reconciles the shipped `KindMetadata {human_name, version}` with
doc 03 §Registry's "(human name, version, capability flags)" via a design-doc
delta that defers capability flags to their first consumer; and (c) records the
registry's open disciplines (no structural id validation; populate-then-freeze
single-threaded population) as decisions, keeping the constitution self-consistent.
No new component, no new dependency edge, no signature change.

## Why it needs to be done

The stream-25 charter is "the layer contract, complete — everything the walking
skeleton's sync-only subset deferred." The walking skeleton (via
`kinds.imageseq_plugin`) shipped a *working* registry, but only as much of it as
the imageseq plugin path exercised: the sole claim on it,
`03-layer-plugin-interface#plugin-registers-through-extern-c-entry`
(`registry.tsv:161`), pins the **plugin path** (dlopen → `arbc_plugin_register`
→ factory yields a working `Content`) end-to-end, and its `enforces:` tag lives
in `tests/imageseq_plugin_path.t.cpp:30`. The registry as a *contract surface* —
the persistent-identity guarantee serialization will depend on, and the
value-level API in `src/contract/t/registry.t.cpp` — carries **no `enforces:`
tag and no dedicated claim** today. Until it does, the persistent-identifier
contract doc 03 promises "from day one" (`03:192-194`) is unpinned, and the
serialization stream would inherit an unclaimed dependency. This task closes that
gap and settles the three questions doc 03 §Registry left implicit (metadata
scope, id validation, concurrency) so `runtime.plugin_loading` and `serialize.*`
build on a fully specified seam.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/03-layer-plugin-interface.md:188-194` — **§Registry**, the source
  of truth: "The core keeps a `Registry` mapping kind identifiers (reverse-DNS
  strings, e.g. `org.arbc.raster`) to factories plus metadata (human name,
  version, capability flags). The registry is what a future serialization format
  references, so kind identifiers are part of the persistent contract from day
  one …". **Amended by this task** — a following paragraph (delta below) records
  the v1 metadata scope, the id-validation discipline, and the population
  discipline.
- `03:164-171` — **Stage 1 plugin mechanism**: "A layer kind is a library
  exposing factory functions returning `std::unique_ptr<arbc::Content>` … a
  single `extern "C" arbc_plugin_register(Registry&)` entry point."
- `03:173-176` — **Stage 2**: the future stable C ABI carries "semver-gated
  capability flags"; the natural home of the deferred metadata flags.
- `03:177-180` — errors are values across the boundary, "no exceptions across
  the boundary". The registry honors this: `RegistryError`, not throws.
- `03:196-218` — the reference-kind table (`org.arbc.solid` … `org.arbc.nested`)
  and the note that `org.arbc.imageseq` is the permanent out-of-lib exercise of
  the `extern "C" arbc_plugin_register(Registry&)` path.
- `docs/design/08-serialization.md:30-31,50,58-64` — the persistent-identifier
  contract's consumer: `"kind": "org.arbc.raster"` "// registry id (doc 03)",
  `params` "kind-owned, opaque to core", and Principle 2 "**Unknown kinds
  round-trip losslessly**" — the reason the registry must **not** structurally
  reject an id it doesn't recognize.
- `docs/design/17-internal-components.md:53` — levelization: `arbc::contract` is
  Level 3 and already lists **`Registry`** among its contents; dependency edges
  `base, pool, media, surface, model`. No new edge. `17:41-44` — "A component may
  depend only on strictly lower levels … the CI dependency check validates the
  CMake target graph and the include graph against this table"
  (`scripts/check_levels.py`).

### Source seams (already landed by `kinds.imageseq_plugin`)

- `src/contract/arbc/contract/registry.hpp` — `class Registry`
  (`:51-81`): `add(id, factory, metadata) -> expected<monostate, RegistryError>`
  (`:57-58`), `factory(id) -> const ContentFactory*` (`:61`),
  `metadata(id) -> const KindMetadata*` (`:64`), `size()` (`:67`),
  `ids() -> vector<string_view>` in registration order (`:70`). Supporting
  types: `RegistryError {EmptyId, DuplicateId}` (`:18-21`),
  `KindMetadata {human_name, version}` (`:26-29`, comment `:23-25` defers
  capability flags), `ContentConfig = std::string_view` (`:31-35`, opaque,
  "a serialization format (doc 08) gives it structure later"),
  `ContentFactory = std::function<expected<std::unique_ptr<Content>, std::string>(ContentConfig)>`
  (`:39-40`). The loader carve-out comment (`:46-50`) already names
  `runtime.plugin_loading` (M8) as the owner of production loading.
- `src/contract/registry.cpp:16-26` — `add` rejects empty (`EmptyId`) and
  duplicate (`DuplicateId`) ids as `unexpected<RegistryError>`; success stores an
  owning `std::string` copy of the id in registration order. `find` (`:7-14`) is
  a linear scan.
- `src/contract/arbc/contract/plugin.hpp:20` —
  `extern "C" ARBC_PLUGIN_EXPORT void arbc_plugin_register(arbc::Registry&)`.
- `src/contract/t/registry.t.cpp:27-72` — the value-level unit test
  (`TEST_CASE("Registry maps reverse-DNS ids to factories and metadata")`):
  covers lookup-by-id, registration-order enumeration, and the empty/duplicate
  error branches. **Carries no `enforces:` tag today** — this task adds one.
- `tests/claims/registry.tsv:161` — the existing plugin-path claim
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry` (the
  end-to-end dlopen path; enforced by `tests/imageseq_plugin_path.t.cpp:30`). The
  claims register is TAB-separated `<claim-id>\t<description>`, gated both
  directions by `scripts/check_claims.py` (scans `src/`, `tests/`, `testing/`).

**Predecessor / sibling refinements:**
[`imageseq_plugin.md`](../kinds/imageseq_plugin.md) (landed the seam;
`:381`, `:395-399`), [`operator_members.md`](operator_members.md) (kind id as
serialization projection; `:289-291`),
[`conformance_suite.md`](conformance_suite.md) (each kind wires its own
`arbc::contract_tests(factory)` gate; Decision 2). The audio refinement
[`../audio/device_monitor.md`](../audio/device_monitor.md) (`:131-132`) records
that the registry is content-kind-only today — a scope boundary this task
preserves.

## Constraints / requirements

1. **No new component, no new edge.** All work lands in `arbc::contract` (L3),
   which already lists `Registry` in its doc-17 contents (`17:53`). No CMake
   `DEPENDS` change; `scripts/check_levels.py` must stay green.
2. **Errors are values.** Registration and lookup never throw across the plugin
   boundary (`03:177-180`); the shipped `RegistryError` / `expected` shapes stand.
3. **Persistent-identity guarantee is claim-pinned.** The registry's own
   semantics — deterministic id → factory + metadata resolution, registration
   order preserved by `ids()`, empty/duplicate rejected as values — get a
   dedicated `03-layer-plugin-interface#…` claim and an `enforces:` tag in
   `src/contract/t/registry.t.cpp`. `scripts/check_claims.py` must pass both
   directions.
4. **Claim anchors to doc 03's stem.** Per the stream-wide convention
   (`operator_members.md:368-374`, `audio_conformance.md:326-334`), the new claim
   id is `03-layer-plugin-interface#<slug>` regardless of doc 08 being the
   downstream consumer — the id is a conceptual identifier, not a markdown anchor.
5. **Metadata scope reconciled with doc 03.** Shipped `KindMetadata` is
   `{human_name, version}`; doc 03 §Registry names "human name, version,
   capability flags". The divergence is resolved by a design-doc delta (below),
   not by adding an unused field — this is a *deviation from a design doc's
   stated content*, so doc 16's same-commit amend rule applies and the doc edit
   rides in this task's commit.
6. **Kind ids are opaque persistent tokens.** `add` validates only
   non-emptiness and per-registry uniqueness — no reverse-DNS grammar check.
   Doc 08 Principle 2 (unknown kinds round-trip losslessly, `08:58-64`) forbids
   the registry/loader from rejecting an id it doesn't recognize.
7. **Diff coverage ≥ 90%** on changed lines (doc 16). The strengthened unit test
   and the claim carry it; any code touched (e.g. an assertion added to
   `registry.t.cpp`) stays covered.

## Acceptance criteria

- **New claim + enforcing test.** Register
  `03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata`
  in `tests/claims/registry.tsv` — description (single-line, present tense,
  ASCII): *"The Registry resolves a reverse-DNS kind id to exactly one factory
  and its metadata (human name, version), enumerates registered ids in
  registration order, and rejects an empty or duplicate id as a value
  (RegistryError), never a throw; a kind id is a stable persistent identifier a
  serialization format references."* Tag `src/contract/t/registry.t.cpp` with
  `// enforces: 03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata`,
  strengthening the existing `TEST_CASE` if needed so every clause of the
  description is exercised (lookup, registration-order `ids()`, both error
  branches, metadata round-trip). `scripts/check_claims.py` passes both
  directions.
- **Design-doc delta lands in the same commit.** `03-layer-plugin-interface.md`
  §Registry gains the paragraph deferring capability flags, stating the
  no-structural-validation id discipline, and stating the populate-then-freeze
  population discipline (delta already written by this refinement; the closer
  commits it with the code).
- **Levelization + build green.** `scripts/check_levels.py`, the full build, and
  the test suite pass; `tj3 project.tjp` is silent on error/warning after the
  `.tji` `complete 100` back-link lands.
- **Concurrency: none added.** `add()`/lookup are single-threaded host-side
  setup; reads are `const` on a structure frozen after startup. No TSan/stress
  work is scoped here — stated explicitly so the closer does not scope it. The
  concurrent-discovery story is `runtime.plugin_loading`'s (M8).
- **No new leaf.** Property-based verification that a registered factory yields a
  *conformant* `Content` is each kind's own `arbc::contract_tests(factory)` gate
  (`conformance_suite.md` Decision 2), already owned by the `kinds` stream; the
  end-to-end dlopen registration path is the existing
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry` claim
  (`imageseq_plugin`). Nothing new is deferred to the WBS.

## Decisions

1. **Ratify the shipped seam; do not re-land it.**
   The `Registry` class, `plugin.hpp`, and unit test already exist and are
   correct (`kinds.imageseq_plugin`, `d8e8282`). This task's deliverable is the
   *claim + doc reconciliation* that promotes them from an imageseq side effect
   to a specified contract surface, exactly the completion split
   `imageseq_plugin.md:395-399` anticipated.
   *Rejected — rewrite the registry (e.g. a `std::unordered_map` index, an
   `ObjectId`-keyed store):* the linear `std::vector<Entry>` scan is correct and
   fast enough for the handful of kinds a host registers; registration-order
   `ids()` is a deliberate, tested property (stable enumeration for diagnostics /
   canonical output). Re-architecting a working L3 seam for a micro-optimization
   with no measured need violates "reuse existing seams."

2. **Defer capability flags; keep `KindMetadata {human_name, version}` — design-doc delta.**
   Doc 03 §Registry lists metadata as "(human name, version, capability flags)";
   the shipped descriptor omits flags (`registry.hpp:23-29`: "capability flags
   arrive with the kinds that first need them"). No kind needs them today, and
   doc 03's own Stage-2 section (`:173-176`) already binds capability flags to
   the future "semver-gated" C ABI. So the flags land with their first consumer,
   and `KindMetadata` grows the field then rather than carrying an unused one now.
   *Design-doc delta:* a following paragraph in `03-layer-plugin-interface.md`
   §Registry records this — this **is** a deviation from the doc's stated
   metadata content, so per doc 16 the amendment rides in this task's commit. Not
   doc-00-worthy: it is a v1-scope call, not a project-shaping decision.
   *Rejected — add an empty `capability_flags` field now:* a speculative,
   unconsumed field on a descriptor destined for a C ABI mirror violates "the
   simpler abstraction with one or two call sites today" and would need
   redesigning once a real capability exists.

3. **Kind ids are opaque persistent tokens — validate non-empty + unique only, no reverse-DNS grammar check.**
   `add` rejects empty and duplicate ids; it does **not** parse the id for
   reverse-DNS shape. Reverse-DNS is a collision-avoidance convention, not an
   enforceable invariant, and doc 08 Principle 2 (`08:58-64`) requires unknown /
   future-namespace kinds to round-trip losslessly — a registry or loader that
   rejected "malformed" ids could destroy data from a plugin the host merely
   lacks. The hard invariants are exactly the two the shipped `add` already
   enforces.
   *No design-doc delta beyond the §Registry paragraph:* doc 03 never promised
   structural validation and doc 08's lossless-round-trip contract already
   implies this; the amendment merely makes the discipline explicit so future
   refinements don't add a validator. *Rejected — reject ids not matching a
   `^[a-z0-9]+(\.[a-z0-9]+)+$` grammar:* would break doc 08's lossless
   round-trip and reject legitimate future namespaces the host has never seen.

4. **Population discipline: populate-then-freeze, single-threaded — no internal synchronization.**
   `add()` runs during single-threaded host startup / plugin load; every
   subsequent access is a `const` read of a frozen structure. The `Registry`
   carries no lock and needs none. Concurrent host-side discovery, `dlopen`
   scanning, and the error plumbing that surrounds it are
   `runtime.plugin_loading`'s deliverable (M8, `registry.hpp:46-50`,
   `tasks/65-runtime.tji:34`), which owns whatever synchronization *loading*
   requires — not the registry seam.
   *No TSan scoped here* (see Acceptance criteria). *Rejected — make `Registry`
   internally thread-safe now (mutex on `add`/`find`):* pays a per-lookup cost on
   the render hot path for a concurrency model no current caller exercises;
   premature and misplaced — the concurrency lives one level up in `runtime`.

5. **Registry stays content-kind-only.**
   The map keys content-kind ids to `ContentFactory`. Audio device sinks and any
   other future plugin type are out of scope (`../audio/device_monitor.md:131-132`
   records "v1 has NO device-sink registry"). Generalizing the registry to a
   multi-kind-of-thing registry is unmotivated by any current consumer.
   *Rejected — a generic `Registry<T>`:* no second registrant exists; a template
   with one instantiation is abstraction ahead of need.

6. **`ContentConfig` stays an opaque `string_view`; the deserialize factory is serialization's job.**
   Doc 08's structured `params` and the registry-side
   `deserialize(json, LoadContext&) -> Content*` (`08:53-57`) belong to the
   `serialize.*` stream, built on this seam. This task does not give
   `ContentConfig` structure or add a deserialize entry point.
   *Rejected — anticipate the JSON `deserialize` signature now:* it depends on
   `LoadContext` (an L4 `serialize` type that does not yet exist) and would drag
   an unbuildable dependency into L3.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- `tests/claims/registry.tsv` — registered claim `03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata` with full description (lookup, registration-order enumeration, error branches, metadata round-trip, persistent-identity guarantee).
- `src/contract/t/registry.t.cpp` — added `// enforces: 03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata` tag over the existing `TEST_CASE`; existing test coverage satisfies every clause of the claim description; no new test case needed.
- `docs/design/03-layer-plugin-interface.md` — §Registry delta paragraph landed: capability-flags deferral to Stage 2 / first consumer, no-structural-id-validation discipline, populate-then-freeze single-threaded population discipline.
- No new component, edge, or signature change; `arbc::contract` (L3) dependency graph unchanged.
- `tasks/25-contract.tji` — `complete 100` added to `task registry`; refinement back-link appended to `note`.
