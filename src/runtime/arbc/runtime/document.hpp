#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp> // TimeMap (set_layer_time_map)
#include <arbc/base/time.hpp>          // TimeRange (set_layer_span)
#include <arbc/base/transform.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/audio_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/model/journal.hpp>
#include <arbc/model/model.hpp>
#include <arbc/runtime/editable_binding.hpp>
#include <arbc/serialize/unknown_fields.hpp> // names no JSON type (doc 08:61-63)

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace arbc {

// Host-facing document: the versioned model, the document-wide history, and the
// content binding. Records hold opaque content ids; the id-to-Content binding
// lives here, keeping the model free of the contract vtable (doc 17).
class Document {
public:
  Document();

  // Mint a versioned content object: commits a `Transaction::add_content(kind)`,
  // publishing a `ContentRecord` (opaque `kind` id + inert `StateHandle`) into a
  // pinnable `DocState` (revision +1), and binds the returned record id to
  // `content` in the runtime side-map. `kind` is an opaque caller-supplied token
  // (the reverse-DNS<->numeric bridge is `runtime.document_serialize`'s); it
  // defaults to 0. `resolve(id)` serves the vtable binding, unchanged.
  //
  // If the content exposes the `Editable` facet (doc 03:98), its state sinks are
  // registered onto this document's live `Model`/`Journal` here -- so its edits
  // are journaled and undoable, its undo memory is budgeted by `state_cost`, and
  // a published version pins its state until the record is reclaimed
  // (doc 14:173-176) -- and the minted record embeds the content's CAPTURED
  // initial state rather than the inert handle `model.content_binding` left behind
  // (still one published version). Non-editable content registers nothing and
  // keeps the inert record, byte-identical to before.
  //
  // A document may hold ANY NUMBER of editable contents: the document's one sink
  // trio routes every retain/release/cost/restore by the owning `ObjectId` that
  // now rides each state seam (`EditableBinding`). The v1 one-per-document limit,
  // which used to throw here, is gone (`runtime.editable_sink_multiplex`).
  ObjectId add_content(std::shared_ptr<Content> content, std::uint64_t kind = 0);
  ObjectId add_layer(ObjectId content, const Affine& transform, double opacity = 1.0);
  void set_layer_transform(ObjectId layer, const Affine& transform);

  // Configure a layer's temporal placement (doc 11:59-73): the half-open parent-
  // time span `[in, out)` the layer exists over, and the 1D affine time map from
  // parent time to content-local time. Host-facing wrappers over the model's
  // transactional `set_span`/`set_time_map`, mirroring `set_layer_transform`; each
  // commits its own version and bumps the revision. The offline/interactive render
  // drivers read these off the pinned layer record for span-cull + retiming.
  void set_layer_span(ObjectId layer, const TimeRange& span);
  void set_layer_time_map(ObjectId layer, const TimeMap& time_map);

  // Attach `layer` at the top of `composition`'s ordered membership (doc 14): the
  // host-facing wrapper over the model's transactional attach, mirroring
  // `set_layer_transform`. The audio mix drives composition membership
  // (`mix_composition` walks `for_each_layer_in`, `mix.hpp:59`), so a composition an
  // export monitor mixes has its layers attached here. Commits its own version.
  void attach_layer(ObjectId composition, ObjectId layer);

  // Configure a layer's audio placement (doc 12:89-92): the additive-mix `gain`
  // (analog of opacity, not clamped at 1) and the `audible` flag (analog of
  // `visible`). Host-facing wrappers over the model's transactional
  // `set_gain`/`set_audible`, the audio siblings of `set_layer_transform`. Each
  // commits its own version.
  void set_layer_gain(ObjectId layer, double gain);
  void set_layer_audible(ObjectId layer, bool audible);

  // Configure a composition's working audio format (doc 12:94-104): the working
  // sample rate + channel layout the mix is produced at, the audio twin of
  // `set_working_space`. Committed as its own version, bumping the revision.
  void set_working_audio_format(ObjectId composition, const AudioFormat& format);

  // Insert a composition (doc 07 rule 2: the unit that owns a working space). Its
  // working space defaults to the doc 07 walking-skeleton format; the render
  // drivers read it from the pinned state for target/temp allocation.
  ObjectId add_composition(double canvas_w, double canvas_h);
  // Configure a composition's working space -- the `SurfaceFormat` the compositor
  // blends it in (doc 07). Committed as its own version, bumping the revision.
  void set_working_space(ObjectId composition, const SurfaceFormat& format);

  // Open a transaction on the document's model -- the host-facing edit seam an
  // editable content's mutators ride (doc 03:152-158: mutation through the
  // concrete type, under transactional discipline; `RasterContent::paint` takes
  // the `Transaction&`). Commits publish one version and, through the document's
  // `CommitSink`, append one journal entry, so a content edit made this way is
  // undoable without the host wiring anything. WRITER-THREAD ONLY.
  Model::Transaction transact(std::string name = {});

  // The document-wide history (doc 14:193-195 -- one journal across all objects).
  // Core-owned: it is this document's `CommitSink`, and an editable content's
  // cost/restore sinks are registered onto it by `add_content`. `undo()`/`redo()`
  // publish ordinary forward versions. WRITER-THREAD ONLY.
  Journal& journal() noexcept { return d_journal; }
  const Journal& journal() const noexcept { return d_journal; }

  // Run deferred reclamation to quiescence (the model's drain, doc 15:129-136):
  // superseded records whose last reference is gone are reclaimed, releasing any
  // content state they pinned. WRITER-ONLY / single-drainer.
  void drain();

  // Pin the current version for rendering (doc 14).
  DocStatePtr pin() const;
  Content* resolve(ObjectId content) const;

  // Visit every minted top-level content (the runtime operator binder walks these
  // plus each content's `inputs()` to reach operator input children,
  // `operator_binding.cpp`). Read-only over the side-map; `fn` receives the borrowed
  // `Content*` (never null). The `Content` vtable stays in `runtime`, doc 17:66-72.
  void for_each_content(const std::function<void(Content*)>& fn) const;

  // The same walk with each content's `ObjectId` in hand -- the key its unknown-field
  // stash lives under (serialize.unknown_field_preservation Decision 3). The save path
  // needs it for operator INPUT CHILDREN, which carry no `ContentRecord` after
  // demote-after-sink yet keep their entry in this map, so their id is still known here.
  void for_each_content(const std::function<void(ObjectId, Content*)>& fn) const;

  // The document's every-tier unknown-field stash (doc 08 Principle 4): the fields a
  // load preserved verbatim because this build's core does not name them, keyed by
  // `(scope, ObjectId)`. Writer-thread-owned exactly like the content side-map, and
  // COPIED into a `ContentSnapshot` by `capture_snapshot` so an off-thread save never
  // reads live editor state (Constraint 9). Replaced wholesale by a successful load;
  // empty for a document built programmatically, which is why an unknown-free document
  // serializes byte-identically to before.
  const UnknownFieldStore& unknown_fields() const noexcept { return d_unknown; }

  // The document's editable-state multiplexer, for the behavioral counters doc 16
  // asks the editable tests to assert: `unrouted_state_calls()` (zero in any
  // correct document -- a state call that could not reach its owner would free the
  // wrong content's pixels) and `seam_registrations()` (three, however many
  // editable contents the document holds). Const: the mutating half is the
  // writer's, driven from `add_content`.
  const EditableBinding& editable_binding() const noexcept { return d_binding; }

private:
  // The runtime load façade (`runtime.document_serialize`) installs a reconstructed
  // graph into `d_model` through the serialize reader, which needs the mutable
  // `Model`. Granted through an attorney-client accessor (defined in
  // `document_serialize.cpp`) rather than a public method, so `Document`'s public
  // shape and member set stay unchanged (refinement Constraint 4).
  friend struct DocumentSerializeAccess;

  // DECLARATION ORDER IS THE TEARDOWN CONTRACT (destruction runs in reverse).
  //
  // `~Model` drops the current version and drains, which reclaims every content
  // record at zero count and, through `ContentStateReclaimSink`, RELEASES each
  // record's embedded `StateHandle` through the installed `StateRefSink` -- that
  // is what makes "release fires exactly once, when the record is reclaimed"
  // (doc 14:173-176) true at document teardown rather than a leak. So both the
  // sink objects (`d_binding`) and the content they forward into (`d_contents`)
  // MUST still be alive while the model destructs, and the journal -- whose
  // entries hold record edges -- must go first so those records can reach zero.
  //
  // Reverse-declaration destruction gives exactly that: journal, model, binding,
  // contents. Do not reorder these members, and do not clear the sink slots in a
  // `~Document` body: clearing them before the model's final drain would silence
  // the release and strand the content's version refcount at 1. (The explicit,
  // per-content teardown is `EditableBinding::unbind()`, which drains first.)
  // The permanent, levelization-mandated home of the id->Content vtable binding
  // (doc 17:66-72): the versioned `ContentRecord` in `DocState` holds only the
  // opaque `{kind, StateHandle}`, so the model stays free of the `Content` vtable.
  // Writer-thread-owned; keyed by the record's `ObjectId`; `resolve()` reads it.
  std::unordered_map<ObjectId, std::shared_ptr<Content>> d_contents;
  // The document's one state-sink trio, routing each seam call to the `Editable`
  // facet of the content that owns the handle.
  EditableBinding d_binding;
  Model d_model;
  Journal d_journal{d_model};
  // The unknown-field stash (doc 08 Principle 4). Deliberately declared AFTER the
  // teardown-contract members above: it is plain maps of `std::string`, owns no record
  // edge and no state handle, so its destruction order is immaterial and it perturbs
  // nothing the contract above pins down.
  UnknownFieldStore d_unknown;
};

} // namespace arbc
