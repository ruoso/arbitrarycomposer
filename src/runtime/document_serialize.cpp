// runtime.document_serialize: the L5 glue binding a `runtime::Document` to the
// serialize content seams (docs 08, 17). Holds the runtime-owned `KindBridge`, the
// built-in `CodecTable` assembly, the pinned-snapshot save path, and the
// `ContentSink` load path. This is a runtime TU that links nlohmann::json PRIVATE
// (through the internal `codec.hpp`, Decision 3); the runtime PUBLIC headers name no
// JSON type (Constraint 7).

#include <arbc/runtime/document_serialize.hpp>

#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/serialize/codec.hpp>       // CodecTable (internal; names nlohmann::json)
#include <arbc/serialize/deserialize.hpp> // DeserializeFn
#include <arbc/serialize/load_context.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace arbc {

// Attorney-client accessor (see `document.hpp`): the sole seam by which the load
// façade reaches `Document`'s internal `Model` to install the reconstructed
// version-0 baseline, without widening `Document`'s public shape (Constraint 4).
struct DocumentSerializeAccess {
  static Model& model(Document& doc) { return doc.d_model; }
};

// ---- KindBridge -------------------------------------------------------------

KindBridge::KindBridge() {
  // Pre-intern the built-in leaf kinds so their tokens are stable from construction
  // (Decision 1). Assignment order is free -- only the strings are serialized.
  intern(SolidContent::kind_id, k_solid_kind_version);
  intern(ToneContent::kind_id, k_tone_kind_version);
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
    codecs.add(SolidContent::kind_id,
               Codec{solid.serialize,
                     recording_deserialize(solid.deserialize, SolidContent::kind_id,
                                           k_solid_kind_version, session, bridge)});
    Codec tone = tone_codec();
    codecs.add(ToneContent::kind_id,
               Codec{tone.serialize,
                     recording_deserialize(tone.deserialize, ToneContent::kind_id,
                                           k_tone_kind_version, session, bridge)});
  }

  // The sink binds each reconstructed content into `doc` (minting its `ContentRecord`
  // and the side-map entry), stamping the kind the codec recorded -- or the reserved
  // unknown token for a PlaceholderContent no wrapper touched.
  const ContentSink sink = [&doc, &session](std::unique_ptr<Content> c) -> SunkContent {
    Content* const live = c.get();
    const auto it = session.find(live);
    const std::uint64_t kind = (it != session.end()) ? it->second : KindBridge::k_unknown_kind;
    const ObjectId id = doc.add_content(std::shared_ptr<Content>(std::move(c)), kind);
    return SunkContent{id, live};
  };

  // Built-in leaf kinds resolve no external assets, so the base URI is empty.
  LoadContext ctx{std::string{}};
  Model& into = DocumentSerializeAccess::model(doc);
  return arbc::load_document(bytes, registry, codecs, ctx, sink, into);
}

} // namespace arbc
