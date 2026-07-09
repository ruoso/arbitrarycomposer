# serialize.json_dep — JSON dependency decision

## TaskJuggler entry

Back-link: [`tasks/60-serialize.tji:6-10`](../../60-serialize.tji) — `task json_dep`
inside `task serialize`.

> note "Evaluate against doc 08 requirements (round-trip fidelity, no exceptions
> across boundaries, vendoring-free consumption); lean nlohmann. Doc 10."

## Effort estimate

**0.5d** (`tasks/60-serialize.tji:7`). This is the stream's decision-and-wiring
gate, not a code landing: its deliverable is a **settled library choice**, the
one-line design-doc delta that records it (doc 10's dependency table), the
`FetchContent` wiring that makes the library buildable, and a minimal smoke test
that proves the choice actually satisfies doc 08's load-bearing requirements in
the real build. It is the lightest task in the serialize stream by design —
`writer` (2d), `reader` (2d), and `kind_params` (2d) all build on the seam this
task settles. Comparable in shape to a contract-surface ratification pass
(`contract.registry`, 1d) but lighter: no new claim, no strengthened contract
test — a decision, its doc record, and a build-integration probe.

## Inherited dependencies

`json_dep` is the first subtask in the `serialize` chain and declares no
`depends` of its own, so it inherits the parent `task serialize` edges
(`tasks/60-serialize.tji:4`): `depends contract.registry, model.journal`.

**Settled (formal `depends`, inherited):**

- **`contract.registry`** — DONE (2026-07-09,
  [`contract/registry.md`](../contract/registry.md)). Establishes the kind id as
  the stable persistent token the serialization format references
  (`registry.md:66-69`) and the discipline that **unknown kind ids round-trip —
  `add` never rejects an id it doesn't recognize** (`registry.md:277-290`,
  Decision 3). That discipline is the registry-side half of doc 08 Principle 2;
  the JSON library this task picks must be capable of the file-side half
  (verbatim value round-trip of the `params` of a kind the host lacks). The
  registry deliberately leaves `deserialize(json, LoadContext&)` and structured
  `params` to the `serialize.*` stream (`registry.md:313-320`, Decision 6) — this
  chain owns them.
- **`model.journal`** — DONE (2026-07-05,
  [`model/journal.md`](../model/journal.md)). Provides document revisions and the
  pin/version machinery `serialize.writer` will read when it "serializes a pinned
  version" (its note, `tasks/60-serialize.tji:15`). Bears on the *writer*, not on
  the *library choice* this task makes — inherited through the shared parent, not
  a direct constraint on the JSON dependency.

No **pending** inherited dependencies — both predecessors landed.

**Downstream (this task unblocks):**

- `serialize.writer` (`tasks/60-serialize.tji:11-16`, `depends !json_dep`) — the
  canonical writer, and the first component that links the chosen library onto
  `arbc_serialize`. Then `reader` → `kind_params` → {`sharing`, `format_tests`}.
  Every serialize subtask sits on the library this task settles.

## What this task is

Settle the core's single JSON reader/writer dependency and prove the choice
against doc 08's requirements. Concretely: (a) **choose `nlohmann/json`** — the
lean doc 10 already records (`10:26`) — after checking it against doc 08 §Dependency
note's four requirements (`08:96-102`); (b) record the choice as a **design-doc
delta** to doc 10's dependency table, promoting the JSON-library row from
"needed; evaluate…" to a decided entry that also fixes the *consumption mechanism*
(find-first with a version-pinned `FetchContent` fallback, never an in-tree
vendored copy) and reconciles doc 08's "unproblematic vendoring" wording with doc
10's "never vendored" policy; (c) **wire the dependency** into the build via a
`FetchContent_Declare` + `FIND_PACKAGE_ARGS` block mirroring the shipped Catch2
pattern (`CMakeLists.txt:66-83`), with SYSTEM includes so `-Werror -Wpedantic`
does not fire on third-party headers; and (d) land a **minimal smoke test** that
exercises the two capabilities the whole format rests on — verbatim round-trip of
an arbitrary/unknown-kind JSON tree, and a **non-throwing** parse path that an L4
API can surface as `arbc::expected` (errors as values across the boundary). This
task does **not** implement canonical output, `LoadContext`, `$ref` resolution,
or the `src/serialize/` component — those are `writer`/`reader`'s.

## Why it needs to be done

Doc 08 opens: "This is the first place the 'minimal vetted deps' policy (doc 10)
bites: the core needs a JSON reader/writer" (`08:96-102`). Every subtask in the
serialize stream — and therefore milestone M7 — is blocked on which library that
is and how it is consumed, because the answer determines the include a writer
`#include`s, the error-handling shape at the L4 API boundary, and whether an
embedder of `libarbc` inherits a transitive link or system requirement. Doc 10
already leans `nlohmann` and doc 17 already sanctions the edge (the
`arbc::serialize` row lists an explicit external **"JSON dep"**, `17:58`), but the
lean is un-ratified and the *consumption mechanism* is unsettled: doc 08 says
"unproblematic vendoring" (`08:102`) while doc 10 says "never vendored copies
in-tree; consume through `find_package`" (`10:31-34`). This task resolves the
lean into a decision, resolves that tension into one concrete pattern, and proves
the choice compiles-and-links against the requirements so `writer` starts on a
settled, buildable seam instead of re-litigating the library.

## Inputs / context

### Design docs (normative, doc 16)

- `docs/design/08-serialization.md:96-102` — **§Dependency note**, the requirements
  list this task evaluates against, verbatim: *"order-preserving-optional, exact
  round-trip of unknown content, no exceptions across the plugin boundary (errors
  as values at the API), and unproblematic vendoring."*
- `08:58-64` — **Principle 2 (Unknown kinds round-trip losslessly)**: a file using
  a missing plugin "re-serializes byte-equivalent (modulo formatting)"; "A missing
  plugin must never destroy data." The file-side round-trip capability the library
  must provide.
- `08:75-77` — **Principle 5 (Determinism)**: canonical output is "sorted keys,
  fixed number formatting." Relevant because `nlohmann::json`'s default
  `std::map`-backed object *already* sorts keys — canonical key order for free
  (the *number-formatting* half is `writer`'s, not this task's).
- `08:71-74` — **Principle 4 (Versioning)**: within a format major, unknown *fields*
  are preserved-and-ignored — reinforcing verbatim round-trip over strict schemas.
- `docs/design/10-tooling-and-packaging.md:19-34` — **§Dependency policy**: "each
  one is individually justified"; the JSON-library table row (`10:26`, **amended by
  this task**); and the consumption rule "consume through standard find mechanisms
  (`find_package`), never vendored copies in-tree; lockstep versions pinned in CI …
  embedding the core must never transitively impose codecs, GPU SDKs, or a GUI
  toolkit" (`10:31-34`).
- `10:15-17` — "No exceptions across public API and plugin boundaries (doc 03);
  Public errors are values (`expected`-style result — `arbc::expected`)." The
  error-shape the JSON parse boundary must honor.
- `docs/design/17-internal-components.md:58` — levelization: `arbc::serialize` is
  **Level 4**, contents "JSON read/write, canonical form, unknown-kind placeholders,
  `LoadContext`, `$ref` resolution", depends "contract, model (+ below); **JSON
  dep**" — the external library edge is *already* part of the constitution; no new
  component-graph edge is created. `17:41-44` — levelization rule, CI-enforced by
  `scripts/check_levels.py`.

### Source seams (build integration)

- `CMakeLists.txt:66-83` — the shipped **Catch2** `FetchContent_Declare`
  (`GIT_TAG v3.7.1`, `FIND_PACKAGE_ARGS 3`, `FetchContent_MakeAvailable`): the
  find-first-then-fetch template this task mirrors for the JSON dependency.
- `CMakeLists.txt:84-105` — the **Google Benchmark** block: the precedent for
  re-marking a fetched dependency's includes `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES`
  (`98-104`) so `-Werror -Wpedantic` (`CMakeLists.txt:22-27`) does not fire on
  third-party headers.
- `scripts/check_levels.py:29` — `"serialize": {"contract", "model"}` is already in
  the `ALLOWED` table. The external JSON target is **not** an `arbc_*` component
  edge, so it is not validated by this checker — it links directly onto the future
  `arbc_serialize` object target (per `writer`), and this task's smoke test links it
  standalone.
- No `src/serialize/` exists yet, and no JSON code exists anywhere in `src/`/`tests/`
  (the "serialize" hits in `src/runtime/*` are concurrency-serialization, unrelated).
  `src/contract/arbc/contract/registry.hpp:33,43-44` and
  `src/kind_crossfade/arbc/kind_crossfade/crossfade_content.hpp:24-25` mark data
  serialization as deferred to this stream / M7.
- `cmake/ArbcComponent.cmake:53,112` — how test/conformance targets link Catch2
  (`Catch2::Catch2WithMain`); the smoke test follows the same component-test pattern.

**Predecessor / sibling refinements:**
[`contract/registry.md`](../contract/registry.md) (unknown-kind ids round-trip;
`deserialize`/`params` deferred to this stream — Decisions 3, 6),
[`model/journal.md`](../model/journal.md) (revisions/pins the writer reads). This
is the first `serialize` refinement — no serialize siblings yet.

## Constraints / requirements

1. **Ratify doc 10's lean; do not re-open the trade study.** The library is
   `nlohmann/json`. `simdjson`+writer / `yyjson` stay parked for a possible future
   binary/perf profile, "gated on evidence" (`08:12-13`, `10:26`) — not re-evaluated
   here. The decision is recorded as a doc-10 delta (below), same-commit per doc 16.
2. **The four doc-08 requirements are the acceptance gate**, each satisfied by
   `nlohmann/json`: (a) *order-preserving-optional* — the default `json` sorts keys
   (canonical order for free); `ordered_json` is available if a caller ever needs
   insertion order. (b) *exact round-trip of unknown content* — a parsed value tree
   dumps back to an equal tree; unknown/nested `params` survive verbatim. (c) *no
   exceptions across the boundary* — the non-throwing `parse(input, nullptr, false)`
   overload yields a discarded value on malformed input, which the L4 API wraps as
   `arbc::expected`; no `nlohmann` exception escapes core. (d) *unproblematic
   vendoring* — header-only, so it imposes **no transitive link or system
   requirement** on an embedder of `libarbc` (satisfying `10:33-34`).
3. **Consumption mechanism: find-first, pinned `FetchContent` fallback, never
   in-tree vendored.** A `FetchContent_Declare(nlohmann_json …)` at a pinned
   `GIT_TAG` with `FIND_PACKAGE_ARGS`, mirroring Catch2 (`CMakeLists.txt:70-74`).
   Includes marked SYSTEM so `-Werror` stays green on third-party headers. This is
   the concrete reconciliation of doc 08's "unproblematic vendoring" with doc 10's
   "never vendored / `find_package`."
4. **No new component-graph edge; no `src/serialize/` yet.** doc 17 already lists
   the JSON dep on the `arbc::serialize` row (`17:58`) and `check_levels.py:29`
   already permits `serialize → {contract, model}`. This task links the JSON target
   only into a standalone smoke test; creating the `arbc_serialize` component and
   linking the JSON target onto it is `serialize.writer`'s first act.
   `scripts/check_levels.py` must stay green.
5. **Header-only is load-bearing, not incidental.** Because a serialize-component
   dependency folds into the shipped `libarbc`, a compiled JSON library would push a
   transitive link/system requirement onto every embedder — violating doc 10's "must
   never transitively impose" promise (`10:33-34`). `nlohmann/json` being header-only
   is a deciding factor, recorded in the delta, not an accident.
6. **Diff coverage ≥ 90%** (doc 16) on changed lines. The changed *code* is the
   smoke-test TU; it executes fully. CMake wiring and the doc delta are not
   coverage-instrumented (they carry no executable lines).

## Acceptance criteria

- **Decision recorded as a doc-10 delta (same commit).** `docs/design/10-tooling-and-packaging.md`
  §Dependency policy's JSON-library row is amended from "needed; evaluate…" to the
  decided entry: `nlohmann/json`, the four requirements it meets, the parked
  perf-profile alternatives, and the find-first/pinned-`FetchContent`/never-vendored
  consumption mechanism reconciling doc 08's "unproblematic vendoring." (Delta
  already written by this refinement; the closer commits it with the code.)
- **Dependency wired and buildable both ways.** A `FetchContent_Declare(nlohmann_json …)`
  block lands in `CMakeLists.txt` at a pinned `GIT_TAG` with `FIND_PACKAGE_ARGS`
  and SYSTEM includes; a clean configure fetches it, and a configure with a system
  `nlohmann_json` present satisfies it via `find_package` instead. `-Werror` stays
  green (no warnings from `nlohmann` headers).
- **Smoke test green — the requirements are proven, not asserted on paper.** A
  Catch2 test (e.g. `tests/serialize_json_dep_smoke.t.cpp`) links the JSON target
  standalone and exercises: (i) parse → dump round-trip of a JSON tree carrying an
  unknown-kind object with nested/arbitrary `params` yields an **equal** value tree
  (Principle 2, file side); (ii) the **non-throwing** `parse(bad, nullptr, false)`
  overload returns a discarded value on malformed input — no exception escapes —
  demonstrating the `arbc::expected`-wrappable error path (doc 08 req (c),
  `10:15-17`); (iii) a default `json` object serializes its keys in sorted order
  (Principle 5 key-ordering, the half this task can pin cheaply). Byte-canonical
  *number* formatting and full canonical output are **not** asserted here — deferred
  to `serialize.writer`'s goldens.
- **Levelization + build + WBS gate green.** `scripts/check_levels.py`, the full
  build, and the test suite pass; `tj3 project.tjp 2>&1 | grep -iE "error|warning"`
  is silent after the `.tji` `complete 100` back-link lands.
- **No claims-register growth here.** The doc-08 determinism / unknown-kind-round-trip
  claims (byte-canonical goldens, unknown-kind preservation, loader fuzzing) are
  owned by the already-scoped WBS leaves `serialize.format_tests`
  (`tasks/60-serialize.tji:35-40`) and `serialize.kind_params`
  (`:23-28`). This task's smoke test is a build-integration probe carrying diff
  coverage; it takes no `enforces:` tag — stated explicitly so the closer does not
  scope a claim.
- **Nothing new deferred to the WBS.** Every downstream piece (writer, reader,
  kind_params, sharing, format_tests) is already a WBS leaf depending transitively
  on this task; the parked `simdjson`/`yyjson` perf profile is a doc-recorded
  "gated on evidence" option, not an agent-implementable task — no new leaf, no
  parking-lot item.

## Decisions

1. **Choose `nlohmann/json`; park `simdjson`/`yyjson` for a possible future perf
   profile.** It meets all four doc-08 requirements (Constraint 2), its default
   `std::map`-backed object hands canonical key order to `writer` for free, its
   ergonomics/ubiquity lower the cost for plugin authors who serialize `params`
   (`10:26`), and — decisively for a *core* dependency that folds into `libarbc` —
   it is header-only, so it imposes no transitive link/system burden on embedders
   (`10:33-34`). Doc 08 already frames the parser as swappable: "it's the format
   that's contractual, not the parser" (`10:26`); a binary/perf profile is
   explicitly evidence-gated (`08:12-13`).
   *Rejected — `simdjson`+separate writer:* fastest parse, but SAX-style and
   read-optimized; it needs a bolt-on writer to produce canonical output and does
   not model a mutable DOM, so unknown-kind verbatim round-trip and re-serialization
   cost more code for a perf win no v1 workload has evidence for. *Rejected —
   `yyjson` (C):* excellent perf but a C API and its own build; the ergonomic and
   header-only wins of `nlohmann` matter more for a small-graph format where parse
   speed is not the bottleneck (`08:9-13`, bulk assets stay external).

2. **Errors as values at the boundary via the non-throwing parse overload — no
   `nlohmann` exception crosses the API.** `nlohmann::json` throws by default;
   `parse(input, /*cb*/nullptr, /*allow_exceptions*/false)` returns a discarded
   value instead, which the L4 loader surfaces as `arbc::expected`. This honors doc
   03 / doc 10's "no exceptions across the boundary; errors are values" (`10:15-17`)
   without a broad try/catch. The smoke test pins the non-throwing path.
   *Rejected — wrap every `nlohmann` call in try/catch:* larger surface, easy to
   miss a call site, and pays exception-unwinding structure for a routine parse
   failure that the non-throwing overload models directly.

3. **Consumption: find-first with a pinned `FetchContent` fallback, SYSTEM includes,
   never an in-tree vendored copy — design-doc delta.** Doc 08 §Dependency note says
   "unproblematic vendoring" (`08:102`) while doc 10 says "never vendored copies
   in-tree; consume through `find_package`" (`10:31-34`). The shipped Catch2 wiring
   (`CMakeLists.txt:70-74`) already reconciles these: `FetchContent_Declare … GIT_TAG
   <pin> FIND_PACKAGE_ARGS` prefers a system package and falls back to a pinned
   clone — reproducible, distro-friendly, and never a mutable in-tree copy. Read doc
   08's "vendoring" as "cleanly redistributable/consumable," not an endorsement of
   an in-tree copy. *Design-doc delta:* doc 10's JSON-library row records this as the
   canonical consumption mechanism (doc 10 is the tooling/packaging home; doc 08
   already defers candidates to it, `08:99`). Same-commit per doc 16.
   *Rejected — vendor `nlohmann/json.hpp` in-tree (as the imageseq/miniaudio plugins
   vendor their private codec headers):* those are *plugin-private* deps deliberately
   kept out of `libarbc`; a *core* dep in-tree violates doc 10's "never vendored"
   promise and defeats lockstep CI version pinning.

4. **This task does not create `src/serialize/` or the `arbc_serialize` component.**
   The library is wired and probed by a standalone smoke test; the component that
   links it and grows the `serialize()`/canonical-writer surface is
   `serialize.writer`'s first act. Splitting the *decision + dependency wiring*
   (0.5d) from the *component + writer* (2d) keeps this gate the cheap, reusable
   seam it is meant to be and avoids landing an empty component.
   *Rejected — stand up `src/serialize/` now with the FetchContent link on it:* an
   empty component whose only consumer is a smoke test is abstraction ahead of need;
   `writer` creates it the moment there is real code to hold.

5. **No doc-00 decision-record bullet.** doc 10 already committed the project to
   taking a JSON dependency and to the `nlohmann` lean (`10:26`), and doc 17 already
   sanctions the edge (`17:58`); finalizing the specific library within an
   already-sanctioned slot is a v1-scope tooling call, not a newly project-shaping
   decision — the same reasoning `contract.registry` Decision 2 applied to its
   metadata-scope call. The doc-10 table row is the right and only home.
   *Rejected — add a doc-00 bullet:* would inflate the decision record with a detail
   doc 10 already owns; doc 00 is for project-shaping decisions.

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- `CMakeLists.txt` — `FetchContent_Declare(nlohmann_json …)` at pinned `GIT_TAG v3.11.3` with `FIND_PACKAGE_ARGS 3` and SYSTEM-include re-marking (mirrors the Catch2/benchmark patterns; `-Werror -Wpedantic` stays green on third-party headers).
- `docs/design/10-tooling-and-packaging.md` — JSON-library row promoted from "needed; evaluate…" to a decided entry recording the four doc-08 requirements satisfied, the parked `simdjson`/`yyjson` evidence-gated options, and the find-first/pinned-FetchContent/never-vendored consumption mechanism.
- `tests/CMakeLists.txt` — registers `arbc_serialize_json_dep_smoke_t`, a standalone target linking `nlohmann_json::nlohmann_json` + Catch2; no `enforces:` claim (doc-08 round-trip/determinism claims owned by `serialize.format_tests`/`serialize.kind_params`).
- `tests/serialize_json_dep_smoke.t.cpp` — 3-case build-integration probe: (i) unknown-kind tree verbatim round-trip (`parsed == reparsed`), (ii) non-throwing `parse(bad, nullptr, false)` returns a discarded value with no exception escaping, (iii) default `json` object dumps keys in sorted order; all three pass.
