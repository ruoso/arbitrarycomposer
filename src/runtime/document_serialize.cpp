// runtime.document_serialize: the L5 glue binding a `runtime::Document` to the
// serialize content seams (docs 08, 17). Holds the runtime-owned `KindBridge`, the
// built-in `CodecTable` assembly, the pinned-snapshot save path, and the
// `ContentSink` load path. This is a runtime TU that links nlohmann::json PRIVATE
// (through the internal `codec.hpp`, Decision 3); the runtime PUBLIC headers name no
// JSON type (Constraint 7).

#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/damage.hpp> // Damage, damage_add (the arrival's own damage)
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document_serialize.hpp>
#include <arbc/runtime/external_asset_loader.hpp>
#include <arbc/runtime/external_composition_loader.hpp>
#include <arbc/runtime/raster_tile_store.hpp>
#include <arbc/serialize/codec.hpp>       // CodecTable (internal; names nlohmann::json)
#include <arbc/serialize/deserialize.hpp> // DeserializeFn
#include <arbc/serialize/load_context.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arbc {

// Attorney-client accessor (see `document.hpp`): the sole seam by which the load
// façade reaches `Document`'s internal `Model` to install the reconstructed
// version-0 baseline, without widening `Document`'s public shape (Constraint 4).
struct DocumentSerializeAccess {
  static Model& model(Document& doc) { return *doc.d_model; }
  static Journal& journal(Document& doc) { return doc.d_journal; }
  // The mutable half of `Document::unknown_fields()`: the load path hands this to the
  // serialize reader, which replaces it wholesale on a successful load (doc 08
  // Principle 4). Writer-thread-only, exactly like the content side-map.
  static UnknownFieldStore& unknown(Document& doc) { return doc.d_unknown; }
  // The load state that outlives one load (runtime.async_external_load Decision 2). The
  // `shared_ptr` itself is what the loader takes, because the `on_ready` closures it hands
  // to the `AssetSource` capture a `weak_ptr` into it (Constraint 6).
  static const std::shared_ptr<PendingExternalLoads>& pending(Document& doc) {
    return doc.d_pending_loads;
  }
};

namespace {

// A load is a fresh document at version 0 with an EMPTY journal (doc 14:263-264;
// serialize.reader Decision 3: "an undo immediately after load is a no-op that
// never reverts the freshly-loaded document to empty"). `Model::load_baseline`
// already bypasses the `CommitSink` for the baseline publish itself -- but the
// reconstruction commits that PRECEDE it (each `add_content`, each demotion of a
// provisional root) go through the ordinary transactional path and would
// otherwise land in the document's journal as undoable edits. Detach the commit
// sink for the reconstruction and restore it after, so a load journals nothing.
class JournalSuspension {
public:
  explicit JournalSuspension(Document& doc) noexcept : d_doc(&doc) {
    DocumentSerializeAccess::model(doc).set_commit_sink(nullptr);
  }
  ~JournalSuspension() {
    DocumentSerializeAccess::model(*d_doc).set_commit_sink(
        &DocumentSerializeAccess::journal(*d_doc));
  }
  JournalSuspension(const JournalSuspension&) = delete;
  JournalSuspension& operator=(const JournalSuspension&) = delete;

private:
  Document* d_doc;
};

// Recover a live content's built-in `(kind_id, kind_version)` from its concrete type
// (runtime.operator_codecs Decision 5). An operator input child carries no ObjectId,
// so the ObjectId-keyed `ContentRecord` cannot resolve its kind; the `const Content&`-
// keyed reverse map is re-derived per save by walking `Content::inputs()` and matching
// the live pointer here (runtime is the only layer that sees every concrete built-in
// kind, doc 17). A content of no built-in kind (a `PlaceholderContent`, or a
// not-yet-built-in kind) yields `false`, so the meta provider returns `nullopt` and its
// stored body re-emits verbatim (doc 08 Principle 2). Cheap to re-derive, so no
// persistent `Content*`->kind map is kept (Decision 5, rejected alternative).
bool builtin_kind_of(const Content& c, std::string_view& kind_id, std::string_view& kind_version) {
  if (dynamic_cast<const SolidContent*>(&c) != nullptr) {
    kind_id = SolidContent::kind_id;
    kind_version = k_solid_kind_version;
    return true;
  }
  if (dynamic_cast<const ToneContent*>(&c) != nullptr) {
    kind_id = ToneContent::kind_id;
    kind_version = k_tone_kind_version;
    return true;
  }
  if (dynamic_cast<const FadeContent*>(&c) != nullptr) {
    kind_id = FadeContent::kind_id;
    kind_version = k_fade_kind_version;
    return true;
  }
  if (dynamic_cast<const CrossfadeContent*>(&c) != nullptr) {
    kind_id = CrossfadeContent::kind_id;
    kind_version = k_crossfade_kind_version;
    return true;
  }
  if (dynamic_cast<const NestedContent*>(&c) != nullptr) {
    kind_id = NestedContent::kind_id;
    kind_version = k_nested_kind_version;
    return true;
  }
  if (dynamic_cast<const RasterContent*>(&c) != nullptr) {
    kind_id = RasterContent::kind_id;
    kind_version = k_raster_kind_version;
    return true;
  }
  return false;
}

} // namespace

// ---- KindBridge -------------------------------------------------------------

KindBridge::KindBridge() {
  // Pre-intern the built-in kinds so their tokens are stable from construction
  // (Decision 1). Assignment order is free -- only the strings are serialized.
  intern(SolidContent::kind_id, k_solid_kind_version);
  intern(ToneContent::kind_id, k_tone_kind_version);
  intern(FadeContent::kind_id, k_fade_kind_version);
  intern(CrossfadeContent::kind_id, k_crossfade_kind_version);
  intern(NestedContent::kind_id, k_nested_kind_version);
  intern(RasterContent::kind_id, k_raster_kind_version);
}

std::uint64_t KindBridge::intern(std::string_view kind_id, std::string_view kind_version) {
  std::string key(kind_id);
  if (const auto it = d_by_id.find(key); it != d_by_id.end()) {
    return it->second; // first-seen wins; the token (and its version) is retained
  }
  d_entries.push_back(Entry{std::string(kind_id), std::string(kind_version)});
  const auto token = static_cast<std::uint64_t>(d_entries.size()); // 1-based; 0 == k_unknown_kind
  d_by_id.emplace(std::move(key), token);
  return token;
}

bool KindBridge::lookup(std::uint64_t token, std::string_view& kind_id,
                        std::string_view& kind_version) const {
  if (token == k_unknown_kind || token > d_entries.size()) {
    return false;
  }
  const Entry& e = d_entries[static_cast<std::size_t>(token - 1)];
  kind_id = e.kind_id;
  kind_version = e.kind_version;
  return true;
}

// ---- Built-in codec table ---------------------------------------------------

CodecTable builtin_codecs() {
  CodecTable table;
  table.add(SolidContent::kind_id, solid_codec());
  table.add(ToneContent::kind_id, tone_codec());
  table.add(FadeContent::kind_id, fade_codec());
  table.add(CrossfadeContent::kind_id, crossfade_codec());
  table.add(NestedContent::kind_id, nested_codec());
  // Correct but NON-MEMOIZING (serialize.raster_tile_store Decision 5): it hashes every
  // tile on every save. Deliberate -- every existing sink-less call site keeps working and
  // still saves correct pixels; it just does not get the incremental CPU win. A host that
  // owns a `RasterTileStore` should save through `builtin_codecs(registry, &tiles)`.
  table.add(RasterContent::kind_id, raster_codec());
  return table;
}

CodecTable builtin_codecs(const Registry& registry, RasterTileStore* tiles) {
  CodecTable table = builtin_codecs(registry);
  table.add(RasterContent::kind_id, raster_codec(tiles));
  return table;
}

CodecTable builtin_codecs(const Registry& registry, RasterTileStore* tiles,
                          TileEncodeDispatch* dispatch) {
  // The parallel-save table (serialize.tile_store_parallel_save): the raster codec fans its
  // per-tile encode across `dispatch`'s executor. A null `dispatch` degenerates to the
  // inline save, byte-identical to `builtin_codecs(registry, tiles)`.
  CodecTable table = builtin_codecs(registry);
  table.add(RasterContent::kind_id, raster_codec(tiles, dispatch));
  return table;
}

CodecTable builtin_codecs(const Registry& registry) {
  CodecTable table = builtin_codecs();
  // The OUT-OF-LIB kinds' codecs, each GATED ON ITS PLUGIN BEING LOADED (kinds.image
  // Decision 2). The witness is the `Registry`: a registered factory for the kind IS "the
  // plugin is present". With no plugin there is no codec, and the layer round-trips verbatim
  // as a `PlaceholderContent` through the ordinary `CodecTable::find`-miss path -- a missing
  // plugin must never destroy data (doc 08 Principles 2/4).
  if (registry.factory(k_image_kind_id) != nullptr) {
    table.add(k_image_kind_id, image_codec(registry));
  }
  return table;
}

// ---- Save path --------------------------------------------------------------

ContentSnapshot capture_snapshot(const Document& doc, const KindBridge& bridge) {
  // MUST run on the writer thread: `doc.resolve` reads the writer-thread-owned
  // content side-map. We pin the version and copy each layer-bound content's live
  // pointer + bridged kind into an immutable structure the emit reads off-thread
  // (Decision 6).
  ContentSnapshot snap;
  snap.state = doc.pin();
  // COPY the unknown-field stash off the live document (Decision 6): the off-thread emit
  // reads only the snapshot, never live editor state (Constraint 9).
  snap.unknown = doc.unknown_fields();
  ObjectId comp_id;
  const CompositionRecord* comp = nullptr;
  if (!snap.state || !snap.state->find_first_composition(comp_id, comp)) {
    return snap;
  }

  // Every content's ObjectId, including the operator INPUT CHILDREN that demote-after-sink
  // stripped of their `ContentRecord` -- their entry in the document's content side-map
  // survives demotion, so this is the one place that knows the id keying each content's
  // unknown-field stash (Decision 3). Read on the writer thread, like `doc.resolve`.
  std::unordered_map<const Content*, ObjectId> ids;
  doc.for_each_content([&ids](ObjectId id, Content* c) { ids.emplace(c, id); });

  // The document is a GRAPH of compositions (doc 08 Principle 7,
  // serialize.compositions_table): the walk spans every composition reachable from the
  // root through a nesting content's `composition_ref()`, not just the lowest-id one, so
  // a multi-composition `Document` round-trips through the L5 sinks. Breadth-first over
  // compositions, and the visited set makes a Droste cycle terminate. Kind-agnostic: an
  // unknown-kind `PlaceholderContent` carries its child reference too, so a missing
  // plugin never orphans the composition it embeds. Writer-thread only, like the rest of
  // this function (Constraint 11): the off-thread emit reads only the finished snapshot.
  // Returns whether `cid` names a REACHABLE composition -- the same predicate the writer's
  // `ContentGraph::enqueue_composition` answers, because the two walks must agree on which
  // contents are nesting contents (below).
  std::vector<ObjectId> comp_queue;
  std::unordered_set<ObjectId> comp_seen;
  const auto enqueue_composition = [&](ObjectId cid) -> bool {
    if (!cid.valid() || snap.state->find_composition(cid) == nullptr) {
      return false;
    }
    if (comp_seen.insert(cid).second) {
      comp_queue.push_back(cid);
    }
    return true;
  };
  static_cast<void>(enqueue_composition(comp_id));

  // Extend the reverse map to the operator INPUT CHILDREN (runtime.operator_codecs
  // Decision 5): the layer walk captures only layer-root contents (bound by a
  // `ContentRecord`, keyed into `by_id`). An operator's input children have no
  // ObjectId, so the writer resolves each child's `(kind, kind_version)` through the
  // `const Content&`-keyed meta provider instead -- backed here by a transitive walk
  // of `Content::inputs()` over the pinned graph, interning each reachable built-in
  // child into `by_ptr` (a meta entry, no `by_id`/body entry). A `PlaceholderContent`
  // (or any non-built-in) child gets no entry -> meta `nullopt` -> its stored body
  // re-emits verbatim (doc 08 Principle 2); its own inputs are still descended so deeper
  // built-in nodes are captured. v1 `$ref` graphs are acyclic DAGs, and `walked` guards
  // shared re-encounters (sharing Decision 8). Every content reached -- layer root or
  // input child -- also contributes its `composition_ref()` to the composition queue.
  std::vector<const Content*> frontier;
  std::unordered_set<const Content*> walked;
  std::size_t next_comp = 0;
  while (next_comp < comp_queue.size() || !frontier.empty()) {
    if (next_comp < comp_queue.size()) {
      const ObjectId walking = comp_queue[next_comp++];
      snap.state->for_each_layer_in(walking, [&](ObjectId lid) {
        const LayerRecord* lr = snap.state->find_layer(lid);
        if (lr == nullptr) {
          return;
        }
        const ObjectId cid = lr->content;
        if (!cid.valid() || snap.by_id.find(cid) != snap.by_id.end()) {
          return; // placement-only layer, or a shared content already captured
        }
        const Content* c = doc.resolve(cid);
        if (c == nullptr) {
          return;
        }
        const ContentRecord* rec = snap.state->find_content(cid);
        const std::uint64_t kind = (rec != nullptr) ? rec->kind : KindBridge::k_unknown_kind;
        std::string_view kind_id;
        std::string_view kind_version;
        // unknown -> empty views (the placeholder's own stored body wins)
        bridge.lookup(kind, kind_id, kind_version);
        const std::size_t idx = snap.entries.size();
        snap.entries.push_back(
            ContentSnapshot::Entry{c, std::string(kind_id), std::string(kind_version), cid});
        snap.by_id.emplace(cid, idx);
        snap.by_ptr.emplace(c, idx);
        if (walked.insert(c).second) {
          frontier.push_back(c);
        }
      });
      continue;
    }
    const Content* const c = frontier.back();
    frontier.pop_back();
    // A content naming a resolvable child composition has no AUTHORED inputs: its
    // `inputs()` are a projection of that child's layers, which the composition queue above
    // already walks in full (doc 08 Principle 7's closing rule). Stop here, exactly as the
    // writer's `ContentGraph::visit` does -- the reverse map this walk builds feeds that
    // traversal, so the two must skip the same edges or the map would carry meta entries
    // for contents the writer never emits. It is also what keeps this walk from reading the
    // memo `NestedContent::attach` mutates on the render thread (Constraint 6).
    //
    // An EXTERNAL child is stopped at BEFORE either edge, exactly as the writer's
    // `ContentGraph::visit` stops (doc 08 Principle 3): its composition lives in THIS
    // model but is the other document's data, so enqueueing it would capture the other
    // document's contents into this snapshot and the emit would inline them -- destroying
    // the very reference this content exists to hold. The two walks must skip the same
    // edges or the reverse map would carry meta entries for contents the writer never
    // emits.
    if (!c->external_composition_ref().empty()) {
      continue;
    }
    if (enqueue_composition(c->composition_ref())) {
      continue;
    }
    for (const ContentRef child : c->inputs()) {
      if (child == nullptr || !walked.insert(child).second) {
        continue; // null slot, or a shared child already reached
      }
      frontier.push_back(child); // descend regardless of known kind
      if (snap.by_ptr.find(child) != snap.by_ptr.end()) {
        continue; // already captured as a layer root (shared root+input)
      }
      std::string_view child_kind_id;
      std::string_view child_kind_version;
      if (!builtin_kind_of(*child, child_kind_id, child_kind_version)) {
        continue; // unknown/placeholder child -> meta nullopt -> verbatim re-emit
      }
      const auto idit = ids.find(child);
      const ObjectId child_id = (idit != ids.end()) ? idit->second : ObjectId{};
      const std::size_t cidx = snap.entries.size();
      snap.entries.push_back(ContentSnapshot::Entry{child, std::string(child_kind_id),
                                                    std::string(child_kind_version), child_id});
      snap.by_ptr.emplace(child, cidx);
    }
  }
  return snap;
}

expected<std::string, SerializeError>
serialize_snapshot(const ContentSnapshot& snapshot, const CodecTable& codecs, SaveContext& ctx) {
  // The provider resolves a content by its ObjectId off the pinned record's kind, so
  // the serialized kind reflects the pinned version (Constraint 2,
  // 08-serialization#writer-serializes-the-pinned-version). The meta provider keys by
  // `const Content&` for input-child kinds (unused by leaf kinds, but the seam the
  // writer requires). Both read only the immutable snapshot.
  const ContentBodyProvider provider = [&snapshot](ObjectId id) -> std::optional<ContentBody> {
    const auto it = snapshot.by_id.find(id);
    if (it == snapshot.by_id.end()) {
      return std::nullopt;
    }
    const ContentSnapshot::Entry& e = snapshot.entries[it->second];
    return ContentBody{e.kind_id, e.kind_version, *e.content};
  };
  const ContentMetaProvider meta = [&snapshot](const Content& c) -> std::optional<ContentMeta> {
    const auto it = snapshot.by_ptr.find(&c);
    if (it == snapshot.by_ptr.end()) {
      return std::nullopt;
    }
    const ContentSnapshot::Entry& e = snapshot.entries[it->second];
    return ContentMeta{e.kind_id, e.kind_version, e.id};
  };
  // The unknown-field stash rides the SNAPSHOT's copy, never the live document's, so this
  // stays a pure read of immutable state (Decision 6, Constraint 9).
  return serialize_document(*snapshot.state, provider, meta, codecs, ctx, &snapshot.unknown);
}

expected<std::string, SerializeError> serialize_snapshot(const ContentSnapshot& snapshot,
                                                         const CodecTable& codecs) {
  SaveContext ctx;
  return serialize_snapshot(snapshot, codecs, ctx);
}

expected<std::string, SerializeError> save_document(const Document& doc, const KindBridge& bridge,
                                                    const CodecTable& codecs, SaveContext& ctx) {
  return serialize_snapshot(capture_snapshot(doc, bridge), codecs, ctx);
}

expected<std::string, SerializeError> save_document(const Document& doc, const KindBridge& bridge,
                                                    const CodecTable& codecs) {
  // A sink-less context seeded from the document's own storage format. Every pre-existing
  // call site lands here and is byte-identical -- and a document that DOES hold a raster
  // layer fails loudly with `AssetSinkMissing` rather than silently dropping its pixels
  // (serialize.raster_tile_store Constraint 5).
  SaveContext ctx;
  ctx.set_storage_format(doc.storage_format());
  return save_document(doc, bridge, codecs, ctx);
}

expected<std::string, SerializeError> save_document(const Document& doc, const KindBridge& bridge) {
  const CodecTable codecs = builtin_codecs();
  return save_document(doc, bridge, codecs);
}

// ---- Load path --------------------------------------------------------------

namespace {

// The per-load session (Decision 4): each built-in codec's deserialize wrapper
// records `live Content* -> interned kind uint64` here after constructing its node,
// so the sink can stamp the right kind into the `ContentRecord` without widening the
// Done `ContentSink` signature. A PlaceholderContent (no wrapper runs) is absent, so
// the sink falls back to `k_unknown_kind`. Transient: discarded after the load.
using LoadSession = std::unordered_map<const Content*, std::uint64_t>;

// Wrap a built-in `deserialize` so a successful construction records its interned
// kind into the session. The base deserialize owns `params`/`inputs`/`ctx`.
DeserializeFn recording_deserialize(DeserializeFn base, std::string_view kind_id,
                                    std::string_view kind_version, LoadSession& session,
                                    KindBridge& bridge) {
  return [base = std::move(base), kind = std::string(kind_id), version = std::string(kind_version),
          &session, &bridge](const nlohmann::json& params, std::span<const ContentRef> inputs,
                             ObjectId composition,
                             LoadContext& ctx) -> expected<std::unique_ptr<Content>, ReaderError> {
    expected<std::unique_ptr<Content>, ReaderError> produced =
        base(params, inputs, composition, ctx);
    if (produced) {
      session[(*produced).get()] = bridge.intern(kind, version);
    }
    return produced;
  };
}

// The whole load-scoped assembly, in one place (runtime.async_external_load Decision 2). It
// used to be four stack locals of `load_document`; a deferring `AssetSource` fires its
// `on_ready` after all four are destroyed, so a SETTLE has to rebuild them. Rebuilding beats
// keeping them alive: the `CodecTable` and the `ContentSink` are load-scoped by nature, and
// parking them on the `Document` would make the loader the mutable, half-initialized shared
// service its own header says it is not. What is genuinely durable -- the resolved-identity
// map, the pending entries, the completion queue -- is the `PendingExternalLoads` the
// `Document` owns, which this borrows.
//
// One class, both callers, so the two paths cannot drift: a settle parses a child's bytes
// through exactly the codecs, exactly the sink, and exactly the loader a load would have.
class LoadAssembly {
public:
  LoadAssembly(Document& doc, KindBridge& bridge, const Registry& registry, RasterTileStore* tiles,
               TileDecodeDispatch* decode)
      : d_assets(DocumentSerializeAccess::model(doc), DocumentSerializeAccess::pending(doc)),
        d_sink(make_sink(doc)), d_loader(DocumentSerializeAccess::model(doc), registry, d_codecs,
                                         d_sink, DocumentSerializeAccess::pending(doc)) {
    // The codec table and the loader are mutually referential and that is intentional
    // (nested_external_ref Constraint 3): the loader reads the table when it recurses into a
    // child document, and the nested codec's closure holds the loader. The table is filled
    // AFTER the loader is constructed over it -- it is only ever read at call time.
    Codec solid = solid_codec();
    d_codecs.add(
        SolidContent::kind_id,
        Codec{solid.serialize, recording_deserialize(solid.deserialize, SolidContent::kind_id,
                                                     k_solid_kind_version, d_session, bridge)});
    Codec tone = tone_codec();
    d_codecs.add(
        ToneContent::kind_id,
        Codec{tone.serialize, recording_deserialize(tone.deserialize, ToneContent::kind_id,
                                                    k_tone_kind_version, d_session, bridge)});
    Codec fade = fade_codec();
    d_codecs.add(
        FadeContent::kind_id,
        Codec{fade.serialize, recording_deserialize(fade.deserialize, FadeContent::kind_id,
                                                    k_fade_kind_version, d_session, bridge)});
    Codec crossfade = crossfade_codec();
    d_codecs.add(CrossfadeContent::kind_id,
                 Codec{crossfade.serialize,
                       recording_deserialize(crossfade.deserialize, CrossfadeContent::kind_id,
                                             k_crossfade_kind_version, d_session, bridge)});
    // The one codec that takes the loader: nested is the only kind that names a child
    // composition, so it is the only one with an external reference to resolve.
    Codec nested = nested_codec(&d_loader);
    d_codecs.add(
        NestedContent::kind_id,
        Codec{nested.serialize, recording_deserialize(nested.deserialize, NestedContent::kind_id,
                                                      k_nested_kind_version, d_session, bridge)});
    // The out-of-lib kinds' codecs, gated on their plugin being loaded (kinds.image
    // Decision 2) -- the `Registry` is the plugin-present witness, exactly as it already is
    // for the placeholder path. Ungated, an image body would deserialize through a codec
    // whose factory does not exist; gated, it falls through to `PlaceholderContent` and the
    // authored URI survives untouched. `org.arbc.image` is a LEAF -- it names no child
    // composition, so it takes no COMPOSITION loader and adopts no inputs -- but it does
    // reference an external ASSET, and `d_assets` is the loader that gives that reference its
    // third outcome (kinds.image_async_pending Decision 2).
    if (registry.factory(k_image_kind_id) != nullptr) {
      Codec image = image_codec(registry, &d_assets);
      d_codecs.add(
          k_image_kind_id,
          Codec{image.serialize, recording_deserialize(image.deserialize, k_image_kind_id,
                                                       k_image_kind_version, d_session, bridge)});
    }
    // The hash memo rides the LOAD path too (Decision 5): every tile the reader decodes has
    // a name it already knows, so seeding the memo here is what makes the first save after a
    // load -- and the reader's own params-only residual re-serialize -- a pure memo sweep
    // rather than a full re-hash of the document. `decode` (serialize.tile_store_parallel_load)
    // fans the per-tile decode across pool workers when a host supplies it; null loads inline.
    Codec raster = raster_codec(tiles, decode);
    d_codecs.add(
        RasterContent::kind_id,
        Codec{raster.serialize, recording_deserialize(raster.deserialize, RasterContent::kind_id,
                                                      k_raster_kind_version, d_session, bridge)});
  }

  LoadAssembly(const LoadAssembly&) = delete;
  LoadAssembly& operator=(const LoadAssembly&) = delete;

  const CodecTable& codecs() const noexcept { return d_codecs; }
  const ContentSink& sink() const noexcept { return d_sink; }
  ExternalCompositionLoader& loader() noexcept { return d_loader; }
  ExternalAssetLoader& assets() noexcept { return d_assets; }

private:
  // The sink that binds each reconstructed content into the document. Provisional-root
  // records are keyed by live pointer: a node is minted as a layer root when first sunk, then
  // DEMOTED to an owned-only input child (its `ContentRecord` removed, its object kept alive
  // in `d_contents`) the moment a later-sunk parent operator lists it in `inputs()`
  // (runtime.operator_codecs Decision 4). The read recursion sinks children bottom-up BEFORE
  // their parent and binds a layer to the OUTERMOST (last) sunk node, threading inner nodes
  // only by live `Content*` (their `.id` is discarded, reader.cpp) -- so demoting an inner
  // node's record is invisible to the graph wiring, and only true layer roots keep a record
  // (find_content surfaces roots alone). The model stores no input edges (records.hpp), so a
  // child needs no ObjectId. On the LOAD path every intermediate transaction's revision is
  // reset by `load_baseline`'s revision-0 publish; on the SETTLE path they are ordinary
  // forward revisions, exactly as any other structural edit is.
  ContentSink make_sink(Document& doc) {
    return [this, &doc](std::unique_ptr<Content> c) -> SunkContent {
      Content* const live = c.get();
      // Any input this node adopts is now known to be an input child, not a layer root:
      // drop its provisional record (the object stays owned by `d_contents`).
      for (const ContentRef child : live->inputs()) {
        const auto mit = d_minted.find(child);
        if (mit == d_minted.end()) {
          continue;
        }
        Model& model = DocumentSerializeAccess::model(doc);
        Model::Transaction txn = model.transact();
        txn.remove(mit->second);
        static_cast<void>(txn.commit());
        d_minted.erase(mit);
      }
      const auto it = d_session.find(live);
      const std::uint64_t kind = (it != d_session.end()) ? it->second : KindBridge::k_unknown_kind;
      const ObjectId id = doc.add_content(std::shared_ptr<Content>(std::move(c)), kind);
      d_minted.emplace(live, id);
      // The one point where MINTING meets IDENTITY (kinds.image_async_pending Decision 3). A
      // content whose asset is still in flight was registered by the codec against the raw
      // pointer it had -- the only key available before an id exists; here, and only here, the
      // real `ObjectId` is known, so it is bound onto the durable pending entry. That id is
      // what the arrival installs into and what its damage names. A no-op for every content
      // that awaits nothing, which is every content but a pending image.
      d_assets.bind(live, id);
      return SunkContent{id, live};
    };
  }

  // DECLARATION ORDER IS THE CONSTRUCTION CONTRACT: the sink closes over the session, the
  // minted map and the asset loader, and the composition loader binds the (still-empty) codec
  // table by reference.
  LoadSession d_session;
  std::unordered_map<const Content*, ObjectId> d_minted;
  CodecTable d_codecs;
  ExternalAssetLoader d_assets;
  ContentSink d_sink;
  ExternalCompositionLoader d_loader;
};

// The damage one arrival owes: whole-object, all-time, on every content EMBEDDING the child
// that just landed (Decision 3). Kind-agnostic -- it asks `Content::composition_ref()`, the
// contract-level accessor, so a third-party nesting kind is damaged on the same terms as
// `org.arbc.nested` -- and it naturally damages BOTH parents when two contents share one
// external child. `Rect::infinite()` / `TimeRange::all()` are the absorbing shape (damage.hpp
// :64-73): a child appearing where there was a placeholder changes the parent everywhere, at
// every instant.
//
// Computed BEFORE the install, which is fine and in fact necessary: the embedding contents
// were sunk by the parent load (or by an earlier settle round) and already hold the
// pre-allocated child id. It is the composition RECORD under that id that does not exist yet.
std::vector<Damage> arrival_damage(const Document& doc, ObjectId child) {
  std::vector<Damage> out;
  doc.for_each_content([&out, child](ObjectId id, Content* c) {
    if (c->composition_ref() == child) {
      damage_add(out, Damage{id, Rect::infinite(), TimeRange::all()});
    }
  });
  return out;
}

// An ASSET arrival: the bytes behind an `org.arbc.image`'s `params.source`, landing after the
// load that asked for them finished (kinds.image_async_pending).
//
// Asked FIRST, before the composition arm, because one queue carries both arrival kinds and
// `take_pending_asset` is the discriminant: an id with no asset entry is a composition child
// (or a duplicate arrival, which finds nothing pending and installs nothing -- the same
// idempotence `ExternalCompositionLoader::settle` already relies on).
//
// Unlike a composition arrival, this mutates NO model state: the decoded pyramid is
// plugin-owned and deliberately unversioned (`kinds.image` Decision 4 -- no CoW, no
// `StateHandle`, no pool backing). So the model-side arrival is one DAMAGE-ONLY transaction
// (Decision 5). `Transaction::commit()` publishes unconditionally -- one atomic store, one
// revision increment, then exactly one damage flush -- so that is precisely "one new revision
// carrying the reason to re-render", which is what doc 02 asks of every publish. Both halves
// are needed and one commit gives both: the DAMAGE wakes the frame loop (doc 02:50-51, "no
// damage -> no work"), and the REVISION is what stops the parent composition's composed-result
// tiles from being served as stale hits (doc 02:255-284).
//
// The install happens BEFORE the commit. Under `HostViewport::step()` the settle runs at step
// 0, before the pin, so no worker of the current frame can straddle the transition; only
// workers still in flight from a PREVIOUS frame can observe it, and the pyramid's atomic,
// monotonic publish (Decision 6) makes that benign in either order -- they see the empty state
// (culled) or the complete pyramid, and this damage guarantees a following frame sees the
// latter.
//
// The damage names the image content DIRECTLY, from the pending entry's own awaiting list.
// There is no embedding indirection to reverse the way `arrival_damage` must for a
// composition: an image IS the damaged object. `Rect::infinite()` / `TimeRange::all()` and not
// the decoded extent, because the content's bounds were EMPTY and are now non-empty -- the
// region that changed is not expressible in the old geometry at all, and the absorbing shape
// is the honest answer.
//
// Returns whether the arrival was an ASSET arrival at all (so the caller knows not to hand it
// to the composition loader), and reports through `installed` whether it actually landed
// pixels. An arrival carrying empty or undecodable bytes changes nothing observable, so it
// opens NO transaction, publishes NO revision and flushes NO damage (Constraint 9): it costs
// exactly the one map erase `take_pending_asset` already did.
bool settle_asset_arrival(Document& doc, const PendingExternalLoads::Arrival& arrival,
                          bool& installed) {
  PendingExternalLoads::AssetEntry entry;
  if (!DocumentSerializeAccess::pending(doc)->take_pending_asset(arrival.child, entry)) {
    return false;
  }

  std::vector<Damage> damage;
  for (const ObjectId id : entry.awaiting) {
    Content* const content = doc.resolve(id);
    // `install_asset` is the contract-level virtual (`content.hpp`): the runtime reaches the
    // plugin's pixels through it and never through a `dynamic_cast` to the kind, which would
    // drag the decoder into `libarbc` -- the one thing doc 17's codec line forbids. A kind
    // that does not override it returns false and stays unavailable, which is also what
    // undecodable bytes yield: a value, never a throw (Constraint 11).
    if (content != nullptr && content->install_asset(arrival.bytes)) {
      damage_add(damage, Damage{id, Rect::infinite(), TimeRange::all()});
    }
  }
  if (damage.empty()) {
    installed = false;
    return true; // unavailable, and free: no transaction, no revision, no damage
  }

  Model::Transaction txn = DocumentSerializeAccess::model(doc).transact();
  for (const Damage& d : damage) {
    txn.add_damage(d);
  }
  static_cast<void>(txn.commit());
  installed = true;
  return true;
}

} // namespace

expected<std::monostate, ReaderError> load_document(std::string_view bytes, Document& doc,
                                                    KindBridge& bridge, const Registry& registry,
                                                    std::string base_uri, AssetSource* assets,
                                                    RasterTileStore* tiles,
                                                    TileDecodeDispatch* decode) {
  const JournalSuspension no_journal(doc);
  Model& into = DocumentSerializeAccess::model(doc);

  // `base_uri` is what a kind's relative references resolve against (doc 08 Principle 3:
  // "URIs resolved relative to the document"); `assets` is how their bytes are fetched.
  // Both default to nothing, and both being absent is a supported, benign configuration:
  // every external reference is then simply unavailable, which is what the fuzz lane and
  // every leaf-kind document already are.
  //
  // The SOURCE is also recorded on the document's durable load state, because a settle needs it
  // after this `LoadContext` is long gone: a child that lands late may itself hold external
  // refs, and its own fetches go through the same source.
  LoadContext ctx{std::move(base_uri)};
  ctx.set_asset_source(assets);
  DocumentSerializeAccess::pending(doc)->set_source(assets);

  // The host document's ROOT composition id, allocated up front so the loader can seed
  // its resolved-identity map with "this document -> this composition" (Constraint 6):
  // a document that references ITSELF then dedups to its own root, and a cross-document
  // Droste collapses onto the in-document one exactly. `allocate_id` is a bare monotonic
  // counter bump that installs no record, so this mutates nothing -- and the root is
  // still the lowest-id composition, as `find_first_composition` requires.
  const ObjectId root_composition = into.allocate_id();

  LoadAssembly assembly(doc, bridge, registry, tiles, decode);
  assembly.loader().seed(ctx.base_uri(), root_composition);

  // The document's unknown-field stash is replaced wholesale by a successful load and
  // left untouched on any error, exactly like the model (doc 08 Principle 4).
  const expected<std::monostate, ReaderError> loaded =
      arbc::load_document(bytes, registry, assembly.codecs(), ctx, assembly.sink(), into,
                          &DocumentSerializeAccess::unknown(doc), root_composition);
  if (loaded) {
    // The reader parsed `arbc.storage_format` into the context; carry it onto the document
    // so a subsequent save re-emits it (Decision 4). Re-saving at a format the user did not
    // author would rename every blob in the store and rewrite their whole painting at a
    // different precision.
    doc.set_storage_format(ctx.storage_format());
  }
  return loaded;
}

std::size_t settle_external_loads(Document& doc, KindBridge& bridge, const Registry& registry) {
  const std::shared_ptr<PendingExternalLoads>& state = DocumentSerializeAccess::pending(doc);

  // An arrival is not a user edit, so it is not undoable: an undo taken right after a widget
  // finally loaded must not "revert" it back to the placeholder. Same reasoning as a load
  // (doc 14:263-264) -- suspend the COMMIT sink. The DAMAGE sink stays installed: the whole
  // point of the settle is that its commit flushes damage (`model.cpp`'s commit flushes the
  // damage union whether or not a commit sink is present).
  const JournalSuspension no_journal(doc);

  std::size_t installed = 0;
  // Loop to quiescence (Decision 4): a settled child may itself hold external refs, and a
  // source that answers inline for THOSE will have queued their bytes during this very loop.
  // A deferring grandchild instead lands on a later settle -- its request cannot even be
  // ISSUED until its parent's bytes are parsed.
  for (;;) {
    std::vector<PendingExternalLoads::Arrival> ready = state->take_ready();
    if (ready.empty()) {
      break;
    }
    // A late-arriving child decodes INLINE: the settle path holds no worker pool, and a
    // deferred child is a rare, off-the-open-critical-path event.
    LoadAssembly assembly(doc, bridge, registry, nullptr, nullptr);
    for (const PendingExternalLoads::Arrival& arrival : ready) {
      // The ASSET arm first: one queue, two value shapes, and the asset map is the
      // discriminant (kinds.image_async_pending Decision 2). An asset arrival never reaches
      // the composition loader, and a composition arrival never finds an asset entry -- the
      // two id spaces are one monotonic `Model::allocate_id()` counter, so they cannot collide.
      if (bool landed = false; settle_asset_arrival(doc, arrival, landed)) {
        installed += landed ? 1U : 0U;
        continue;
      }
      const std::vector<Damage> damage = arrival_damage(doc, arrival.child);
      if (assembly.loader().settle(arrival.child, arrival.bytes, damage).valid()) {
        ++installed;
      }
    }
  }
  return installed;
}

} // namespace arbc
