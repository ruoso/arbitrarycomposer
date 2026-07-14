#pragma once

// The L5 load<->save integration (runtime.document_serialize): wire a
// `runtime::Document` to the serialize content seams so a built-in-kind document
// (org.arbc.solid / org.arbc.tone) round-trips through the `.arbc` format with its
// content bodies intact (docs 08, 17). All new code is `arbc::runtime`; this public
// header names NO JSON type (Constraint 7) -- `CodecTable` is only forward-declared,
// exactly as `writer.hpp`/`reader.hpp` do.

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/serialize/load_context.hpp> // AssetSource (the external-reference load hook)
#include <arbc/serialize/reader.hpp>       // ReaderError
#include <arbc/serialize/writer.hpp>       // SerializeError

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace arbc {

class Content;
class CodecTable; // forward-declared (names nlohmann::json; off this public header)

// The runtime-owned bijection realizing the `ContentRecord.kind` uint64 <->
// reverse-DNS `kind_id` string bridge the model and writer deferred (Decision 1).
// `Document::add_content` stores an opaque `uint64`; the format's contractual
// identity is the reverse-DNS string. This is the one place mapping between them,
// carrying each kind's producer `kind_version`. Built-in kinds are pre-interned at
// construction; `intern` assigns monotonically on first sight. Owns stable string
// storage (a `deque`, whose elements never relocate) so the `string_view`s handed
// to `ContentBody`/`ContentMeta` outlive the serialize call. The uint64 is never
// serialized (only the string is), so its concrete values do not affect goldens.
class KindBridge {
public:
  // The reserved token a `PlaceholderContent` (unknown kind, no codec) is stamped
  // with: it round-trips as a placeholder whose stored body is authoritative, so the
  // bridge never needs a string for it (`lookup` returns false, empty views).
  static constexpr std::uint64_t k_unknown_kind = 0;

  // Pre-interns the built-in leaf kinds (solid, tone) with their pinned versions.
  KindBridge();

  // The interned token for `kind_id`, assigning a fresh one (monotonic) on first
  // sight and remembering its `kind_version`; a repeat `kind_id` returns the token
  // assigned before (the first-seen version is retained).
  std::uint64_t intern(std::string_view kind_id, std::string_view kind_version);

  // The reverse the save path reads: resolves a token to its `{kind_id,
  // kind_version}`. Returns false (leaving the views empty) for `k_unknown_kind` or
  // any token never interned.
  bool lookup(std::uint64_t token, std::string_view& kind_id, std::string_view& kind_version) const;

private:
  struct Entry {
    std::string kind_id;
    std::string kind_version;
  };
  std::deque<Entry> d_entries;                            // token-1 -> entry (stable storage)
  std::unordered_map<std::string, std::uint64_t> d_by_id; // kind_id -> token
};

// An immutable, off-thread-safe capture of a pinned document's content bindings
// (Decision 6). Built on the writer thread by `capture_snapshot`: a pinned
// `DocRoot` plus, for every content a layer binds, the live `Content*` (raw --
// kept valid by the `Document` that owns it, whose content side-map is append-only)
// and its bridged `{kind_id, kind_version}` strings. The provider/meta closures and
// `serialize_document` then run over ONLY this snapshot -- no lock, no `d_contents`
// read -- so a save captured on the writer thread emits correctly off-thread while
// editing continues.
struct ContentSnapshot {
  struct Entry {
    const Content* content{nullptr};
    std::string kind_id;
    std::string kind_version;
    // The content's ObjectId -- its unknown-field stash key. A layer root's comes from
    // the `LayerRecord`; a demoted operator input child's comes from `Document`'s
    // content side-map, which keeps its entry after demotion (Decision 3).
    ObjectId id{};
  };
  DocStatePtr state; // the pinned, immutable DocRoot
  std::vector<Entry> entries;
  std::unordered_map<ObjectId, std::size_t> by_id;        // content ObjectId -> entry index
  std::unordered_map<const Content*, std::size_t> by_ptr; // live Content* -> entry index
  // A COPY of the document's unknown-field stash (doc 08 Principle 4, Decision 6). The
  // store is writer-thread-owned like the content side-map, so the snapshot copies it
  // rather than holding a reference into live state -- that is what keeps the off-thread
  // save free of any live-editor read (Constraint 9) and preserves
  // `08-serialization#writer-serializes-the-pinned-version`. Cheap: plain maps of
  // `std::string`.
  UnknownFieldStore unknown;
};

// The runtime's built-in codec table for the SAVE path: org.arbc.solid +
// org.arbc.tone, each a `SerializeFn`/`DeserializeFn` pair. Returned by value (the
// caller holds it; `CodecTable` is complete at the caller through `codec.hpp`).
CodecTable builtin_codecs();

// The same table PLUS the codecs of every OUT-OF-LIB kind whose plugin is actually loaded
// (kinds.image Decision 2). The `Registry` is the plugin-present witness: a registered
// factory for `org.arbc.image` IS "the image plugin is here", so its codec joins the table;
// with no plugin there is no codec and the layer round-trips verbatim as a
// `PlaceholderContent`, losing nothing (doc 08 Principles 2/4).
//
// A HOST WITH PLUGINS LOADED SHOULD SAVE THROUGH THIS TABLE, not the no-argument one: a live
// `org.arbc.image` content has no codec in the plain built-in table, and serializing it would
// be `SerializeError::Kind::NoCodec`. The load path (`load_document`) already assembles its
// table this way. The no-argument overload remains exactly right for a caller that only ever
// holds built-in kinds.
CodecTable builtin_codecs(const Registry& registry);

// Capture a pinned content-binding snapshot of `doc` (MUST run on the writer
// thread): pins the current version and copies each layer-bound content's live
// pointer + bridged kind into an immutable `ContentSnapshot`.
ContentSnapshot capture_snapshot(const Document& doc, const KindBridge& bridge);

// Emit a captured snapshot to canonical `.arbc` bytes (thread-safe: reads only the
// immutable snapshot). The content-body provider resolves each content from the
// pinned `ContentRecord.kind`, so the serialized kind reflects the pinned version.
expected<std::string, SerializeError> serialize_snapshot(const ContentSnapshot& snapshot,
                                                         const CodecTable& codecs);

// Convenience: `serialize_snapshot(capture_snapshot(doc, bridge), codecs)`. Pins,
// captures on the writer thread, then emits.
expected<std::string, SerializeError> save_document(const Document& doc, const KindBridge& bridge,
                                                    const CodecTable& codecs);

// Convenience over the built-in codec table (`builtin_codecs()`), so a caller that
// only round-trips the built-in kinds need not name `CodecTable`.
expected<std::string, SerializeError> save_document(const Document& doc, const KindBridge& bridge);

// Load canonical `.arbc` `bytes` into `doc` for the built-in leaf kinds: constructs
// a `LoadContext`, a per-load session-recording codec table, and a `ContentSink`
// that binds each reconstructed content into `doc` (interning its kind through
// `bridge`), then drives the serialize reader's content-aware `load_document`. An
// unknown kind (no codec) round-trips as a `PlaceholderContent`. On any error the
// document is left unmutated (revision 0). `registry` is the plugin-present witness
// the placeholder path consults.
//
// `base_uri` is the document's own URI -- what a kind's RELATIVE references resolve
// against (doc 08 Principle 3, "URIs resolved relative to the document") -- and `assets`
// is how the bytes behind a resolved reference are fetched. Supply both to make a nested
// content's external `params.ref` (doc 05:47-61) actually load the `.arbc` it names:
// `FilesystemAssetSource` is the built-in source, and a host with a content store plugs
// its own in here. `runtime.nested_external_ref` ships the loader those two feed.
//
// Both default to nothing, and that is a fully supported configuration -- it is what the
// fuzz lane and every leaf-kind document already are. With no base and no source, an
// external reference is simply UNAVAILABLE: the nested content loads with a null child,
// keeps its `ref` verbatim (so the document re-saves byte-identically), renders the doc-05
// placeholder, and the parent load SUCCEEDS. A missing widget file never makes a project
// unopenable.
expected<std::monostate, ReaderError> load_document(std::string_view bytes, Document& doc,
                                                    KindBridge& bridge, const Registry& registry,
                                                    std::string base_uri = {},
                                                    AssetSource* assets = nullptr);

// Install every external child whose bytes have ARRIVED since the last call, and return how
// many compositions were installed (runtime.async_external_load). WRITER-THREAD ONLY.
//
// `load_document` only resolves an external reference if the `AssetSource` answers INSIDE
// `request()`. `FilesystemAssetSource` does; a network fetch, a content store, or any
// plugin-supplied source does not -- it returns from `request()` with nothing, and its
// `on_ready` fires later, on whatever thread it chose, after the load's `LoadContext`,
// `CodecTable`, loader and sink are all destroyed. Such a reference loads as PENDING: the
// embedding content binds the VALID child id the loader minted before fetching, which names
// no `CompositionRecord` yet and therefore renders as the doc-05 placeholder, and the parent
// load completes at revision 0 without waiting.
//
// This is where the late bytes land. `on_ready` did nothing but copy them into the document's
// completion queue -- it touched no `Model`, because every publish path is writer-confined
// (doc 14, "single writer, lock-free pinned reads"). Here, on the writer thread, each arrival
// is parsed and installed under that SAME pre-allocated id through one ordinary transaction,
// which publishes a new revision and flushes, in that same commit, damage naming the
// EMBEDDING content. Doc 02's *Refine* step turns that damage into a follow-up frame, and the
// placeholder is replaced live.
//
// It drains to quiescence: a child that lands may itself hold external references, whose
// bytes an inline-answering source has already queued. A DEFERRING grandchild instead lands
// on a later call -- its request cannot even be issued until its parent's bytes are parsed --
// so a host calls this once per frame (`HostViewport::Config::settle_external_loads` wires it
// into doc 02 step 1) and the chain lands over successive frames.
//
// Cheap and safe to call on a document with nothing pending: it is one queue check. Not
// undoable -- a widget finally loading is not an edit, so an undo taken right after must not
// revert it (doc 14:263-264).
std::size_t settle_external_loads(Document& doc, KindBridge& bridge, const Registry& registry);

} // namespace arbc
