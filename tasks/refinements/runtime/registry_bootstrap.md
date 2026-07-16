# runtime.registry_bootstrap — L6 umbrella built-in-kind Registry bootstrap

## TaskJuggler entry

Back-link: [`tasks/65-runtime.tji:234-239`](../../65-runtime.tji)

```
task registry_bootstrap "L6 umbrella built-in-kind Registry bootstrap" {
  effort 2d
  allocate team
  depends contract.registry, !plugin_loading, !plugin_operator_registration
  note "Doc 17:61 assigns 'registry bootstrap; built-in kind registration' to
        the L6 umbrella `arbc` target, and nothing does it. [...] Populate a
        Registry with the built-in kinds at the umbrella, so built-ins and
        plugins present one surface. Settle whether the CodecTable/binder
        tables fold into Registry or stay beside it (doc 03 Registry vs doc
        08 codecs) — that choice is the task's design content and may carry
        a doc 03/17 delta. Source: tasks/parking-lot.md 2026-07-09 (L6
        umbrella built-in-kind -> Registry bootstrap). Docs 03/17."
}
```

Milestone edge: `m10_post_01` (M10: post-v0.1) lists this task in its
`depends` (`tasks/99-milestones.tji:72`).

Closer: add `complete 100` after the `allocate team` line, append
`Refinement: tasks/refinements/runtime/registry_bootstrap.md` to the note,
run `tj3 project.tjp 2>&1 | grep -iE "error|warning"` and confirm silence.
If this is the last open M10 dependency, also mark the milestone (milestones
don't infer completion). Note: the `.tji` note's "doc 17:61" citation is
stale — the graph line is doc 17:33 and the table row is doc 17:72; the same
stale citation lives in `src/runtime/arbc/runtime/plugin_host.hpp:37-40`
(fixing that comment is fair game for the implementer since this task
delivers the seam the comment points at).

## Effort estimate

**2 days.** Day 1: the umbrella bootstrap TU (`register_builtin_kinds` +
six factories + metadata), the `arbc_finalize_library` extension to carry a
hand-written public header, and the export annotation. Day 2: the
enforcing test suite (`tests/registry_bootstrap.t.cpp`), two claims-register
rows, the byte-equality no-behavior-change check, and the doc 03/17 deltas
(already drafted at refinement time — see Decisions 1 and 5). Same shape as
`contract.registry` (1d, pure-seam ratification) plus a day for the umbrella
build plumbing and factory semantics.

## Inherited dependencies

**Settled:**

- `contract.registry` — **Done 2026-07-09**
  (`tasks/refinements/contract/registry.md`). `Registry` at
  `src/contract/arbc/contract/registry.hpp` (impl
  `src/contract/registry.cpp`): `add(id, factory, metadata, codec?,
  binder?) -> expected<monostate, RegistryError>` (first-wins,
  `RegistryError{EmptyId, DuplicateId}`), `factory(id)`, `metadata(id)`,
  `size()`, `ids()` (registration order preserved — tested). Populate-then-
  freeze threading: `add` runs single-threaded at startup/plugin-load, all
  later access is const reads, no internal lock (doc 03:268-271). Errors are
  values; kind ids are opaque tokens validated only for non-emptiness and
  uniqueness.
- `runtime.plugin_loading` — **Done 2026-07-09**
  (`tasks/refinements/runtime/plugin_loading.md`). `PluginHost`
  (`src/runtime/arbc/runtime/plugin_host.hpp:120-157`) owns a `Registry` and
  RAII dlopen handles; `arbc_plugin_register` is the sole entry point;
  claims `03-layer-plugin-interface#explicit-host-registration-precedes-scan`
  and `#loader-errors-are-values` pin the precedence and error model this
  task's skip-on-duplicate semantics must compose with. Its refinement
  explicitly declared the built-in-kind → `Registry` bootstrap out of scope
  and pointed it at the L6 umbrella — this task.
- `runtime.plugin_operator_registration` — **Done 2026-07-16**
  (`tasks/refinements/runtime/plugin_operator_registration.md`).
  `Registry::Entry` grew optional `KindCodec` (text-typed, JSON-free) and
  `KindBinder`, atomic with the factory in one `add`; accessors
  `codec(id)`/`binder(id)`; `ids()` is the enumeration surface. Its Decision
  4 kept `bind_operators`'s global-built-in-binders-first order byte-for-byte
  and deferred "precedence once registry_bootstrap puts built-ins into the
  Registry" to this task; its Decision 5 appends registry codecs after
  built-ins with `CodecTable::add` last-wins. Both interactions are resolved
  here by Decision 1 (bootstrap entries carry neither hook, so neither
  precedence question arises).

**Pending — explicitly NOT depended on:**

- `runtime.plugin_codec_asset_context` (2d, M10) — widens `KindCodec` with
  asset-I/O context. Irrelevant here: bootstrap entries carry no codec
  (Decision 1), so this task neither waits for nor constrains that seam.

## What this task is

Give the six in-lib kinds (`org.arbc.solid`, `tone`, `raster`, `fade`,
`crossfade`, `nested`) a presence in `contract::Registry`, from the L6
umbrella target where doc 17:33/72 has always assigned "registry bootstrap;
built-in kind registration". Concretely: a new umbrella translation unit and
public header exposing `arbc::register_builtin_kinds(Registry&)`, which
`add`s each built-in kind's `ContentFactory` and `KindMetadata` (human name
+ persisted kind version) to a host-supplied registry, skipping ids already
present. The design content the task note demands — fold the serialize
`CodecTable` / global operator-binder table into the `Registry`, or leave
them beside it — is settled as **beside** (Decision 1): the bootstrap is
factory-and-metadata only, and the built-ins' codec/binder transport stays
on `runtime`'s existing tables.

## Why it needs to be done

Today a built-in kind and a plugin kind arrive by two unrelated mechanisms:
built-ins reach serialization through the hardcoded `builtin_codecs()`
table (`src/runtime/document_serialize.cpp:166-179`) and attach through the
process-global `register_builtin_operator_binders()` table
(`src/runtime/operator_binding.cpp:51-62`), while plugin kinds arrive
through `Registry::add` via `arbc_plugin_register`. Nothing populates a
`Registry` with built-ins in production (`grep` confirms: only the CI
dual-build modules and test stubs do), so `Registry::ids()` on a
`PluginHost`'s registry enumerates *only* plugins — a host building an
"insert layer" menu cannot ask the library what it can instantiate. Doc
17:33 ("umbrella: symbol surface, version, registry bootstrap") and 17:72
("built-in kind registration") assign this exact job to the L6 `arbc`
target, which today carries only `version.cpp`
(`cmake/ArbcComponent.cmake:187`). Origin: parking-lot entry 2026-07-09
("L6 umbrella built-in-kind → Registry bootstrap has no WBS home"), raised
by the `runtime.plugin_loading` closer and resolved into this WBS leaf by
the 2026-07-16 triage.

## Inputs / context

**Design docs (normative):**

- `docs/design/03-layer-plugin-interface.md` § Registry (251-311): registry
  purpose/metadata/threading (253-271), the optional codec/binder hooks and
  entry atomicity (273-290), and the new bootstrap paragraph landed with
  this refinement (292-311 — factory-and-metadata only, skip-on-duplicate,
  fade/crossfade error-value factories, nested config-constructible);
  § Reference implementations (313-337): the eight kind ids, image/imageseq
  ship out-of-lib (329-337) and are NOT bootstrapped.
- `docs/design/08-serialization.md` Principle 1 (131-150): built-in codecs
  are runtime-registered JSON-typed table entries; plugin codecs are
  text-typed registry entries wrapped by serialize — the two-transport
  design this task's Decision 1 preserves.
- `docs/design/17-internal-components.md`: graph line 33 (L6 umbrella:
  "registry bootstrap"); table rows 64 (`contract` L3 owns `Registry`), 71
  (`runtime` L5), 72 (`arbc` L6, "built-in kind registration", depends on
  "runtime + all" — the direct edge this task needs is already granted);
  § Why object libraries point 1 (142-156, incl. the delta landed with this
  refinement: bootstrap is direct `Registry::add`, never the `extern "C"`
  seam); dual-build block (166-181: the registry factory carries no input
  edges or service handles, 170-176).
- `docs/design/00-overview.md:101`: "the core must not enumerate layer
  kinds" — satisfied, not violated, by this task: the *registry instance* a
  host owns enumerates; no core code path switches on kind ids.

**Source seams:**

- `src/contract/arbc/contract/registry.hpp:114-163` — `Registry` API;
  `ContentFactory` (47-48), `KindMetadata` (34-37), `KindCodec` (64-79),
  `KindBinder` (100-103), `OperatorBindServices` (87-90).
- `cmake/ArbcComponent.cmake:182-282` — `arbc_finalize_library`: umbrella
  target with `version.cpp` as sole source (187), aggregated header
  FILE_SET (268-279), PRIVATE object-library links (280-282). The bootstrap
  TU and its public header extend this function.
- `src/CMakeLists.txt:22` — the `arbc_finalize_library()` call site, after
  all 17 components.
- `src/runtime/arbc/runtime/builtin_codecs.hpp:34-40` — the persisted
  per-kind `k_*_kind_version` constants the bootstrap metadata must reuse.
- `src/kind_*/arbc/kind_*/*_content.hpp` — `kind_id` constants (e.g.
  `SolidContent::kind_id`, `src/kind_solid/arbc/kind_solid/solid_content.hpp:36`)
  and public constructors the factories call.
- `tests/ci_plugins/ci_kinds.hpp` — the CI dual-build's per-kind config
  grammars (`k_solid_config` "r,g,b,a" 196-219, tone "hz,amp" 221-238,
  raster "WxH" 270-288, nested decimal-ObjectId 290-299, fade/crossfade
  window-seconds with module-owned inputs 301-354) — the grammar precedent
  Decision 4 adopts and the input-ownership expedient it rejects.
- `src/runtime/document_serialize.cpp` — `builtin_codecs(const Registry&)`
  (197-220: image gate at 204-206 keyed on `registry.factory(k_image_kind_id)`,
  registry-codec append at 214-218), `LoadAssembly` (461-540: registry
  `KindCodec` sweep at 531-539), `load_document`'s binder-conditional
  `set_registry` (753-758) — the three registry consumers a bootstrapped
  registry must pass through unchanged.
- `src/runtime/operator_binding.cpp:99-168` — `bind_operators`: global
  built-in binder table first (144-150), document-registry `KindBinder`s
  second (151-160).
- `tests/CMakeLists.txt` — `tests/` executables link the umbrella `arbc`
  target (e.g. lines 56-58), so the enforcing test exercises the real
  shipped surface.

**Tests & claims:**

- `tests/claims/registry.tsv` + `scripts/check_claims.py` — claims register.
- Existing claims this task composes with (not re-tagged):
  `03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata`,
  `#explicit-host-registration-precedes-scan`,
  `#registry-carries-optional-codec-and-binder`.
- `tests/document_serialize_golden.t.cpp:192,209` — the golden-fixture
  pattern the byte-equality criterion reuses.
- `tests/ci_plugins/passthrough_ci_plugin.cpp` +
  `tests/plugin_operator_registration.t.cpp:297-425` — the plugin the
  coexistence case loads beside the bootstrap.

**Predecessor refinements:** listed under Inherited dependencies; the
directly load-bearing prior decisions are plugin_operator_registration's
Decisions 4/5 (precedence questions this task's Decision 1 dissolves) and
kinds/dual_build's Decision 3 (operator input edges cannot travel
`ContentConfig` — the fact behind Decision 4's fade/crossfade factories).

## Constraints / requirements

1. **Levelization (doc 17, CI-enforced).** The bootstrap lives at the L6
   umbrella — a new TU compiled into the `arbc` target itself, not a new
   component and not anywhere in L5-. Its includes (kind headers at L4,
   `builtin_codecs.hpp` at L5, `registry.hpp` at L3) are all within L6's
   granted closure ("runtime + all", doc 17:72). No same-level or upward
   edge appears anywhere else: in particular `PluginHost` (L5) must NOT
   call or know about the bootstrap.
2. **Factory-and-metadata only.** No bootstrap entry carries a `KindCodec`
   or `KindBinder` (Decision 1). `codec(id)` and `binder(id)` return
   nullptr for all six built-in ids after bootstrap — pinned by claim.
3. **Zero behavior change to document load/save.** With a bootstrapped
   registry: `builtin_codecs(const Registry&)`'s registry-codec append and
   `LoadAssembly`'s sweep find no codecs; the image gate stays closed
   (image is not bootstrapped); `load_document` does not retain the
   registry on the `Document` (no binders ⇒ `doc.registry() == nullptr`);
   `bind_operators` order is untouched. Serialized bytes are identical to
   the empty-registry baseline.
4. **Skip-on-duplicate, idempotent.** `register_builtin_kinds` skips ids
   already present (first-wins, matching `Registry::add` and
   `#explicit-host-registration-precedes-scan`) and may be called twice
   without error or growth. Signature: `void register_builtin_kinds(Registry&)`.
5. **Errors are values; no path throws.** Factory failures (config parse
   errors, fade/crossfade input-edge refusals) return
   `unexpected<std::string>`, never throw (doc 03:176-183 boundary rule).
6. **No string or version duplication.** Kind ids come from the
   `*Content::kind_id` constants; metadata versions come from the
   `k_*_kind_version` constants (`builtin_codecs.hpp:34-40`) so registry
   metadata equals the persisted `kind_version`.
7. **The `extern "C"` seam is not traveled.** Built-ins enter by direct
   `Registry::add`; no `arbc_plugin_register` symbol is added to `libarbc`
   (doc 17:149-156). `org.arbc.image`/`imageseq` are NOT bootstrapped —
   they remain out-of-lib plugins (doc 03 § Reference implementations).
8. **Threading model unchanged.** Bootstrap runs in the registry's
   populate-then-freeze window; no synchronization is added (doc
   03:268-271). No TSan scope — no concurrency is introduced.
9. **Export surface.** `register_builtin_kinds` is `ARBC_API`-annotated and
   its header rides the umbrella's public FILE_SET, so shared-library
   consumers (pkg-config/CPS, `tests/consumer/`) see it; the
   `arbc_finalize_library` extension must keep
   `VERIFY_INTERFACE_HEADER_SETS` green.

## Acceptance criteria

- New public header (umbrella FILE_SET, e.g. `src/arbc/builtin_kinds.hpp`
  or the path `arbc_finalize_library` grows to accept) declaring
  `namespace arbc { ARBC_API void register_builtin_kinds(Registry&); }`,
  implemented in a new umbrella TU (e.g. `src/builtin_kinds.cpp`) beside
  `version.cpp`.
- **Lands claim** `03-layer-plugin-interface#builtin-kinds-present-through-registry`
  (new row in `tests/claims/registry.tsv`), enforced by
  `tests/registry_bootstrap.t.cpp` (links umbrella `arbc`): after
  `register_builtin_kinds` on an empty registry, `ids()` returns exactly
  the six built-in ids in the documented order; `factory(id)` is non-null
  for all six; solid/tone/raster/nested factories construct content whose
  behavior matches the config (smoke-level: kind-appropriate `bounds()`/
  facet presence); `metadata(id)->version` equals the corresponding
  `k_*_kind_version` constant and `human_name` is non-empty.
- **Lands claim** `17-internal-components#umbrella-bootstrap-is-factory-and-metadata-only`
  (new row), enforced in the same test: `codec(id)` and `binder(id)` return
  nullptr for all six bootstrapped ids.
- Plain assertions (same test, no separate claim): double-bootstrap leaves
  `size() == 6`; a host pre-registration of `org.arbc.solid` with a stub
  factory survives bootstrap (skip-on-duplicate — the stub's factory is
  still the one returned); fade/crossfade factories return an error value
  whose message names the document-deserialize path; bootstrap on a
  `PluginHost::registry()` followed by loading
  `passthrough_ci_plugin` coexists (`size() == 7`, no `DuplicateId`).
- Byte-equality no-behavior-change check: a representative document
  (reuse the `document_serialize_golden.t.cpp` fixture pattern) saved and
  loaded with a bootstrapped registry produces byte-identical output to the
  empty-registry baseline, and `doc.registry() == nullptr` after load
  (binder-conditional retention untouched).
- Design-doc deltas: doc 03 § Registry bootstrap paragraph and doc 17
  § Why object libraries sentence — **already applied by this refinement**
  (Decision 5); they ride the closer's commit per doc 16's same-commit rule.
  No doc 00 decision-record bullet: docs 08/17 already placed the built-in
  codec/binder transport; the delta clarifies rather than reshapes.
- CI green including levelization check, `VERIFY_INTERFACE_HEADER_SETS`,
  and ≥90% diff coverage on changed lines; `tj3` gate silent after the
  closer's `.tji` edits.
- Not applicable, stated per doc 16 taxonomy: conformance suite (no new
  content kind or operator — factories construct existing, already-conformant
  kinds), TSan/stress (no concurrency introduced), behavioral counters and
  wall-clock (no performance promise).
- No deferred WBS leaves — nothing is left behind (see Decisions; the
  only adjacent future work, `runtime.plugin_codec_asset_context`, was
  already registered by plugin_operator_registration).

## Decisions

1. **The CodecTable and the global binder table stay beside the Registry;
   the bootstrap registers factory + metadata only.** This is the design
   choice the task note demands. The constitution already places built-in
   codec transport in `runtime` (doc 08:139-146: "Concrete per-kind codecs
   are registered from a layer that can see both the kind's concrete type
   and the JSON library — `runtime` (L5) for built-in kinds") and built-in
   binder transport in the global table (doc 17:142-145); folding them into
   registry entries is not just unnecessary but infeasible today:
   - the registry's `KindCodec` is text-typed with no context parameters —
     `raster_codec` needs a `RasterTileStore`, `nested_codec` an
     `ExternalCompositionLoader`, `image_codec` asset I/O; even the
     context-widening task (`runtime.plugin_codec_asset_context`) scopes a
     contract-level asset seam, not runtime-level tile/composition services;
   - routing built-ins through `adapt_kind_codec` would add a
     JSON→text→JSON re-parse to every built-in save/load and demote the
     canonical-form ownership doc 08 assigns to `serialize`;
   - the built-in binders' `BindContext{pull, backend, resolve, doc}` is
     strictly richer than the contract-expressible
     `OperatorBindServices{pull, backend}` — `nested` cannot attach through
     a registry `KindBinder` at all (doc 03:286-288 says exactly this);
   - keeping bootstrap entries hook-free dissolves both precedence
     questions plugin_operator_registration deferred here: `bind_operators`'
     global-first order and `CodecTable`'s last-wins append see nothing new.
   *Rejected: fold codecs into registry entries* — blocked on services the
   text seam cannot carry, and a behavioral regression (re-parse, canonical
   form). *Rejected: fold binders* — impossible for `nested`; for
   fade/crossfade it would perturb the byte-for-byte bind order Decision 4
   of plugin_operator_registration pinned. *Rejected: make Registry
   JSON-typed* — violates L3 levelization and doc 17's codec line (the JSON
   library would enter `contract`'s and every plugin's surface).

2. **The bootstrap is a host-called free function at the umbrella:
   `arbc::register_builtin_kinds(Registry&)`.** Doc 17:33/72 assigns the
   job to L6; a free function over a host-supplied registry keeps the
   choice of *which* registry (a bare `Registry`, a `PluginHost`'s) and
   *when* (before or after plugin loading — see Decision 3) with the host,
   consistent with doc 00:101 (the core never enumerates kinds on its own
   behalf; a host's registry instance does). *Rejected: `PluginHost`
   auto-bootstraps its registry* — L5 cannot name an L6 symbol
   (levelization), it would force built-ins on hosts that don't want them,
   and it would break every existing test that assumes a freshly-scanned
   registry contains only plugins (`tests/dual_build.t.cpp` loads built-in
   ids as plugins — bootstrap-by-default would make every load a
   `DuplicateId`). *Rejected: static initializers in kind components* —
   the initialization-order fiasco, and dual_build's Decision 7 doc delta
   explicitly corrected doc 17 away from the "static registrars" fiction.

3. **Skip-on-duplicate, `void` return, idempotent.** Only `DuplicateId` is
   reachable (ids are non-empty constants), and for a bootstrap it is not
   an error but the designed precedence: a host's explicit registration —
   or a deliberately-first-loaded plugin superseding a built-in id — wins,
   exactly as `#explicit-host-registration-precedes-scan` already pins for
   the loader. Skip-and-continue also means a partial overlap cannot strand
   the remaining kinds unregistered. *Rejected: propagate
   `expected<monostate, RegistryError>` and stop at the first duplicate* —
   partial registration with no recovery story, and it would make the
   legitimate plugin-first ordering an error path. *Rejected: return a
   registered-count report* — no consumer; YAGNI (same call
   `contract.registry` made against `Registry<T>` generalization).

4. **Factory semantics per kind: adopt the CI grammar where it is honest,
   refuse where it is not.** `solid` ("r,g,b,a" premultiplied floats),
   `tone` ("hz,amp"), and `nested` (decimal child `ObjectId`; the child
   resolves host-side at attach, so it IS config-constructible) adopt the
   config grammars the dual-build already established
   (`tests/ci_plugins/ci_kinds.hpp:196-299`) — `ContentConfig` is
   kind-defined, and one kind must not grow two grammars. `raster`
   ("WxH") keeps the CI grammar but constructs a **transparent** raster of
   that extent (the production "new paint layer" semantic) rather than the
   CI test gradient. `fade`/`crossfade` factories return an error value:
   their input edges are raw `ContentRef`s fixed at construction and
   cannot travel a config string (dual_build Decision 3, doc 17:170-176);
   the honest production path is document deserialize, and the error
   message says so. Registering them anyway (rather than omitting them)
   keeps enumeration complete — a host menu lists them, `metadata(id)`
   works, and the built-in ids are occupied under bootstrap-first ordering.
   The bootstrap TU defines its own factories; `ci_kinds.hpp` stays
   independent CI glue (its subject is the entry point, its fade/crossfade
   builders own module-local inputs the production factory must not
   replicate — hidden document-invisible content). *Rejected: fade/crossfade
   construct with a default hidden input* — production content silently
   owning inputs the document cannot see or serialize. *Rejected: omit
   fade/crossfade from the bootstrap* — `Registry::add` requires a factory
   anyway, and an absent entry reads as "unknown kind" to a host, which is
   false. *Rejected: share factory code with `ci_kinds.hpp`* — the CI
   header is deliberately out-of-lib (`tests/ci_plugins/ci_kinds.hpp:8-9`:
   "Nothing here ships"), and the shared subset would be three trivial
   parsers; the grammars are pinned by both suites instead.

5. **Design-doc deltas land with this refinement** (doc 16 same-commit,
   riding the closer's commit): `docs/design/03-layer-plugin-interface.md`
   § Registry gains the bootstrap paragraph (built-ins present through the
   same surface; factory-and-metadata only; skip-on-duplicate;
   fade/crossfade error-value factories; direct `Registry::add`, never the
   `extern "C"` seam), and `docs/design/17-internal-components.md` § Why
   object libraries point 1 gains the umbrella-bootstrap sentence. These
   sharpen promises doc 17:33/72 and doc 08 already made; nothing
   project-shaping changed, so no doc 00 decision-record bullet.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- New public header `src/api/arbc/builtin_kinds.hpp` declaring `arbc::register_builtin_kinds(Registry&)`, added to the umbrella FILE_SET in `cmake/ArbcComponent.cmake`.
- New umbrella TU `src/builtin_kinds.cpp` implementing factory-and-metadata-only bootstrap for six built-in kinds (solid, tone, raster, fade, crossfade, nested); fade/crossfade return error-value factories; raster constructs a transparent WxH tile extent.
- New internal header `src/runtime/arbc/runtime/builtin_kind_versions.hpp` splitting `k_*_kind_version`/`k_image_kind_id` constants out of `builtin_codecs.hpp` to avoid dragging `nlohmann/json.hpp` into the umbrella TU; `builtin_codecs.hpp` re-includes it so all existing includers are unchanged.
- Unit test `tests/registry_bootstrap.t.cpp` (9 cases, 144 assertions) enforcing claims `03-layer-plugin-interface#builtin-kinds-present-through-registry` and `17-internal-components#umbrella-bootstrap-is-factory-and-metadata-only`; also covers double-bootstrap idempotence, host pre-registration precedence, fade/crossfade error factories, PluginHost coexistence (size 7), and byte-equality no-behavior-change check with `doc.registry() == nullptr`.
- Two new claim rows added to `tests/claims/registry.tsv`.
- Stale `plugin_host.hpp` doc citations (17:60/61) updated to 17:71 and 17:33/72.
- Design-doc deltas (doc 03 bootstrap paragraph, doc 17 object-libraries sentence) applied in `docs/design/03-layer-plugin-interface.md` and `docs/design/17-internal-components.md`.
