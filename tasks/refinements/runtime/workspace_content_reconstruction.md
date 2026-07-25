# runtime.workspace_content_reconstruction — Make a workspace reopen give back its content

`Document::open` restores the record graph and binds **no `Content` for any kind**,
so a reopened workspace cannot be rendered, edited or hit-tested. This task closes
that two ways at once: an immediate **rebind seam** so a host can repair a reopened
document from whatever it does have, and the **persisted construction identity** that
lets the library repair it itself — which is what doc 15's "map the file, read the
last valid root, resume" has promised all along.

Reported as [arbitrarycomposer#19](https://github.com/ruoso/arbitrarycomposer/issues/19)
from `ruoso/arbitraryeditor` at the v0.3.0 pin.

## TaskJuggler entry

`tasks/65-runtime.tji` — `task workspace_content_reconstruction`, `effort 8d`,
`allocate team`, `depends model.persistent_state_walk_hook, runtime.document_serialize,
pool.workspace_store_directory`. Docs 08/14/15.

## Effort estimate

**8d**, and it is two tasks' worth of work deliberately kept in one because the
smaller half is meaningless without the larger being *decided*: shipping a rebind
seam while leaving the format gap unregistered would freeze "the host repairs it"
as the architecture.

- **~1d — the rebind seam.** `Document::rebind_content`, `Document::recovered_content_state`,
  and the tests that pin them. Small because the binding table is already a COW map
  built for exactly this shape.
- **~7d — the persisted identity.** A model-record format change (`ContentRecord`
  grows two block refs), a `BigBlockPool` at the model level, blob reachability in
  the recovery walk, capture-on-add through the codec, the reconstruction pass in
  `Document::open`, and the doc 14/15 deltas. The crash sweeps and the workspace
  goldens move with the record format.

## Inherited dependencies

**Settled (formal `depends`):**

- `model.persistent_state_walk_hook` (issue #5) — established that `Model::open`
  **collects** what it cannot interpret and the runtime replays it later, because
  `Model::open` runs before any `Document` exists and levelization forbids the model
  naming a kind (`model.hpp:309-331`). This task reuses that shape exactly: the
  recovery walk collects the persisted construction identity; the runtime turns it
  into `Content` objects. The issue is right that this trio "landed and works" and is
  "one level too high" — it replays *state* onto a content that does not exist yet.
- `runtime.document_serialize` — owns the `ContentRecord.kind` uint64 ↔ reverse-DNS
  `kind_id` bijection (`document_serialize.hpp:36-58`) and the `CodecTable`. This
  task makes the bridge's output *persistent*, which changes it from a per-session
  interning table to something with a durable side.
- `pool.workspace_store_directory` — per-store chunk ownership in the workspace
  header, which is what lets a new size-classed store appear in the file and be
  routed back to its owner on reopen without a header format change.

**Settled (informal — seams this builds on):**

- `pool.big_block_pool` — `BlockSlotRef` is *already* "standard-layout and trivially
  copyable so it can live inside an mmapped record… it carries the logical byte
  length" (`big_block_pool.hpp:39-48`). It is the in-record variable-length reference
  this task needs, and it exists.
- `serialize.kind_params` — `load_document(json, registry, codecs, ctx, sink, into)`
  already routes each content body through `CodecTable` and sinks the resulting
  `Content` (`reader.hpp:90-122`). The reconstruction pass is that routing driven from
  records instead of from JSON.

**Pending (downstream, not blocking):** `kinds.raster_workspace_backing` supplies the
*state* half for the first kind that has one. The two are orthogonal and compose:
identity says which content to build, state says what is in it.

## What this task is

### The three things the workspace does not persist

`ContentRecord` is `{std::uint64_t kind; StateHandle state;}` (`records.hpp:60-63`).
That is the whole of what a workspace reopen knows about a content, and it is not
enough to rebuild one, for three separate reasons:

1. **No construction parameters.** A solid's colour, an imageseq's directory, an
   image's path — the `ContentConfig` the factory was called with — live only in the
   `Content` object, which is process memory. Nothing persists them.
2. **No stable kind identity.** `kind` is a token from `KindBridge`, an in-memory
   table that interns on first sight and is rebuilt every session
   (`document_serialize.hpp:36-58`). Built-ins are pre-interned so their tokens are
   stable by luck; a plugin kind's token depends on the order the session happened to
   see kinds in, so the same file reopened by a differently-ordered session names
   different kinds.
3. **No operator input edges.** `FadeContent`, `CrossfadeContent` and
   `NestedContent` hold their inputs as `Content*` / a child `ObjectId` inside the
   object. `ContentRecord` has no `inputs`. Doc 08 Principle 6 calls input edges
   **core-owned** and the canonical writer persists them; the workspace does not.

`StateHandle` is a fourth thing and it is *already* accounted for — but it is inert
for every shipped kind (`model.hpp:325-327`: *"the list stays empty until the first
persistent workspace-backed kind lands"*), so today a workspace file contains
literally nothing about any content's payload either.

That is why the issue's requested option 1 — "reconstruct each recovered
`ContentRecord` through `registry.factory(kind_id)`" — cannot be implemented as
asked, and **must not be implemented approximately**. A factory called with an empty
config yields the right *type* with the wrong *parameters*: a black solid where a red
one was, an empty raster where a painting was, a nested layer pointing at nothing.
Binding nothing is detectable; binding wrong content is not. See Decision 1.

### The blocker the issue reports does not exist

The issue rules out its own option 2 on the grounds that
`src/pool/arbc/pool/slot_store.hpp:119` says *"it never rebinds and there is no
rebind API"*. That sentence is about the **writer-thread identity** a `SlotStore`
binds on first write — the paragraph it sits in is headed "Threading (doc 15,
pool.free_pools)" and continues *"A consumer whose writes originate on two threads
must FUNNEL them to one dedicated writer thread"*. It says nothing about content
binding.

`Document`'s id→`Content` table is a different thing entirely: a copy-on-write
`std::shared_ptr<ContentBindings>` swapped whole on the writer thread
(`document.cpp:120-128`), built that way so `resolve()` is a lock-free pinned read
(issue #10). Replacing a row in it is the same operation as adding one. A rebind seam
contradicts nothing.

### The rules

**Rule 1 — the record carries the content's construction identity.**
`ContentRecord` gains one `BlockSlotRef identity`: a big-block blob holding the
reverse-DNS `kind_id` and the kind's canonical `params` text, in that order,
length-prefixed. `BlockSlotRef` is 8 bytes, standard-layout and trivially copyable
(`big_block_pool.hpp:39-48`), so the record stays fixed-size and pointer-free —
the doc 15 position-independence requirement is untouched. `k_no_block` is the
absent case and reads exactly as today's record does.

The `kind_id` STRING is persisted, not the `kind` token. The token stays in the
record as the cheap in-memory discriminator it already is, but it is derived from
the string on reopen rather than trusted — which is what makes a plugin kind survive
a session that interned in a different order (finding 2).

**Rule 2 — the record carries its input edges.** `ContentRecord` gains a second
`BlockSlotRef inputs` holding the `ObjectId`s of this content's inputs, in
declaration order. This is doc 08 Principle 6's *core-owned input edges* given a home
in the record graph, which is where they always belonged: the canonical writer
already treats them as the core's rather than the codec's, and a `KindCodec::deserialize`
already takes them as a parameter (`registry.hpp:74-78`).

**Rule 3 — identity is captured through the codec, not supplied by the host.**
`Document::add_content` gains an optional `const CodecTable*` (or takes it once at
construction). When present, it calls the kind's `KindCodec::serialize(content)` —
which already exists, already produces canonical `params` text, and is already what
the save path uses — and writes the result into the record's identity blob in the
same transaction that publishes the record. One capture, at the one point a content
enters the document. A kind with no registered codec captures nothing and behaves
exactly as today (Decision 4).

**Rule 4 — reopen reconstructs through the same routing the canonical loader uses.**
`Document::open(path, registry, codecs, housekeeping)` walks the recovered content
records in input-topological order and, for each, calls
`codecs.deserialize(params, inputs, composition)` — the *identical* call
`load_document` makes per content body (`reader.hpp:90-99`). One reconstruction
routine, two drivers, so the workspace path cannot drift from the canonical one the
way a second render path did (doc 02 § The frame, offline). A record whose kind has
no codec, or whose blob is absent, binds nothing and is **reported**, not guessed.

**Rule 5 — the recovery walk retains the blobs.** `Model::open`'s reachability walk
already reconstructs refcounts for `HamtNode` and `ObjectRecord` slots
(`model.cpp:685-695`). It gains the identity/inputs blobs of every reachable
`ContentRecord`, retained into the model's `BigBlockPool` exactly as the node and
record counts are. A blob the walk does not reach is free space, by the same
complement rule as every other store.

**Rule 6 — the rebind seam ships regardless.**
`Document::rebind_content(ObjectId, std::shared_ptr<Content>)`, writer-thread only:
replace the row for an existing content record, re-running the `Editable` bind so a
rebound editable's sinks are live. It is the escape hatch for everything Rule 4
cannot do — a kind whose plugin is not loaded this session, a kind whose codec is
absent, a document written before this task — and it is what lets a host repair a
document rather than only report it. `Document::recovered_content_state()` ships with
it, the issue's secondary ask: `Model::recovered_content_state()` exists and
`replay_recovered_content_state` consumes it, but `Document` publishes no accessor, so
the issue-#5 trio is unreachable from the public host surface.

**Not this task:**

- Persisting a kind's *state slab* — `kinds.raster_workspace_backing`, already a leaf.
  This task makes the content exist for that state to be replayed onto.
- A workspace **header** format change. Rule 1/2's blobs ride the existing arena and
  the `pool.workspace_store_directory` store table, so the header shape is untouched.
- Cross-machine or cross-version workspace portability. Doc 15 is explicit that
  workspace files are same-machine artifacts with no portability promise; a
  `kind_version` mismatch on reopen is a reported reconstruction failure, not a
  migration.

## Why it needs to be done

A host that reopens a workspace gets a document it cannot render, edit or hit-test —
and today has no repair available, because the library publishes no rebind seam and
the alternative (a full canonical rebuild through `load_document`) both discards the
crash-recovery the workspace exists for and is impossible for a project that has never
been saved. The reporting host has already resorted to forcing the canonical rebuild
for any content-bearing workspace, which makes the fast path dead for real projects.

Doc 15 promises more than that. *"Crash recovery. The workspace file always contains
the records of every checkpointed version. Recovery is: map the file, read the last
valid root, resume. An editor crash costs at-most-since-last-checkpoint, not the
document."* Resuming into a document with no content in it is not that.

## Inputs / context

- **doc 15 § File-backed arenas** (15:275-300) — the four things workspace backing
  buys, "resume" first among them; and 15:315-322, the workspace-vs-canonical split.
- **doc 14 § Transactions / Identity** — the record graph this extends.
- **doc 08 Principle 6** — core-owned input edges (Rule 2), and Principle 1's
  JSON-free codec seam (Rule 3).
- `src/model/arbc/model/records.hpp:51-63` — `StateHandle`, `ContentRecord`.
- `src/model/arbc/model/model.hpp:309-331` — `RecoveredContentState`, and the comment
  recording that the list is empty for every shipped kind.
- `src/model/model.cpp:685-695` — the reachability walk that Rule 5 extends.
- `src/pool/arbc/pool/big_block_pool.hpp:39-70` — `BlockSlotRef`, the in-record
  variable-length reference.
- `src/runtime/document.cpp:61-66` (`open`), `:95-128` (`add_content`, and the COW
  binding swap Rule 6 reuses).
- `src/runtime/arbc/runtime/document_serialize.hpp:36-63` — `KindBridge`.
- `src/serialize/arbc/serialize/reader.hpp:85-122` — `ContentSink` and the
  codec-routing `load_document` Rule 4 shares.
- `src/contract/arbc/contract/registry.hpp:63-79` — `KindCodec`.

## Constraints / requirements

1. **`ContentRecord` stays standard-layout, fixed-size and pointer-free** (doc 15's
   position-independence rule). Two `BlockSlotRef`s, no strings, no owning pointers.
2. **Reconstruction is one routine.** Rule 4 shares `CodecTable` routing with
   `load_document`; a second per-content reconstruction path is forbidden — that is
   the mistake the two render drivers made.
3. **A record that cannot be reconstructed binds nothing and says so.** Never a
   default-constructed stand-in (Decision 1). The count and the ids are reported to
   the caller so a host can drive Rule 6 over exactly them.
4. **Byte-neutral for an anonymous document.** No blobs are written and no
   reconstruction runs; `Document(housekeeping)` and every existing test are unchanged.
5. **Back-compatible with existing workspace files.** A record whose identity blob is
   absent (`k_no_block`) reads exactly as today: the reopen binds nothing for it and
   reports it. Reopening a v0.3.0 workspace must not fail.
6. **Levelization (doc 17).** The model gains a `BigBlockPool` (pool is below model,
   already allowed) but never names a kind, a registry or a codec: it stores and walks
   opaque bytes. All interpretation is runtime's, exactly as `RecoveredContentState`
   already splits it.
7. **Writer-thread only** for `rebind_content`, `add_content`'s capture, and the
   reconstruction pass — the single-writer discipline `slot_store.hpp:113-119` states.

## Acceptance criteria

- **The headline round-trip.** A workspace-backed document holding a bounded
  `org.arbc.solid`, an `org.arbc.raster`, an operator over two leaves, and a nested
  composition is closed and reopened through `Document::open(path, registry, codecs)`;
  every content resolves, and a rendered frame is **byte-identical** to one rendered
  before the close. This is the test the issue's host pins say fails today.
- **The never-saved case.** The same, for a document with no canonical `.arbc` file
  anywhere — the case that has no host-side workaround at all.
- **Wrong content is never invented.** A record whose kind has no registered codec
  binds nothing, is reported in the open result, and `resolve()` returns null for it —
  asserted, not merely observed.
- **Plugin kind token instability.** A document written by a session that interned
  kinds in one order reopens correctly in a session that interns them in another —
  the finding-2 regression guard, which only a persisted `kind_id` string can pass.
- **Rebind.** `rebind_content` replaces a row, re-registers an `Editable`'s sinks, and
  a subsequent edit journals and undoes correctly against the rebound object.
- **Back-compatibility.** A workspace file written before this task reopens without
  error and reports every content as unreconstructed.
- **Crash recovery still holds.** The doc-15 kill sweeps pass with the widened record,
  including a kill injected between the identity-blob write and the record publish.
- Full `./scripts/gate` green (its exit code, not a `tail` of its log).

## Decisions

1. **A record that cannot be reconstructed binds nothing — never a default.** The
   issue asks for `registry.factory(kind_id)` reconstruction, which without persisted
   params yields right-type/wrong-parameter content. A black solid where the user left
   a red one is a silent data-loss bug that looks like a working feature; a null
   binding is a reported one. This is the same posture as `PlaceholderContent` on the
   canonical path (preserve and report, never guess), minus the body there is nothing
   to preserve.
2. **Persist the `kind_id` string, keep the token.** The token stays in the record as
   the in-memory discriminator every existing reader uses; the string is what reopen
   trusts. Persisting only the token would leave plugin kinds hostage to intern order;
   replacing the token with a string would make `ContentRecord` variable-size and
   every existing kind comparison a string compare.
3. **One blob, not two, for `kind_id` + `params`.** They are captured together,
   written together, read together and have the same lifetime; two block refs would
   cost a second size-class allocation and a second reachability edge for no
   independent use.
4. **Capture through the codec, not through a host-supplied config.** `add_content`
   could take the `ContentConfig` the host passed to the factory, which is cheaper.
   Rejected: it puts the burden on every caller, it goes stale the moment a content is
   edited, and it duplicates a serialization the `KindCodec` already performs and the
   save path already trusts. Capture-through-codec means the workspace and the
   canonical file agree by construction.
5. **Input edges in the record, not in the blob.** They are `ObjectId`s — core-owned,
   fixed-width, and needed by the *reconstruction order* before any blob is parsed. A
   topological walk must read them without decoding a kind's params text.
6. **The rebind seam ships even though Rule 4 mostly obviates it.** Three cases
   outlive reconstruction: a plugin absent this session, a kind with no codec, and a
   file written before this task. All three are permanent shapes, not transitional.

## Open questions

(none — all decided)

## Status

_pending implementation_
