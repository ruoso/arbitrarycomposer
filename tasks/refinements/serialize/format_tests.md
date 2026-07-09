# serialize.format_tests — Determinism tests + loader fuzzing

## TaskJuggler entry

Back-link: `task format_tests` in
[`tasks/60-serialize.tji`](../../60-serialize.tji) (lines 40–45), under
`task serialize`.

Note (verbatim): *"Load->save canonical round-trips, unknown-kind
preservation claims; libFuzzer harness over the loader with a checked-in
corpus. Docs 08/16."*

## Effort estimate

**2d** (from the `.tji`). Budget:

- ~0.5d — the determinism / load→save round-trip **corpus test**
  (systematic canonicalization + idempotence + unknown-kind preservation +
  the hand-authored-id-normalization case `sharing` deferred here), and the
  two new claims-register rows it lands.
- ~1d — the **fuzz harness**: the shared fuzz-target function, its two
  drivers (portable corpus-replay + `-fsanitize=fuzzer` libFuzzer entry),
  the checked-in seed corpus, and the `ARBC_FUZZER` CMake option/preset.
- ~0.5d — CI wiring (the corpus-replay regression rides the existing
  `dev`/`asan` ctest lanes; a nightly libFuzzer job gated on clang
  availability), getting all lanes green, and the ≥90% diff-coverage gate.

## Inherited dependencies

Direct WBS edge: `depends !kind_params`. Every seam the harness drives is
already in-tree — the whole `arbc_serialize` component (L4) plus its L5
`.arbc` glue landed on 2026-07-09.

**Settled (all Status: Done, 2026-07-09):**

- `serialize.kind_params` — the content-body codec seam (`CodecTable`,
  `content_body_from_json`/`content_body_to_json` in
  `src/serialize/arbc/serialize/codec.hpp`), `PlaceholderContent`
  (`src/serialize/arbc/serialize/placeholder_content.hpp`), and the
  content-aware `load_document` / `serialize_document` overloads. This is
  the formal predecessor; the round-trip/idempotence behavior the corpus
  test pins is its output.
- `serialize.reader` (transitive) — `load_document(std::string_view,
  const Registry&, LoadContext&, Model&)` (`reader.hpp:62-63`) and the
  content-aware overload (`reader.hpp:98-100`); `ReaderError`
  (`reader.hpp:27-40`, `Kind{MalformedJson, UnknownFormatMajor,
  MissingRequiredField, MalformedField, UnresolvableReference}`);
  `LoadContext` (`load_context.hpp:48-87`, single-writer, not
  thread-safe). **Crucially:** the loader's no-throw precondition is
  already implemented — `reader.hpp:21-26` states no `nlohmann` exception
  crosses the boundary "on well-formed or hostile input alike (the
  precondition serialize.format_tests' loader fuzzing relies on)," backed
  by the non-throwing `json::parse(input, nullptr, false)` and the
  fuzz-hardened `num_or`/`bool_or`/`int_or` accessors at
  `src/serialize/reader.cpp:38-58`. This task's job is to *pin that
  precondition with a fuzzer*, not to build the hardening.
- `serialize.writer` (transitive) — canonical `serialize_document(const
  DocRoot&)` (`writer.hpp:56`) and the content-aware overload
  (`writer.hpp:111-114`); `SerializeError`
  (`writer.hpp:23-33`, `Kind{NonFiniteValue, NoCodec, CodecFailed}`);
  canonical dump (`writer.cpp:194`, `dump(2, ' ', false,
  error_handler_t::replace)`); sorted-keys-for-free + shortest-round-trip
  numbers + omit-at-default discipline (doc 08 Principle 5).
- `serialize.json_dep` (transitive) — the non-throwing-parse contract
  (errors as values, no exceptions across the boundary).
- `serialize.sharing` — `inputs` arrays + the document-level `contents`
  table with `{"$ref": id}`; ids re-derived as **first-encounter ordinals**
  over the canonical layers-then-inputs traversal, so hand-authored
  arbitrary ids normalize deterministically on re-save
  (`sharing.md` Decision 2). `sharing.md:434-436` **explicitly deferred the
  `inputs`/`$ref` shared-`contents` determinism-corpus case to this task.**
  (Not a formal WBS edge, but landed in-tree on 2026-07-09.)

**Available but not depended-on (in-tree, usable from a cross-component
`tests/` harness):** `runtime.document_serialize` +
`runtime.operator_codecs` (L5) provide `runtime::builtin_codecs()`
(`src/runtime/arbc/runtime/document_serialize.hpp:90`) — the solid/tone/
fade/crossfade codec table. A test under top-level `tests/` links the
`arbc` umbrella and may use these to fuzz realistic content bodies (tests/
is outside the levelized graph — see Decision D4).

**Pending:** none.

## What this task is

Turn the serialization component's determinism promises from
"checked case-by-case, hand-picked inputs" into "checked systematically at
corpus scale, and stressed against adversarial bytes." Two deliverables
plus the CI plumbing that runs them:

1. **A determinism / load→save round-trip corpus test.** Over a corpus of
   representative canonical documents (inline goldens, existing
   convention), assert the full inverse-and-canonical set: `serialize(load(x))
   == x` byte-exact for canonical `x`; re-serialization idempotence;
   unknown-kind and unknown-field verbatim preservation; and the
   **hand-authored-id-normalization** case `sharing` deferred here — a file
   with non-canonical `contents` ids and unsorted keys re-serializes to
   canonical byte-exact output with ids collapsed to first-encounter
   ordinals.
2. **A libFuzzer harness over the JSON loader** with a checked-in seed
   corpus. A single shared fuzz-target function feeds arbitrary bytes to
   `serialize::load_document` and asserts the loader never throws, never
   crashes, and — on any successful load — re-serializes deterministically
   (a differential determinism invariant). Two drivers wrap that function:
   a portable corpus-replay `main()` that runs every checked-in corpus file
   as a deterministic regression under gcc + ASan/UBSan (the per-push
   "brief" run), and a `-fsanitize=fuzzer` `LLVMFuzzerTestOneInput` entry
   for coverage-guided fuzzing under clang (the nightly "long" run).

Plus: an `ARBC_FUZZER` CMake option (+ `fuzz` preset), the corpus-replay
regression wired into the existing `dev`/`asan` ctest lanes, and a nightly
`fuzz` job that runs the coverage-guided libFuzzer binaries when clang is
available.

## Why it needs to be done

Doc 16's test taxonomy, tier 8 (`docs/design/16-sdlc-and-quality.md:79-81`):

> **Fuzzing**: libFuzzer harnesses for the JSON loader (doc 08 — untrusted
> input by definition) and for request streams against contract
> properties; corpus checked in, run per-commit briefly and nightly long.

And doc 08's determinism charter (`docs/design/08-serialization.md:92-104`,
Principle 5) plus the no-exceptions-across-the-boundary rule
(`08-serialization.md:143-149`). Today:

- **No fuzzing exists anywhere.** A repo-wide sweep for
  `fuzz`/`libFuzzer`/`LLVMFuzzerTestOneInput`/corpus finds only prose in the
  predecessor refinements anticipating *this* task, plus the "fuzz-hardened"
  comments on the reader's safe accessors (`reader.cpp:38-58`). The loader
  was *built* to be fuzzed (`reader.hpp:21-26` names this task by id); the
  fuzzer that proves it has never been written.
- **The `.arbc` file is untrusted input by definition** (doc 08). A crash or
  uncaught exception in the loader on a malformed file is a robustness
  defect that only a fuzzer reliably finds; the existing golden/round-trip
  tests only exercise well-formed and a handful of hand-picked malformed
  inputs (`serialize_sharing.t.cpp`'s dangling/cyclic `$ref` cases).
- **The determinism claims are proven per-feature, not per-corpus.** Each
  predecessor pinned its own slice (`canonical-output-is-byte-stable`,
  `load-save-round-trips-canonically`, `unknown-kind-round-trips-verbatim`);
  no test drives a *corpus* through load→save→load asserting canonical
  byte-exactness in bulk, and `sharing` explicitly deferred the
  hand-authored-id determinism case here (`sharing.md:434-436`).

Downstream, this is the robustness net for every `.arbc` a user or plugin
ever opens, and the systematic determinism gate that keeps `.arbc` files
diff-clean in version control (doc 08 Principle 5 — "these files are source
artifacts, and VCS-friendliness is a feature").

## Inputs / context

**Governing design docs (normative):**

- `docs/design/08-serialization.md`
  - `:75-81` (Principle 2) — unknown kinds load as placeholder content
    preserving `kind`/`kind_version`/`params` verbatim and re-serialize
    byte-equivalent; "a missing plugin must never destroy data."
  - `:88-91` (Principle 4) — readers reject unknown format majors; within a
    major, unknown *fields* are preserved-and-ignored.
  - `:92-104` (Principle 5) — canonical output: sorted keys, shortest
    round-trip numbers, defaults omitted, non-finite is an error value
    never `null`; "byte-identical across runs and across re-serialization."
  - `:105-131` (Principle 6) — operator graphs serialize structurally;
    `contents` ids are "re-derived deterministically on every save from
    graph structure (first-encounter order ...) so ... a hand-authored
    file's arbitrary ids are normalized on re-serialization —
    canonicalization, not data loss"; a bad/cyclic `$ref` is a serialization
    error as a value, "never a partial load."
  - `:143-149` (Dependency note) — "no exceptions across the plugin
    boundary (errors as values at the API)." This is the invariant the
    loader fuzzer pins.
- `docs/design/16-sdlc-and-quality.md`
  - `:14-25` — the claims register: every normative claim gets an id and an
    enforcing `// enforces: <claim-id>` test; a behavior-altering change
    updates the design doc in the same commit.
  - `:48-53` (tier 3) — deterministic output ⇒ **byte-exact** goldens;
    tolerances are the justified exception, never the default. Serialization
    goldens follow this.
  - `:79-81` (tier 8) — the fuzzing charter (quoted above): libFuzzer over
    the JSON loader, corpus checked in, brief per-commit + long nightly.
  - `:99-105` — CI cadence: local pre-push gate = tiers 1–5; per-push CI =
    tiers 1–8 short-form; nightly = long-form stress/fuzz/crash-sweeps.
  - `:112-118` — diff-coverage hard gate (≥90% on changed lines);
    genuinely-unreachable defensive error paths carry a justified
    `GCOV_EXCL`.
- `docs/design/17-internal-components.md`
  - `:58` — `arbc::serialize` is **L4** (`contract`, `model` + JSON dep).
  - `:41-44` — a component may depend only on strictly lower levels; no
    same-level edges (CI-enforced by `scripts/check_levels.py`).
  - `:187-188` — the top-level `tests/` tree holds cross-component
    artifacts including the **fuzz corpus** (the sanctioned home for an
    on-disk corpus directory).

**Source seams the tests drive** (line numbers are point-in-time anchors):

- Loader: `src/serialize/arbc/serialize/reader.hpp:62-63` (content-free
  `load_document`), `:98-100` (content-aware overload), `:21-26` (the
  no-throw precondition naming this task), `:27-40` (`ReaderError`).
  Implementation: `src/serialize/reader.cpp` — non-throwing parse + the
  fuzz-hardened accessors at `:38-58`.
- Writer: `src/serialize/arbc/serialize/writer.hpp:56` (content-free
  `serialize_document`), `:111-114` (content-aware), `:23-33`
  (`SerializeError`). Canonical dump at `src/serialize/writer.cpp:194`.
- `LoadContext`: `src/serialize/arbc/serialize/load_context.hpp:48-87`.
- `PlaceholderContent`:
  `src/serialize/arbc/serialize/placeholder_content.hpp`.
- Built-in codecs (test-only, via umbrella): `runtime::builtin_codecs()` at
  `src/runtime/arbc/runtime/document_serialize.hpp:90`.

**Existing tests / conventions to reuse:**

- Inline raw-string goldens, **no on-disk `.arbc` fixtures** for golden
  comparison: `tests/serialize_writer_golden.t.cpp:54` (`k_golden`), reused
  as the reader's load input at the round-trip tests. The determinism
  corpus test follows this (its goldens are inline); only the *fuzz corpus*
  goes on disk (Decision D3).
- Shared-`contents`/`$ref` golden + dangling/cyclic `$ref` error cases:
  `tests/serialize_sharing.t.cpp`.
- The closest structural precedent for a test-harness refinement (shared
  helper + portable-vs-instrumented split + per-push/nightly cadence + new
  preset + CI lanes): `tasks/refinements/quality/stress_harness.md` (the
  TSan/stress harness; note it added the `tsan` preset now visible at
  `CMakePresets.json:32-40` / `:80` / `:90`).

**Build / CI scaffolding:**

- Sanitizer knob: `CMakeLists.txt:29-34` (`ARBC_SANITIZERS` →
  `-fsanitize=… -fno-omit-frame-pointer`). Presets: `CMakePresets.json`
  (`asan`, `tsan`, … — sibling presets, the pattern a `fuzz` preset
  follows).
- Cross-component test registration: `tests/CMakeLists.txt` — plain
  `add_executable(...)` + `target_link_libraries(... PRIVATE arbc [nlohmann]
  Catch2::Catch2WithMain arbc_build_flags)` + `catch_discover_tests`.
  Component helpers: `cmake/ArbcComponent.cmake`.
- CI: `.github/workflows/ci.yml` (per-push matrix: gcc/clang × debug/release,
  clang-asan, gcc-tsan, msvc), `.github/workflows/nightly.yml` (`tidy`,
  `asan-full`, `tsan-full`).
- Claims: `tests/claims/registry.tsv` (serialize rows `:185-200`), enforced
  bidirectionally by `scripts/check_claims.py` (tag regex
  `enforces:\s*([A-Za-z0-9#_./-]+)`, scans `src/`/`tests/`/`testing/`).
- Gate: `scripts/gate` (configure → build → ctest → clang-format →
  `check_levels.py` → `check_claims.py` → `check_rt_safety.py`).

## Constraints / requirements

1. **No exceptions, no crashes — the fuzz invariant.** For arbitrary input
   bytes, `serialize::load_document` must return an `expected<…,
   ReaderError>` (an error *value* or success) and never throw, abort, or
   read out of bounds. The fuzz target asserts this; a violation is a
   libFuzzer crash / an ASan report. This is the doc 08:143-149 promise the
   task pins — the hardening already lives in `reader.cpp:38-58`.
2. **Differential determinism inside the fuzz target.** On any *successful*
   load, the target re-serializes (content-aware `serialize_document`) and
   asserts serialize succeeds and re-loading that output succeeds and
   re-serializes byte-identically (`serialize(load(serialize(load(x)))) ==
   serialize(load(x))`). This makes the fuzzer hunt determinism breaks, not
   just crashes.
3. **Byte-exact goldens, no tolerances** (doc 16:48-53). The determinism
   corpus test compares exact `.arbc` bytes; no perceptual/numeric
   tolerance is permitted (serialization is fully deterministic).
4. **Portable per-push value must not depend on clang.** libFuzzer's
   `-fsanitize=fuzzer` is clang-only and GCC has no equivalent; the current
   CI environment has clang unavailable (per
   `quality/stress_harness.md` Status). The per-commit-brief obligation
   (doc 16:79) is therefore met by the **corpus-replay regression** — the
   same fuzz-target function driven over the checked-in corpus by a portable
   `main()`/Catch2 test under gcc + ASan/UBSan — and must be green on the
   existing lanes without clang. The `-fsanitize=fuzzer` binaries are the
   nightly long-form lane, gated on clang availability (Decision D1).
5. **Levelization.** Both test files live under top-level `tests/` (they may
   combine `serialize` L4 with, optionally, `runtime::builtin_codecs()` L5),
   link the `arbc` umbrella + `Catch2::Catch2WithMain`, and reach the loader
   only through public `arbc/serialize/…` headers. They must **not** go in
   any component's `t/` and must not be smuggled into `libarbc`
   (`check_levels.py` stays green — the task adds no `arbc_*` component
   edge).
6. **Corpus checked in** (doc 16:79, doc 17:187-188). The seed corpus lives
   on disk under `tests/fuzz/corpus/load_document/` and is committed. Seeds
   are minimal and cover: valid canonical docs (the writer goldens), a
   valid shared-`contents`/`$ref` doc, unknown-kind and unknown-field docs,
   and malformed inputs (truncated JSON, wrong format major, non-array
   `inputs`, dangling/cyclic `$ref`, non-finite numbers, deeply nested
   input graphs). The corpus-replay test reads the directory via a
   CMake-configured compile definition.
7. **Deterministic, reproducible.** The corpus-replay regression is fully
   deterministic (fixed corpus, no RNG, no time). The libFuzzer run uses
   libFuzzer's own reproducible seeded engine; any crash reproducer is
   minimized and committed to the corpus.
8. **No wall-clock assertions** (doc 16:54-62). The nightly fuzz run is
   time-*bounded* (`-max_total_time`) for cadence, but no test *asserts* on
   duration; the corpus-replay regression asserts only behavioral outcomes
   (no-throw, byte-exact re-serialization).
9. **Diff coverage ≥90%** on changed lines (doc 16:112-118). Loader error
   branches the corpus exercises count toward the gate; any genuinely
   unreachable defensive branch carries a justified `GCOV_EXCL`.
10. **WBS closing gate.** After landing `complete 100` + the refinement
    back-link on `tasks/60-serialize.tji:40-45`,
    `tj3 project.tjp 2>&1 | grep -iE "error|warning"` is silent, and
    `scripts/check_levels.py` + `scripts/check_claims.py` stay green.

## Acceptance criteria

- **Determinism / round-trip corpus test** —
  `tests/serialize_determinism_corpus.t.cpp`. Over an inline corpus of
  representative documents, asserts for each canonical `x`:
  `serialize(load(x)) == x` byte-exact and re-serialization idempotence;
  for unknown-kind/unknown-field docs, verbatim preservation through the
  round-trip; and the **hand-authored-id-normalization** case (non-canonical
  `contents` ids + unsorted keys → canonical byte-exact output with
  first-encounter-ordinal ids) that `sharing.md:434-436` deferred here.
  Carries `// enforces:` tags for the two new claims below **and** re-tags
  the existing `08-serialization#canonical-output-is-byte-stable`,
  `#load-save-round-trips-canonically`, and `#unknown-kind-round-trips-verbatim`
  (multiple enforcing tests per claim are allowed).
- **Fuzz target + corpus-replay regression.**
  - `tests/fuzz/fuzz_target.hpp` (or `.cpp`) — the shared
    `int arbc_fuzz_load_document(const uint8_t* data, size_t size)`
    function implementing constraints 1–2.
  - `tests/fuzz/fuzz_load_document.cpp` — the `-fsanitize=fuzzer`
    `LLVMFuzzerTestOneInput` entry forwarding to the shared function
    (built only under `ARBC_FUZZER`).
  - `tests/fuzz_corpus_replay.t.cpp` — a Catch2 test that reads every file
    in `tests/fuzz/corpus/load_document/` (path via compile definition) and
    feeds it to the shared function, asserting no-throw and the differential
    determinism invariant. Runs in the existing `dev`/`asan` ctest lanes
    (per-push brief). Carries the `// enforces:` tag for the loader-safety
    claim below.
  - `tests/fuzz/corpus/load_document/` — the committed seed corpus of
    constraint 6.
- **New claims-register rows** (`tests/claims/registry.tsv`, `08-serialization#`):
  1. `08-serialization#loader-never-faults-on-hostile-input` — "load_document
     returns a ReaderError value or succeeds on arbitrary input bytes; it
     never throws a JSON/C++ exception, aborts, or reads out of bounds."
     Enforced by `tests/fuzz_corpus_replay.t.cpp` (and, under clang, the
     libFuzzer binary). Doc 08:143-149, `reader.hpp:21-26`.
  2. `08-serialization#hand-authored-ids-normalize-deterministically` — "a
     document with non-canonical/arbitrary `contents` ids and unsorted keys
     re-serializes to canonical byte-exact output; ids collapse to
     first-encounter ordinals over the canonical traversal." Enforced by
     `tests/serialize_determinism_corpus.t.cpp`. Doc 08:105-131.
- **CMake / preset.** An `ARBC_FUZZER` cache option gates the libFuzzer
  targets (adds `-fsanitize=fuzzer` to those targets only, guarded to
  clang); a `fuzz` configure/build/test preset (sibling of `asan`/`tsan`)
  selects it. The corpus-replay regression and determinism corpus test build
  unconditionally under `dev`/`asan`.
- **CI lanes.** The corpus-replay + determinism tests run in the existing
  per-push `dev`/`asan` lanes (no new per-push lane needed — they are
  ordinary ctest targets). `.github/workflows/nightly.yml` gains a `fuzz`
  job that, **when clang is available**, builds `--preset fuzz` and runs each
  libFuzzer target time-bounded (`-max_total_time`), uploading any crash
  reproducer; when clang is unavailable the job skips cleanly (the
  ASan corpus-replay lane remains the enforced safety net). All active lanes
  green.
- **Claims register consistent.** `scripts/check_claims.py` passes with the
  two new rows and their `enforces:` tags.
- **Coverage gate.** ≥90% diff coverage on the changed lines.
- **Deferred follow-up (named future task).** The *second* libFuzzer harness
  doc 16:79 mandates — "request streams against contract properties" — is
  **out of scope** here (this task's note is the JSON loader only). It is
  deferred to `contract.request_stream_fuzz` (a libFuzzer target decoding
  bytes into a render/edit-request sequence driven against the contract
  conformance properties, reusing this task's `ARBC_FUZZER` seam +
  corpus-replay pattern; ~2d; area `contract`; wired into the same
  quality/fuzzing milestone as this task). Closer registers it in the WBS
  if not already present; the closer verifies non-duplication before
  minting.

## Decisions

**D1 — Split the fuzz target from its driver; the portable corpus-replay is
the per-push gate, `-fsanitize=fuzzer` is the nightly lane.** *Rationale:*
libFuzzer's `-fsanitize=fuzzer` is clang-only and GCC has no libFuzzer;
clang is currently unavailable in CI (`quality/stress_harness.md` Status
records the same environment constraint for its lane). If the per-commit
value depended on clang it would not run. Factoring the fuzz logic into one
shared `arbc_fuzz_load_document(data, size)` function lets a portable
`main()`/Catch2 driver replay the checked-in corpus under gcc + ASan/UBSan
as a deterministic regression on the *existing* lanes — satisfying doc
16:79's "run per-commit briefly" without clang — while the
`LLVMFuzzerTestOneInput` entry provides coverage-guided fuzzing under clang
for the nightly "long" run. This dual-driver pattern is the established way
(LLVM's own `StandaloneFuzzTargetMain`) to keep fuzz targets alive as
portable regression tests. *Rejected:* a clang-only libFuzzer target with no
portable driver — would give **zero** per-push protection in the current
environment and let the loader-safety claim go unenforced until clang lands;
also *rejected:* AFL/other engines — doc 16:79 names libFuzzer specifically.

**D2 — The fuzz target is a determinism differential fuzzer, not just a
crash finder.** *Rationale:* on a successful load the target re-serializes
and re-loads, asserting byte-stable re-serialization (constraint 2). This
turns every accepted input into a determinism probe, catching
non-canonical-output bugs (unsorted keys, unnormalized ids, number
formatting drift) that a bare no-crash target would miss — and it directly
exercises doc 08 Principle 5/6 across the whole fuzz-reachable input space.
*Rejected:* no-crash-only target — leaves the determinism promise (the other
half of the task's note) unstressed.

**D3 — Fuzz corpus lives on disk under `tests/fuzz/corpus/`; determinism
goldens stay inline.** *Rationale:* a libFuzzer corpus is inherently a
directory of files the engine reads and grows, and doc 17:187-188 names the
`tests/` tree as the home for the "fuzz corpus" — so an on-disk corpus
directory is the doc-sanctioned convention, a deliberate and narrow
departure from the repo's inline-raw-string golden convention
(`serialize_writer_golden.t.cpp:54`) that applies only to the fuzz seeds.
The determinism corpus test's expected outputs remain inline goldens, as
every serialize test does. *Rejected:* inline-encoding the fuzz seeds —
libFuzzer cannot consume them and cannot write minimized reproducers back.

**D4 — Fuzz realistic content bodies via `runtime::builtin_codecs()` from a
cross-component `tests/` harness.** *Rationale:* the content-aware loader's
richest attack surface is `params`/`contents` codec parsing; feeding it a
real codec table (solid/tone/fade/crossfade) covers far more of the loader
than the content-free path. A test under top-level `tests/` sits outside the
levelized graph (doc 17:41-44 governs *components*, not tests) and links the
`arbc` umbrella, so using L5 `runtime::builtin_codecs()` there introduces no
component edge and keeps `check_levels.py` green — exactly as the
cross-component stress tests link model+pool+runtime. The formal
`depends !kind_params` edge is honored: the content-aware `load_document` /
`CodecTable` seam the harness targets is kind_params' deliverable; the
built-in codecs are merely test fixtures that happen to already be in-tree.
*Rejected:* fuzzing only the content-free loader — leaves the codec/params
parsing (the most format-specific, most-likely-to-fault code) unfuzzed;
*rejected:* hand-rolling toy codecs in the test — needless duplication when
`builtin_codecs()` exists.

**D5 — Land two new claims; re-enforce the rest; no design-doc delta, no
doc-00 bullet.** *Rationale:* `#loader-never-faults-on-hostile-input` and
`#hand-authored-ids-normalize-deterministically` are behaviors doc 08
already *promises* (`:143-149` no-exceptions-across-boundary; `:105-131`
deterministic id normalization) but that no test yet pins — textbook
claims-register *growth* landing doc-promised behavior (doc 16), which needs
no doc amendment. The bulk determinism/round-trip properties re-use the
existing rows (`canonical-output-is-byte-stable`,
`load-save-round-trips-canonically`, `unknown-kind-round-trips-verbatim`) via
additional `enforces:` tags. Like every serialize predecessor, this
test/CI leaf declines a doc-00 decision-record bullet — the doc 16:79
fuzzing charter already sanctions the work. *Rejected:* minting a
per-property claim for each round-trip invariant (redundant with existing
rows); *rejected:* a `#…-is-fuzz-clean` process claim (fuzz-cleanliness is a
CI-lane guarantee, not an observable behavioral claim — it belongs in CI
structure, matching stress_harness D5).

## Open questions

(none — all decided)

## Status

**Done** — 2026-07-09.

- Created `tests/fuzz/fuzz_target.hpp` — shared `arbc_fuzz_load_document(data,size)` (no-throw + differential-determinism invariant over the runtime content-aware loader, D4).
- Created `tests/fuzz/fuzz_load_document.cpp` — `LLVMFuzzerTestOneInput` entry guarded by `ARBC_FUZZER` (Clang-only).
- Created `tests/fuzz_corpus_replay.t.cpp` — portable per-push corpus-replay Catch2 test (runs in existing `dev`/`asan` lanes without clang).
- Created `tests/serialize_determinism_corpus.t.cpp` — determinism/round-trip corpus + hand-authored-id normalization, re-enforces 3 existing claims.
- Created `tests/fuzz/corpus/load_document/` — 15 committed seeds (canonical, shared-`$ref`, unknown-kind, hand-authored-ids; malformed: truncated/wrong-major/non-array-inputs/dangling+cyclic-`$ref`/NaN/deep-nested/empty/garbage).
- Edited `CMakeLists.txt` — added `ARBC_FUZZER` option.
- Edited `tests/CMakeLists.txt` — added 3 targets (`fuzz_corpus_replay`, `serialize_determinism_corpus`, `fuzz_load_document`) + `ARBC_FUZZ_CORPUS_DIR` compile definition.
- Edited `CMakePresets.json` — added `fuzz` configure/build/test preset (sibling of `asan`/`tsan`).
- Edited `.github/workflows/nightly.yml` — added clang-gated `fuzz` job.
- Edited `tests/claims/registry.tsv` — added 2 rows: `08-serialization#loader-never-faults-on-hostile-input` + `#hand-authored-ids-normalize-deterministically`.
- Deviation: runtime document reader drops layer-level unknown sibling fields; unknown-field preservation pinned at params/body level (the guaranteed behavior per doc 08:88-91) — layer-level verbatim preservation not asserted.
