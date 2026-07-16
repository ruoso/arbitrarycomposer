# runtime.plugin_operator_registration — Extend plugin entry-point seam for operator kinds

## TaskJuggler entry

`tasks/65-runtime.tji:221-226`:

```
task plugin_operator_registration "Extend plugin entry-point seam for operator kinds" {
  effort 3d
  allocate team
  depends kinds.dual_build, !plugin_loading, serialize.kind_params
  note "Extend the plugin entry-point seam so an out-of-lib kind can register more
        than a ContentConfig factory: its serialize codec (params + input arity,
        deserialize(params, span<const ContentRef>, LoadContext&) shape) and its
        operator binder (attach/detach), so a third-party operator kind is loadable
        into a Document and not merely constructible. Source-of-debt:
        tasks/refinements/kinds/dual_build.md (Decision 3) — fade/crossfade modules
        make the gap concrete. Docs 03/17."
}
```

Milestone: `m9_release` (`tasks/99-milestones.tji:70-74`) depends on this task
directly, and `runtime.registry_bootstrap` (`tasks/65-runtime.tji:227-231`)
names it as a predecessor. On completion the closer adds `complete 100` after
the `allocate team` line, appends
`Refinement: tasks/refinements/runtime/plugin_operator_registration.md` to the
note, runs `tj3 project.tjp 2>&1 | grep -iE "error|warning"` and confirms
silence. This task is not the last M9 dependency, so no milestone propagation.

## Effort estimate

**3 days**, as budgeted in the WBS:

- ~1d — contract surface: widen `Registry` entries with the optional
  `KindCodec`/`KindBinder` slots and an enumeration accessor; the
  serialize-level text↔JSON codec adapter.
- ~1d — runtime wiring: registry codecs appended in
  `builtin_codecs(const Registry&)` and `LoadAssembly` (with `KindBridge`
  interning + `recording_deserialize`, as for built-ins); `Document` carries
  the `Registry*`; `bind_operators` consults document-registry binders after
  the global built-ins.
- ~1d — proof + docs: the new CI plugin module and its round-trip / bind /
  placeholder / arity tests, goldens, claims rows, template-plugin update,
  and the doc 00/03/08/17 deltas (written alongside this refinement; they
  ride the closer's commit).

## Inherited dependencies

All three predecessors are **settled** (Done):

- `kinds.dual_build` — [tasks/refinements/kinds/dual_build.md] proved the six
  in-lib kinds through the production plugin path and, in its Decision 3,
  named this task's exact charter: the fade/crossfade CI modules can
  *construct* an operator across the boundary (owning their own inputs in a
  module-local `InputOwner`, `tests/ci_plugins/ci_kinds.hpp:114-129`) but
  cannot make one *participate in a serialized document* — no codec, no
  binder can cross the seam (`tests/ci_plugins/fade_ci_plugin.cpp:8-10`).
- `runtime.plugin_loading` — [tasks/refinements/runtime/plugin_loading.md]
  landed `PluginHost` (`src/runtime/arbc/runtime/plugin_host.hpp:120-157`),
  the `dlopen`/`dlsym` path over the single entry point
  (`src/runtime/plugin_host.cpp:46,151-181`), and the errors-as-values +
  opt-in-scan discipline this task inherits unchanged.
- `serialize.kind_params` — [tasks/refinements/serialize/kind_params.md]
  landed the serialize-owned `CodecTable`
  (`src/serialize/arbc/serialize/codec.hpp:65-80`) and the codec shape this
  task adapts to: `SerializeFn(const Content&, SaveContext&) -> params json`
  (`codec.hpp:49-50`) and
  `DeserializeFn(params, span<const ContentRef> inputs, ObjectId composition,
  LoadContext&)` (`src/serialize/arbc/serialize/deserialize.hpp:46-48`).

**Downstream:** `runtime.registry_bootstrap` (built-ins and plugins present
one Registry surface) builds directly on the widened entry;
`runtime.plugin_default_search_paths` and `runtime.plugin_loading_win32` are
orthogonal loader work.

## What this task is

Widen the v1 plugin registration surface so a plugin can register, alongside
its `ContentFactory`:

(a) **A JSON-free serialize codec** (`KindCodec`): the kind's persistent
    `kind_version` plus two hooks — `serialize(const Content&)` returning the
    kind's `params` as JSON-object *text*, and
    `deserialize(std::string_view params_text,
    std::span<const ContentRef> inputs, ObjectId composition)` returning the
    constructed content (arity validated inside, error values only). Both are
    `expected<..., std::string>`, matching `ContentFactory`'s error channel
    (`src/contract/arbc/contract/registry.hpp:40-41`).

(b) **An operator binder** (`KindBinder`): `try_attach(Content&,
    const OperatorBindServices&)` / `detach(Content&) noexcept` function
    pointers, where `OperatorBindServices` is the contract-expressible
    `{PullService& pull; Backend& backend;}` (the `Backend` forward-decl
    precedent: `src/kind_fade/arbc/kind_fade/fade_content.hpp:14`).

(c) **Registry storage + enumeration**: `Registry::Entry`
    (`registry.hpp:74-78`) grows `std::optional<KindCodec>` /
    `std::optional<KindBinder>`, supplied atomically in the same
    `Registry::add` call (a plugin cannot decorate another plugin's kind);
    plus lookup accessors (`codec(id)`, `binder(id)`) and an entry-enumeration
    accessor the codec-table assembly needs (today nothing can iterate a
    `Registry` — the very gap `runtime.registry_bootstrap`'s note complains
    about).

(d) **The serialize adapter**: an `arbc_serialize`-internal wrapper turning a
    `KindCodec` into a JSON-typed `Codec` (`codec.hpp:55-58`) — on save,
    parse the plugin's params text into the `params` node (parse failure is a
    `SerializeError` value; the core owns canonical form); on load, hand
    `deserialize` the canonical dump of the `params` node and pass
    `inputs`/`composition` through.

(e) **Runtime wiring, save/load**: `builtin_codecs(const Registry&)` and its
    widened overloads (`src/runtime/arbc/runtime/document_serialize.hpp:121-135`)
    append wrapped registry codecs *after* the built-ins
    (`CodecTable::add` is last-wins — "a plugin may supersede a built-in",
    `codec.hpp:67-68`); `LoadAssembly`
    (`src/runtime/document_serialize.cpp:449-575`) does the same on the load
    side, wrapping each registry codec in `recording_deserialize`
    (`document_serialize.cpp:422-436`) and interning its kind id in the
    `KindBridge`, exactly as for built-ins.

(f) **Runtime wiring, bind**: `Document` gains a `const Registry*`
    (set by `runtime::load_document`, which already receives the registry —
    `document_serialize.hpp:211-214` — and settable by hosts that assemble
    documents programmatically); `bind_operators`
    (`src/runtime/operator_binding.cpp:97-136`) tries the global built-in
    binders first (unchanged), then the document-registry `KindBinder`s,
    first match wins per content as today (`operator_binding.cpp:124-127`).

(g) **Proof**: a new CI-only plugin module registering a *distinct* kind id
    (`org.arbc.ci.passthrough`) whose concrete operator type is defined
    wholly inside the module — one input, params round-trip, identity render
    of its input, `attach(PullService&, Backend&)` — so the registry-carried
    binder is the *only* binder that can match it, making the bind path a
    discriminating assertion. Plus the template plugin
    (`examples/plugin-template/template_plugin.cpp:17-33`) registering a
    trivial codec so the shipped example demonstrates the widened surface.

**Not this task:**

- No entry-point signature change — the seam stays
  `extern "C" arbc_plugin_register(arbc::Registry&)`
  (`src/contract/arbc/contract/plugin.hpp:20`), Decision 1.
- No `ContentFactory` widening — input edges cross only through the document
  codec path, Decision 6.
- No asset-context codec hooks — `KindCodec` v1 carries no
  `LoadContext`/`SaveContext` access; the image-family codecs stay in
  `runtime` gated on the Registry (`document_serialize.cpp:197-208`);
  deferred to `runtime.plugin_codec_asset_context` (Acceptance criteria).
- No change to how *built-in* kinds register — `builtin_codecs()` and
  `register_builtin_operator_binders()` (`operator_binding.cpp:49-60`) are
  untouched; whether they fold into the Registry is
  `runtime.registry_bootstrap`'s design content, not this task's.
- No ABI versioning — doc 10:92-98's "no plugin ABI version in v1" stands;
  the widened `Registry` is a source-compatible C++ addition inside the v1
  same-toolchain regime.

## Why it needs to be done

Doc 00:39-40 makes third-party layer kinds the project's stated point, and
doc 03's reference table ships three *operator* kinds. Today a third-party
operator kind is constructible but inert: a plugin can populate only
`Registry` (factory + metadata, `registry.hpp:74-78`), while the
`CodecTable` is assembled from a hardcoded runtime list
(`document_serialize.cpp:166-208, 449-516`) and the binder registry from
`register_builtin_operator_binders()`'s hardcoded three
(`operator_binding.cpp:49-60`). So a document using a third-party operator
kind round-trips only as a placeholder and can never attach render services
— "loadable into a Document and not merely constructible" is exactly the M9
promise this closes. `runtime.registry_bootstrap` (one enumerable surface
for built-ins + plugins) is sequenced behind it.

## Inputs / context

### Design docs (normative, doc 16)

- Doc 03:227-234 — the v1 seam: single
  `extern "C" arbc_plugin_register(Registry&)`, C++ in-process,
  same-toolchain; Stage 2's C ABI activates at 1.0 (doc 16:143-148).
- Doc 03:251-270 — § Registry: entries, opaque kind ids, and the
  concurrency contract this task's registration inherits ("populated during
  single-threaded startup or plugin load and read-only for the remainder of
  a session", 03:267-270). **Delta lands here** (Decision 8).
- Doc 08:129-146 — Principle 1: codecs are serialize-owned, keyed by kind
  id; "the plugin's own translation unit for out-of-tree kinds" is the
  charter whose mechanism this task supplies. **Delta lands here.**
- Doc 08:147-153 — Principle 2: absent plugin ⇒ verbatim placeholder; this
  task must preserve that degradation unchanged.
- Doc 08:271-298 — Principle 6: input edges are core-owned; the codec never
  writes `inputs`, only adopts them at deserialize.
- Doc 17:52-72 — levelization: `contract` L3 (Registry, must not name
  JSON), `serialize` L4 (owns the JSON dep), `runtime` L5 (PluginHost,
  binder tables); the CI dependency check enforces the table.
- Doc 17:143-145, 161-171 — built-in kinds reach runtime via explicit
  tables; the Registry factory "carries no input edges or service handles"
  and the *host* injects `PullService`/`Backend` after construction.
- Doc 17:265-286 — § The codec line: the decoder-line invariant (a codec
  parses our JSON + a URI, never an encoded asset byte) and the passage
  asserting codec registration cannot cross the Registry boundary — the
  assertion this task narrows (only *JSON-typed* codecs cannot).
  **Delta lands here.**
- Doc 00:376-387 — the codec-line decision bullet ("the general answer for
  every plugin kind's persistence"). **Delta lands here.**
- Doc 10:47-53, 92-98 — one-line plugin builds, explicit registration
  precedes scan, no ABI version in v1.
- Doc 13:69-71, 91-103 — pulling goes through the core; binding is the
  render driver's obligation, scoped to the frame.

### Source seams

- `src/contract/arbc/contract/plugin.hpp:8-20` — the entire ABI header;
  entry point returns `void` (loader infers `DuplicateId` from registry
  growth, `plugin_host.cpp:173-179`).
- `src/contract/arbc/contract/registry.hpp:19-22, 27-30, 36, 40-41, 52-82`
  — `RegistryError`, `KindMetadata`, `ContentConfig = std::string_view`,
  `ContentFactory`, `Registry`/`Entry`. The header's own comment block
  (`registry.hpp:43-51`) already frames it as "the minimal seam" the entry
  point registers into.
- `src/contract/arbc/contract/content.hpp:212, 608` — `ContentRef` is a raw
  `Content*`; `composition_ref()` returns `ObjectId`, so both types this
  task's `KindCodec` signature needs are already contract-visible.
- `src/serialize/arbc/serialize/codec.hpp:49-80, 125-144` — `SerializeFn`,
  `Codec`, `CodecTable` (add last-wins :67-69, find :73), body routing.
  Internal header (names `nlohmann::json`), stays internal.
- `src/serialize/arbc/serialize/deserialize.hpp:46-48` — the full
  `DeserializeFn` arity the adapter maps onto.
- `src/serialize/codec.cpp:117` — `registry.factory(kind)` as the
  plugin-present witness on placeholders (unchanged).
- `src/runtime/arbc/runtime/builtin_codecs.hpp:34-40, 125-127` — kind
  versions; the gap narrated in-code ("a plugin CANNOT register a codec
  without putting `nlohmann::json` in every plugin's link surface").
- `src/runtime/document_serialize.cpp:166-208` (builtin_codecs overloads,
  image gate :204-206), `:422-436` (recording_deserialize), `:449-575`
  (LoadAssembly), `:670-717` (load_document, settle_external_loads);
  `document_serialize.hpp:121-135, 164-174, 211-214`.
- `src/runtime/arbc/runtime/operator_binding.hpp:40, 58-63, 73-76, 82, 88,
  98-129, 151-152` — `Backend` fwd-decl, `BindContext`, `OperatorBinder`,
  registration, `OperatorBindingScope`, `bind_operators`;
  `operator_binding.cpp:31-34` (global vector), `:38-47` (first-wins),
  `:49-60` (built-in trio), `:97-136` (walk).
- Driver registration sites: `src/runtime/interactive.cpp:69,80,392`;
  `src/runtime/offline_sequence.cpp:63,149,186`;
  `src/runtime/export_monitor.cpp:144-145` — constructors call
  `register_builtin_operator_binders()`; none holds a `Registry`, which is
  why binder lookup rides the `Document` (Decision 4).
- `src/runtime/arbc/runtime/plugin_host.hpp:154-156` — `d_handles` declared
  before `d_registry`: registry (and everything stored in it, including this
  task's codec/binder hooks) dies before any `dlclose`.
- Concrete binder thunks (the shape plugins replicate):
  `src/runtime/codec_fade.cpp:128-145`, `codec_crossfade.cpp:94`,
  `codec_nested.cpp:148`.
- CI modules + gap narration: `tests/ci_plugins/fade_ci_plugin.cpp:8-10,
  18-35`, `tests/ci_plugins/ci_kinds.hpp:104-129`,
  `tests/CMakeLists.txt:1189-1240`, `tests/dual_build.t.cpp`.
- Shipped plugins: `plugins/image/image_plugin.cpp:7-21` ("Registering the
  FACTORY is all a plugin can do"), `plugins/imageseq/imageseq_plugin.cpp:9-14`,
  `examples/plugin-template/template_plugin.cpp:17-33`,
  `tests/consumer/plugin_template_load.cpp`; `src/runtime/codec_image.cpp:15`
  points at this task.

### Tests / claims (existing)

`tests/claims/registry.tsv`:
`03-layer-plugin-interface#plugin-registers-through-extern-c-entry` (:228),
`#registry-resolves-kind-id-to-factory-and-metadata` (:249),
`#loader-errors-are-values` (:280),
`08-serialization#unknown-kind-round-trips-verbatim` (:256),
`#placeholder-renders-input-0-passthrough` (:258),
`#builtin-operator-codecs-round-trip` (:264), `#builtin-operator-codecs-adopt-inputs` (:265),
`#known-kind-params-unknowns-preserved` (:270),
`17-internal-components#in-lib-kinds-dual-build-through-plugin-entry` (:285),
`#imageseq-decode-dep-stays-out-of-libarbc` (:229) /
`#image-decode-dep-stays-out-of-libarbc` (:310) — the link-surface assertion
pattern Decision 7's JSON-free claim follows,
`10-tooling-and-packaging#third-party-plugin-builds-are-one-line` (:337).

### Predecessor / sibling refinements

- `tasks/refinements/kinds/dual_build.md` — Decision 3 (source-of-debt),
  Decision 4 (modules link umbrella `arbc`), the boundary-conformance
  pattern (`arbc::testing::ContentFactory` closing over config + host
  doubles + attach).
- `tasks/refinements/runtime/plugin_loading.md` — loader decisions 1-3
  (host-scoped registry, additive scan, missing-symbol semantics) that this
  task must not disturb.
- `tasks/refinements/serialize/kind_params.md` — Decisions 1-2: codecs are
  serialize-owned and never ride `contract` in JSON-typed form; the
  levelization reasoning this task's text-typed shape is designed around.
- `tasks/refinements/runtime/operator_codecs.md` — arity-validation-first
  idiom (wrong arity is an error value, model unmutated); codec TU owns the
  binder; `params` is codec-owned, `inputs` core-owned.
- `tasks/refinements/runtime/nested_codec.md` — Decision 2
  (`composition_ref()` XOR authored `inputs`, explicitly binding
  out-of-tree kinds); the `OperatorBinder`/`BindContext` widening history.
- `tasks/refinements/packaging/plugin_helper.md` — Constraint 5 (no ABI
  handshake anywhere; registration surface must be reachable through
  installed public headers only) and the shipped-plugin layout the new
  surface must serve.

## Constraints / requirements

1. **No JSON in any plugin's link surface** (doc 17:278-281). `nlohmann` is
   PRIVATE to `arbc_serialize`; `codec.hpp` stays internal. Everything a
   plugin includes for the widened registration is JSON-free contract
   headers. Pinned by the new claims-register link-surface assertion
   (Acceptance criteria) in the pattern of claims :229/:310.
2. **The entry point is unchanged** — same symbol, same signature, `void`
   return (`plugin.hpp:20`). Existing factory-only plugins (template, image,
   imageseq, six CI modules) load unmodified. Claim :228 re-enforced.
3. **Levelization** (doc 17:52-56, CI-enforced): the new contract types name
   only contract-visible types (`Content`, `ContentRef`, `ObjectId`,
   `PullService`, `Backend` by forward-decl); the text↔JSON adapter lives in
   `arbc_serialize`; the wiring in `runtime`. `check_levels.py` stays green;
   no table widening.
4. **Registration is atomic per entry**: codec/binder ride the same
   `Registry::add` call as the factory — no post-hoc decoration of another
   plugin's kind; `DuplicateId`/`EmptyId` semantics unchanged.
5. **Concurrency contract unchanged** (doc 03:267-270): the Registry is
   populated at single-threaded startup / plugin load and read-only
   thereafter; per-frame `bind_operators` and per-save codec lookups read it
   lock-free, exactly as factories are read today. No new locks, no new
   shared mutable state.
6. **Lifetime**: everything registered dies with the host's `Registry`,
   which `PluginHost` destroys before any `dlclose`
   (`plugin_host.hpp:154-156`). A `Document` (and any
   `OperatorBindingScope`) referencing a plugin-populated registry must not
   outlive the `PluginHost` — the same rule that already governs
   plugin-constructed contents. Documented on the new `Document` accessor.
7. **Errors are values everywhere** (doc 03, claim :280): plugin codec hooks
   return `expected<..., std::string>`; adapter parse failures map to
   `SerializeError`/`ReaderError` values; wrong input arity is a
   `ReaderError` with the model unmutated (operator_codecs idiom).
8. **Placeholder degradation preserved** (doc 08 P2): the same document
   without the plugin loaded round-trips verbatim and renders input-0
   passthrough; `params` residual-diff preservation (doc 08 P4) works
   through the adapter's re-serialization diff.
9. **Core owns placement and canonical form**: the plugin's serialize hook
   emits `params` object text only; `kind`, `kind_version`, `inputs`,
   `composition` remain core-written; the writer canonicalizes whatever the
   plugin emits, so goldens stay byte-exact regardless of plugin formatting.
10. Build/test discipline: `-Werror -Wpedantic`, ≥90% diff coverage on
    changed lines, byte-exact goldens (no tolerances), behavioral assertions
    never wall-clock.

## Acceptance criteria

- **New claims** (rows in `tests/claims/registry.tsv`, each with a live
  `// enforces:` tag):
  - `03-layer-plugin-interface#registry-carries-optional-codec-and-binder`
    — `src/contract/t/registry.t.cpp`: an `add` with codec+binder stores and
    resolves them; factory-only `add` leaves both empty; duplicate/empty-id
    semantics unchanged.
  - `08-serialization#plugin-codec-round-trips-through-document` —
    `tests/plugin_operator_registration.t.cpp`: a document containing an
    `org.arbc.ci.passthrough` operator (with a core-owned `inputs` edge to a
    built-in child) saves to a byte-exact inline golden and loads back;
    saving the loaded document reproduces the golden byte-exactly.
  - `03-layer-plugin-interface#plugin-binder-attaches-render-services` —
    same test: `bind_operators` attaches the module-local operator through
    the registry-carried binder (the global registry has no thunk that can
    match its type — a discriminating assertion), and its render is
    byte-exact-equal to rendering its input directly (identity passthrough);
    `OperatorBindingScope` destruction detaches.
  - `17-internal-components#plugin-codec-registration-is-json-free` — the
    new CI module's link surface excludes the JSON library, asserted in the
    pattern of claims :229/:310.
- **Re-enforced claims** (second `// enforces:` tag, no new rows):
  `03-layer-plugin-interface#plugin-registers-through-extern-c-entry`,
  `#registry-resolves-kind-id-to-factory-and-metadata`,
  `08-serialization#unknown-kind-round-trips-verbatim` and
  `#placeholder-renders-input-0-passthrough` (same fixture, plugin absent),
  `#known-kind-params-unknowns-preserved` (hand-authored unknown field
  inside the plugin's `params` survives a load/save through the adapter's
  residual diff).
- **Error-path tests** (same `.t.cpp`): wrong input arity → `ReaderError`
  value, model unmutated (revision 0); plugin serialize hook emitting
  non-JSON text → `SerializeError` value, no throw, document unchanged.
- **Conformance** (doc 16): the boundary-obtained
  `org.arbc.ci.passthrough` factory runs the `arbc-testing` contract
  conformance suite, adapted exactly as `tests/dual_build.t.cpp` adapts the
  in-lib kinds (config + host doubles + attach closure).
- **TSan**: the round-trip + bind test runs in the existing TSan lane
  (nested_codec precedent); no new synchronization introduced — registry
  reads during bind/save are read-only post-load.
- **Template plugin**: `examples/plugin-template` registers a trivial codec
  for `org.example.template`; `tests/consumer/plugin_template_load.cpp`
  asserts the codec resolves through the installed package (re-enforce
  `10-tooling-and-packaging#third-party-plugin-builds-are-one-line`,
  second tag).
- **Docs**: the doc 00/03/08/17 deltas below are in the tree (written with
  this refinement) and ride the closer's commit.
- **Deferred** (closer registers in the WBS, wired into `m10_post_01`):
  - `runtime.plugin_codec_asset_context` — **2d** — Extend the registry
    `KindCodec` seam so a plugin codec can reach asset I/O (`LoadContext`
    resolve/load_asset, `SaveContext` asset sink) through a JSON-free
    contract-level context, then migrate the image-family codecs out of
    `runtime` (`image_codec`, `builtin_codecs.hpp:154-155`; registry gate
    `document_serialize.cpp:197-208`; `codec_image.cpp:15`) into
    `arbc-plugin-image`/`arbc-plugin-imageseq`, deleting the runtime
    hardcoding. `depends runtime.plugin_operator_registration, kinds.image`.
    Source-of-debt: this refinement, Decision 2.

## Decisions

1. **Registration rides the existing entry point by widening `Registry`,
   not the ABI.** The seam stays the single
   `extern "C" arbc_plugin_register(Registry&)`; `Registry::Entry` grows
   optional `KindCodec`/`KindBinder` slots supplied in the same `add` call.
   *Rejected: widening the entry-point signature* (e.g.
   `arbc_plugin_register(PluginRegistrar&)`) — breaks claim :228, doc 03's
   single-symbol seam, and every shipped plugin, and forces an L5 registrar
   type into the L3 ABI header. *Rejected: a second optional
   `extern "C"` symbol* — forks the seam doc 03 declares single and muddies
   the scan's missing-symbol semantics (plugin_loading Decision 3).
   *Rejected: the plugin calling global runtime free functions*
   (`register_operator_binder` is already `ARBC_API`) — loses host-scoped
   isolation, and the global binder registry's try-all scan
   (`operator_binding.cpp:124-127`) would call dangling thunks after a
   `dlclose` while first-wins blocks re-registration after reload.

2. **The plugin-facing codec is text-typed, not JSON-typed.** `KindCodec`
   carries `kind_version`, `serialize(const Content&) ->
   expected<std::string, std::string>` (a JSON-object text for `params`),
   and `deserialize(std::string_view params_text,
   std::span<const ContentRef> inputs, ObjectId composition) ->
   expected<std::unique_ptr<Content>, std::string>`. An
   `arbc_serialize`-internal adapter wraps it into the JSON-typed `Codec`:
   parse on save (core canonicalizes), canonical-dump on load. This defuses
   doc 17:278-281's objection on its own terms — the JSON library never
   enters the plugin link surface — while finally supplying the mechanism
   doc 08 Principle 1 promised ("the plugin's own translation unit for
   out-of-tree kinds"). The plugin parses/emits its own `params` grammar (it
   authored both sides; it may vendor any parser privately). Input arity is
   validated inside `deserialize` (operator_codecs idiom), not declared —
   the `.tji` note's "params + input arity" is the codec's responsibility,
   not a table field. `composition` is passed through for parity with
   `DeserializeFn` (`deserialize.hpp:46-48`) so nested-like plugin kinds
   deserialize without a second widening. No `LoadContext`/`SaveContext` in
   v1: asset-referencing plugin codecs stay runtime-side (the image worked
   example, doc 17:272-275) until `runtime.plugin_codec_asset_context`.
   *Rejected: exposing `nlohmann` to plugins* — doc 17:278-281, explicit.
   *Rejected: a core-owned JSON-DOM abstraction* — a large speculative API
   surface for one consumer. *Rejected: runtime-side codecs for third
   parties* — `runtime` cannot name a third party's concrete type; that
   "answer" only ever worked for in-repo plugins.

3. **The plugin binder is contract-shaped:
   `OperatorBindServices{PullService&, Backend&}`.** `KindBinder` mirrors
   runtime's `OperatorBinder` (plain function pointers, typed-match by
   `dynamic_cast` inside the thunk, `detach` noexcept) but takes the
   contract-expressible services only. `PullService` is contract; `Backend`
   is the L2 seam reached through contract's transitive closure
   (`fade_content.hpp:14` precedent). The L5-only `BindContext` members
   (`ContentResolver`, `DocRoot`) are *not* exposed: their sole consumer is
   the built-in `org.arbc.nested`, whose binder stays in
   `codec_nested.cpp:148` untouched; a third-party nesting kind has no
   consumer today, and widening `OperatorBindServices` later is additive.
   *Rejected: moving `BindContext` down to contract* — drags `ContentResolver`
   /`DocRoot` (L5) below their level. *Rejected: type-erasing the context* —
   defeats the typed-thunk pattern for no consumer.

4. **Plugin binders are consulted through the `Document`'s registry, not
   forwarded into the global binder registry.** `Document` gains a
   `const Registry*` (doc 17:71 already describes `Document` as "arenas +
   model + registry + loaders" — this lands the promised edge), set by
   `runtime::load_document` from the registry it already receives
   (`document_serialize.hpp:211-214`) and settable explicitly by hosts that
   assemble documents programmatically. `bind_operators` tries the global
   built-ins first (drivers' constructor registration unchanged,
   `interactive.cpp:69`), then the document-registry binders. Rationale:
   the drivers hold no `Registry` and threading one through three renderer
   constructors is invasive; lifetime is naturally correct (the registry
   outlives the document and dies before `dlclose`,
   `plugin_host.hpp:154-156`), whereas global forwarding dangles after
   unload (Decision 1). Precedence once `runtime.registry_bootstrap` puts
   built-ins into the Registry is that task's design content; this task's
   global-first order preserves today's built-in behavior byte-for-byte.

5. **Registry codecs are appended after built-ins, wrapped exactly like
   built-ins.** `builtin_codecs(const Registry&)` and `LoadAssembly` append
   the adapter-wrapped registry codecs after the hardcoded list, inheriting
   `CodecTable::add`'s last-wins semantic that the header already reserves
   for plugins (`codec.hpp:67-68`), plus `recording_deserialize` wrapping
   and `KindBridge` interning so plugin kinds stamp sessions identically to
   built-ins. Requires the new `Registry` enumeration accessor (deliverable
   (c)). *Rejected: a second, plugin-only codec table* — kind_params
   Decision 2 already rejected a split table; the routing consults one seam.

6. **`ContentFactory` stays input-free.** The doc-17:165-167 factory shape
   ("no input edges or service handles") is by design: input edges exist
   only in documents (core-owned `inputs`, doc 08 P6) and arrive through
   `deserialize`'s span; a bare factory-constructed operator legitimately
   has no inputs. dual_build Decision 3's module-local `InputOwner` remains
   the bare-factory story; its narration comment updates to point at the
   landed document path. *Rejected: widening `ContentFactory` to carry
   inputs* — already rejected by dual_build Decision 3; nothing in the
   document path needs it.

7. **The CI proof is a new module-local operator kind, not an upgrade of
   the fade/crossfade modules.** `tests/ci_plugins/passthrough_ci_plugin.cpp`
   registers `org.arbc.ci.passthrough` — factory + codec + binder — with the
   operator type defined inside the module, so no global binder thunk can
   match it and no built-in codec can serialize it: every green assertion
   discriminates the new path. The fade/crossfade modules stay factory-only
   (valid: codec/binder are optional), avoiding churn in
   `tests/dual_build.t.cpp`'s 4739 assertions. *Rejected: registering
   codec+binder from the fade module* — `FadeContent` is matched by the
   global built-in binder and superseding the built-in fade codec proves
   only last-wins ordering, not the third-party path.

8. **Design-doc deltas (written with this refinement, doc 16 same-commit
   rule).** Doc 03 § Registry: entries widen to "factories plus metadata,
   plus an optional JSON-free kind codec and operator binder", with the
   text-codec and bind-services shapes. Doc 08 Principle 1: names the
   registered-text-codec mechanism for out-of-tree kinds. Doc 17 § The
   codec line: narrows "a Registry traffics in content factories, not
   codecs" to the JSON-typed prohibition it actually argues, and keeps the
   asset-referencing exception runtime-side. Doc 00 codec-line bullet:
   amends "it lives in `runtime` beside the built-in codecs" to the
   two-branch answer (reference asset kinds runtime-side; third-party
   pure-`params` kinds through their own registration). No new doc 00
   bullet — this amends an existing decision record rather than minting a
   new one.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-16.

- Widened `Registry::Entry` with `std::optional<KindCodec>` / `std::optional<KindBinder>` and lookup/enumeration accessors — `src/contract/arbc/contract/registry.hpp`, `src/contract/registry.cpp`.
- Added `arbc_serialize`-internal text↔JSON adapter (`adapt_kind_codec`) turning a plugin `KindCodec` into a JSON-typed `Codec` — `src/serialize/arbc/serialize/codec.hpp`, `src/serialize/codec.cpp`.
- Wired registry codecs into `builtin_codecs(const Registry&)` (save) and `LoadAssembly` (load), with `KindBridge` interning and `recording_deserialize` wrapping — `src/runtime/arbc/runtime/builtin_codecs.hpp`, `src/runtime/document_serialize.cpp`, `src/runtime/arbc/runtime/document_serialize.hpp`.
- `Document` carries `const Registry*` set by `load_document` when bindercarrying; `bind_operators` consults document-registry `KindBinder`s after global built-ins — `src/runtime/arbc/runtime/document.hpp`, `src/runtime/operator_binding.cpp`, `src/runtime/arbc/runtime/operator_binding.hpp`.
- New CI plugin `tests/ci_plugins/passthrough_ci_plugin.cpp` registers `org.arbc.ci.passthrough` (factory + codec + binder) with module-local operator type; new test binary `tests/plugin_operator_registration.t.cpp` (8 cases / 651 assertions): round-trip golden, unknown-params preservation, zero-inputs arity error, `ReaderError`/`SerializeError` paths, conformance suite, bind/detach via registry-only path.
- Four new claims + rows in `tests/claims/registry.tsv`: `03…#registry-carries-optional-codec-and-binder`, `08…#plugin-codec-round-trips-through-document`, `03…#plugin-binder-attaches-render-services`, `17…#plugin-codec-registration-is-json-free`; re-enforced six existing claim tags.
- Template plugin and consumer updated — `examples/plugin-template/template_plugin.cpp`, `tests/consumer/plugin_template_load.cpp`.
- Design-doc deltas in `docs/design/00-overview.md`, `03-layer-plugin-interface.md`, `08-serialization.md`, `17-internal-components.md`.
- Fixer: retained `Registry*` on `Document` only when registry carries at least one `KindBinder` — eliminates dangling-pointer UB with stack-scoped factory-only registries (`src/runtime/document_serialize.cpp`, `src/runtime/arbc/runtime/document.hpp`).
