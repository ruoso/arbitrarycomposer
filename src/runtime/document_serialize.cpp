// runtime.document_serialize: the L5 glue binding a `runtime::Document` to the
// serialize content seams (docs 08, 17). Holds the runtime-owned `KindBridge`, the
// built-in `CodecTable` assembly, the pinned-snapshot save path, and the
// `ContentSink` load path. This is a runtime TU that links nlohmann::json PRIVATE
// (through the internal `codec.hpp`, Decision 3); the runtime PUBLIC headers name no
// JSON type (Constraint 7).

#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/document_serialize.hpp>
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
  static Model& model(Document& doc) { return doc.d_model; }
  static Journal& journal(Document& doc) { return doc.d_journal; }
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
  ObjectId comp_id;
  const CompositionRecord* comp = nullptr;
  if (!snap.state || !snap.state->find_first_composition(comp_id, comp)) {
    return snap;
  }
  snap.state->for_each_layer_in(comp_id, [&](ObjectId lid) {
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
    bridge.lookup(kind, kind_id, kind_version); // unknown -> empty views (placeholder body wins)
    const std::size_t idx = snap.entries.size();
    snap.entries.push_back(
        ContentSnapshot::Entry{c, std::string(kind_id), std::string(kind_version)});
    snap.by_id.emplace(cid, idx);
    snap.by_ptr.emplace(c, idx);
  });

  // Extend the reverse map to the operator INPUT CHILDREN (runtime.operator_codecs
  // Decision 5): the layer walk above captured only layer-root contents (bound by a
  // `ContentRecord`, keyed into `by_id`). An operator's input children have no
  // ObjectId, so the writer resolves each child's `(kind, kind_version)` through the
  // `const Content&`-keyed meta provider instead -- backed here by a transitive walk
  // of `Content::inputs()` over the pinned graph, interning each reachable built-in
  // child into `by_ptr` (a meta entry, no `by_id`/body entry). The walk runs on the
  // writer thread over the `Document`-owned content graph, so the off-thread emit
  // reads only immutable snapshot data (Constraint 10). A `PlaceholderContent` (or any
  // non-built-in) child gets no entry -> meta `nullopt` -> its stored body re-emits
  // verbatim (doc 08 Principle 2); its own inputs are still descended so deeper
  // built-in nodes are captured. v1 `$ref` graphs are acyclic DAGs, and `walked` guards
  // shared re-encounters (sharing Decision 8).
  std::vector<const Content*> frontier;
  frontier.reserve(snap.entries.size());
  std::unordered_set<const Content*> walked;
  for (const ContentSnapshot::Entry& e : snap.entries) {
    frontier.push_back(e.content);
    walked.insert(e.content);
  }
  while (!frontier.empty()) {
    const Content* const c = frontier.back();
    frontier.pop_back();
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
      const std::size_t cidx = snap.entries.size();
      snap.entries.push_back(ContentSnapshot::Entry{child, std::string(child_kind_id),
                                                    std::string(child_kind_version)});
      snap.by_ptr.emplace(child, cidx);
    }
  }
  return snap;
}

expected<std::string, SerializeError> serialize_snapshot(const ContentSnapshot& snapshot,
                                                         const CodecTable& codecs) {
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
    return ContentMeta{e.kind_id, e.kind_version};
  };
  return serialize_document(*snapshot.state, provider, meta, codecs);
}

expected<std::string, SerializeError> save_document(const Document& doc, const KindBridge& bridge,
                                                    const CodecTable& codecs) {
  return serialize_snapshot(capture_snapshot(doc, bridge), codecs);
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
                             LoadContext& ctx) -> expected<std::unique_ptr<Content>, ReaderError> {
    expected<std::unique_ptr<Content>, ReaderError> produced = base(params, inputs, ctx);
    if (produced) {
      session[(*produced).get()] = bridge.intern(kind, version);
    }
    return produced;
  };
}

} // namespace

expected<std::monostate, ReaderError> load_document(std::string_view bytes, Document& doc,
                                                    KindBridge& bridge, const Registry& registry) {
  LoadSession session;

  // The per-load codec table: built-in serialize (unused on load) + a
  // kind-recording deserialize wrapper per built-in kind (Decision 4).
  CodecTable codecs;
  {
    Codec solid = solid_codec();
    codecs.add(
        SolidContent::kind_id,
        Codec{solid.serialize, recording_deserialize(solid.deserialize, SolidContent::kind_id,
                                                     k_solid_kind_version, session, bridge)});
    Codec tone = tone_codec();
    codecs.add(ToneContent::kind_id,
               Codec{tone.serialize, recording_deserialize(tone.deserialize, ToneContent::kind_id,
                                                           k_tone_kind_version, session, bridge)});
    Codec fade = fade_codec();
    codecs.add(FadeContent::kind_id,
               Codec{fade.serialize, recording_deserialize(fade.deserialize, FadeContent::kind_id,
                                                           k_fade_kind_version, session, bridge)});
    Codec crossfade = crossfade_codec();
    codecs.add(CrossfadeContent::kind_id,
               Codec{crossfade.serialize,
                     recording_deserialize(crossfade.deserialize, CrossfadeContent::kind_id,
                                           k_crossfade_kind_version, session, bridge)});
  }

  // Provisional-root records minted by the sink, keyed by live pointer: a node is
  // minted as a layer root when first sunk, then DEMOTED to an owned-only input child
  // (its `ContentRecord` removed, its object kept alive in `d_contents`) the moment a
  // later-sunk parent operator lists it in `inputs()` (runtime.operator_codecs
  // Decision 4). The read recursion sinks children bottom-up BEFORE their parent and
  // binds a layer to the OUTERMOST (last) sunk node, threading inner nodes only by live
  // `Content*` (their `.id` is discarded, reader.cpp) -- so demoting an inner node's
  // record is invisible to the graph wiring, and only true layer roots keep a record
  // (find_content surfaces roots alone). The model stores no input edges (records.hpp),
  // so a child needs no ObjectId. Every intermediate transaction's revision is reset by
  // `load_baseline`'s revision-0 publish, so a successful load still lands at revision 0.
  // (A content that is BOTH a layer root and an operator input -- a shared node at a
  // layer position AND an inputs slot -- is outside this leaf's fade/crossfade scope;
  // it would be demoted here. See the parked multi-composition design question.)
  std::unordered_map<const Content*, ObjectId> minted;

  const ContentSink sink = [&doc, &session, &minted](std::unique_ptr<Content> c) -> SunkContent {
    Content* const live = c.get();
    // Any input this node adopts is now known to be an input child, not a layer root:
    // drop its provisional record (the object stays owned by `d_contents`).
    for (const ContentRef child : live->inputs()) {
      const auto mit = minted.find(child);
      if (mit == minted.end()) {
        continue;
      }
      Model& model = DocumentSerializeAccess::model(doc);
      Model::Transaction txn = model.transact();
      txn.remove(mit->second);
      static_cast<void>(txn.commit());
      minted.erase(mit);
    }
    const auto it = session.find(live);
    const std::uint64_t kind = (it != session.end()) ? it->second : KindBridge::k_unknown_kind;
    const ObjectId id = doc.add_content(std::shared_ptr<Content>(std::move(c)), kind);
    minted.emplace(live, id);
    return SunkContent{id, live};
  };

  // Built-in leaf kinds resolve no external assets, so the base URI is empty.
  LoadContext ctx{std::string{}};
  const JournalSuspension no_journal(doc);
  Model& into = DocumentSerializeAccess::model(doc);
  return arbc::load_document(bytes, registry, codecs, ctx, sink, into);
}

} // namespace arbc
