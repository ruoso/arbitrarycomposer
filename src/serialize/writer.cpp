#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/media/audio_format.hpp>
#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp> // to_string(PixelFormat)
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/serialize/codec.hpp>        // the content-body write routing this task lands
#include <arbc/serialize/unknown_json.hpp> // the never-shadow merge (doc 08 Principle 4)
#include <arbc/serialize/writer.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace arbc {
namespace {

using json = nlohmann::json;

// Canonical number rule (doc 08 Principle 5, as amended): a placement scalar that
// is not finite cannot round-trip through JSON, so it is a serialization error,
// never a `null`. `Emitter` faults as a value -- the first non-finite scalar
// latches `d_ok = false` and records the offending object; the walk finishes
// harmlessly (returning a benign 0) and the public entry returns the error before
// any bytes are handed back.
class Emitter {
public:
  // A fractional placement scalar (transform / opacity / gain) as a JSON real.
  json real(double v, ObjectId owner) {
    if (!std::isfinite(v)) {
      fault(owner);
      return json(0.0);
    }
    return json(v);
  }

  // An integer-valued core scalar (a canvas extent) as a JSON integer. Faulted
  // before the cast, since casting a non-finite double to int64 is UB.
  json integer_extent(double v, ObjectId owner) {
    if (!std::isfinite(v)) {
      fault(owner);
      return json(std::int64_t{0});
    }
    return json(static_cast<std::int64_t>(v));
  }

  bool ok() const { return d_ok; }
  const SerializeError& error() const { return d_err; }

  // Latch an arbitrary serialization error (a content-body codec failure): the
  // first fault wins and the walk finishes harmlessly, exactly as a non-finite
  // scalar does, so the public entry returns the error before any bytes escape.
  void fail(const SerializeError& err) {
    if (d_ok) {
      d_ok = false;
      d_err = err;
    }
  }

private:
  void fault(ObjectId owner) { fail(SerializeError{SerializeError::Kind::NonFiniteValue, owner}); }

  bool d_ok{true};
  SerializeError d_err{};
};

const char* primaries_token(Primaries p) {
  switch (p) {
  case Primaries::Srgb:
    return "srgb";
  }
  return "srgb";
}

const char* transfer_token(TransferFunction t) {
  switch (t) {
  case TransferFunction::Linear:
    return "linear";
  case TransferFunction::Srgb:
    return "srgb";
  }
  return "linear";
}

// working_space: a lossless object in the doc 08 example's shape (primaries /
// transfer / format), extended with the `premultiplied` tag so the full
// SurfaceFormat round-trips (Principle 2 -- never destroy data). Emitted only when
// the composition's working space differs from the doc 07 default.
json working_space_json(const SurfaceFormat& f) {
  json o = json::object();
  o["format"] = to_string(f.pixel_format);
  o["premultiplied"] = f.premultiplied == Premultiplied::Yes;
  o["primaries"] = primaries_token(f.color_space.primaries);
  o["transfer"] = transfer_token(f.color_space.transfer);
  return o;
}

// working_audio_format: the audio twin, emitted only when it differs from the
// doc 12 default (k_working_audio). Sample rate is an integer core scalar.
json working_audio_format_json(const AudioFormat& f) {
  json o = json::object();
  o["channels"] = f.layout == ChannelLayout::Stereo ? "stereo" : "mono";
  o["sample_rate"] = static_cast<std::int64_t>(f.sample_rate);
  return o;
}

// time_map: the parent->content-local 1D affine (doc 11). Flick instants are
// integers; the exact rational rate is a `[num, den]` integer pair. Emitted only
// when non-identity.
json time_map_json(const TimeMap& m) {
  json o = json::object();
  o["in"] = static_cast<std::int64_t>(m.in.flicks);
  o["offset"] = static_cast<std::int64_t>(m.offset.flicks);
  o["rate"] = json::array(
      {static_cast<std::int64_t>(m.rate.num()), static_cast<std::int64_t>(m.rate.den())});
  return o;
}

// The write face of doc 08 Principles 6 AND 7 (serialize.sharing +
// serialize.compositions_table): the document's *graph of compositions* and the
// operator graph rooted at each one's layer contents, emitted structurally by ONE
// interleaved pre-pass.
//
// The traversal (compositions_table Constraint 3): compositions BREADTH-first from the
// root, each composition's layers bottom-to-top, each layer's content graph DEPTH-first
// over `inputs()` in declared order; a content's `composition_ref()` enqueues its child
// composition on first encounter. It assigns BOTH id spaces -- the composition ordinals
// (`"0"` == the root, always, since it is encountered first; `"1"`..`"N"` the reachable
// non-root ones) and the shared-`contents` ordinals -- from that single deterministic
// order, so output is byte-stable across runs and re-serializations (Principle 5).
//
// Content reached two or more times ACROSS THE WHOLE reachable graph -- spanning
// compositions, not per-composition (Constraint 4) -- is shared: hoisted once into the
// document-level `contents` table, referenced by `{"$ref": id}` at every use site.
//
// Breadth-first, not depth-first, is what makes the walk cycle-safe: a composition is
// enqueued on first encounter and its layers built only when popped, and the visited set
// (keyed by composition `ObjectId`) bounds a Droste back-edge. Cycles here are LEGAL --
// unlike `$ref` input cycles, which stay acyclic (Principle 6).
class ContentGraph {
public:
  ContentGraph(const ContentBodyProvider& provider, const ContentMetaProvider& meta,
               const CodecTable& codecs, const UnknownFieldStore* unknown)
      : d_provider(provider), d_meta(meta), d_codecs(codecs), d_unknown(unknown) {}

  // One reachable composition: its model id, its record, and (implicitly, by index) its
  // ordinal -- index 0 is the root, which keeps its home at `root["composition"]`.
  struct CompEntry {
    ObjectId id;
    const CompositionRecord* record;
  };

  // Pre-pass over the document graph: discover every reachable composition, count
  // content occurrences, record metadata, assign both id spaces. Must run before any
  // layer, `compositions`, or `contents` emission.
  void build(const DocRoot& doc, ObjectId root_comp) {
    d_doc = &doc;
    enqueue_composition(root_comp); // the root: ordinal "0" (Decision 2)
    // Breadth-first: `d_comps` grows while we walk it, so a composition discovered
    // through a nesting content's `composition_ref()` has its layers walked in turn --
    // and a back-edge to an already-seen composition simply re-uses its ordinal.
    for (std::size_t i = 0; i < d_comps.size(); ++i) {
      doc.for_each_layer_in(d_comps[i].id, [&](ObjectId lid) {
        const LayerRecord* lr = doc.find_layer(lid);
        if (lr == nullptr) {
          return;
        }
        const std::optional<ContentBody> body = d_provider(lr->content);
        if (!body.has_value()) {
          return;
        }
        // A layer root's ObjectId is the layer record's `content` -- the same id the
        // reader's sink minted, so it keys this content's unknown-field stash directly.
        visit(&body->content, body->kind, body->kind_version, lr->content);
      });
    }
    std::size_t next = 0;
    for (const Content* c : d_order) {
      if (d_counts[c] >= 2) {
        d_ids.emplace(c, std::to_string(next++));
        d_shared.push_back(c);
      }
    }
  }

  // The reachable compositions in first-encounter order; index i holds ordinal `i`, so
  // index 0 is the root and `[1, size)` is exactly the `compositions` table.
  const std::vector<CompEntry>& compositions() const { return d_comps; }

  // Emit a use site: `{"$ref": id}` for a shared content, else its inline body.
  json emit_use(const Content* c, Emitter& em) {
    if (const auto it = d_ids.find(c); it != d_ids.end()) {
      json ref = json::object();
      ref["$ref"] = it->second;
      return ref;
    }
    return emit_definition(c, em);
  }

  // The optional document-level `contents` table: each shared content's full inline
  // body emitted ONCE (its own shared children referenced by `$ref`), keyed by id.
  // Returns nullopt when nothing is shared, so the table is omitted (Constraint 6).
  std::optional<json> emit_contents(Emitter& em) {
    if (d_shared.empty()) {
      return std::nullopt;
    }
    json contents = json::object();
    for (const Content* c : d_shared) {
      contents[d_ids.at(c)] = emit_definition(c, em);
    }
    return contents;
  }

private:
  struct MetaEntry {
    std::string kind;
    std::string kind_version;
    ObjectId id; // this content's stash key (Decision 3)
  };

  // Enqueue a composition on first encounter, assigning it the next ordinal. A null id,
  // or one naming no `CompositionRecord` in this document, is not a composition
  // reference: it is silently no reference at all, so the emitted body simply carries no
  // `composition` key (an id the core cannot resolve is never written out as a dangling
  // one). Returns true when `cid` names a reachable composition.
  bool enqueue_composition(ObjectId cid) {
    if (!cid.valid() || d_doc == nullptr) {
      return false;
    }
    if (d_comp_ids.find(cid) != d_comp_ids.end()) {
      return true; // already seen -- the Droste back-edge lands here
    }
    const CompositionRecord* rec = d_doc->find_composition(cid);
    if (rec == nullptr) {
      return false;
    }
    d_comp_ids.emplace(cid, std::to_string(d_comps.size()));
    d_comps.push_back(CompEntry{cid, rec});
    return true;
  }

  void visit(const Content* c, std::string_view kind, std::string_view kind_version, ObjectId id) {
    const auto [it, inserted] = d_counts.try_emplace(c, 0);
    it->second += 1;
    if (inserted) {
      d_meta_of.emplace(c, MetaEntry{std::string(kind), std::string(kind_version), id});
      d_order.push_back(c);
    }
    if (it->second > 1) {
      return; // already descended into this subtree; count the extra edge, don't recurse
    }
    // A nesting content's child composition is graph structure exactly like its inputs
    // (doc 08 Principle 7), and the core reads it off the same kind-agnostic accessor an
    // unknown-kind placeholder also carries -- so a missing plugin never orphans it.
    //
    // And a content that names a RESOLVABLE child composition has no authored inputs to
    // descend: its `inputs()` are a PROJECTION of that child's layers (doc 08 P7's closing
    // rule), which the BFS above already walks in full. Descending them anyway would count
    // each of the child's layer contents twice -- pushing them over Principle 6's
    // shared-two-or-more threshold and hoisting them into `contents` behind `$ref`s that
    // describe no authored sharing -- so the same scene would emit different bytes
    // depending on whether a render binding happened to be attached when it was saved. For
    // a Droste scene it is worse: the content becomes a transitive input of ITSELF and the
    // `$ref` closes an operator-input cycle, which Principle 6 forbids and the reader
    // rejects. Nothing is lost by stopping here.
    //
    // An EXTERNAL child is stopped at BEFORE either edge (doc 08 Principle 3, Principle
    // 7's third corollary; runtime.nested_external_ref Decision 2). Its composition is a
    // perfectly ordinary composition in THIS model -- that is what makes render, damage
    // and caching need no external-ness at all -- but it is the other document's DATA, and
    // enqueueing it would walk its layers into this document's `contents` table and emit a
    // `compositions` entry for it. Load, save, and the reference is gone, replaced by a
    // frozen copy: the reference the user authored is silently INLINED. So the traversal
    // descends neither the child composition nor the projected `inputs()`; the URI rides
    // the kind's `params`, which is the one thing the core does not own.
    if (!c->external_composition_ref().empty()) {
      return;
    }
    if (enqueue_composition(c->composition_ref())) {
      return;
    }
    for (const ContentRef child : c->inputs()) {
      if (child == nullptr) {
        continue;
      }
      const std::optional<ContentMeta> m = d_meta(*child);
      if (m.has_value()) {
        visit(child, m->kind, m->kind_version, m->id);
      } else {
        // No metadata: let content_body_to_json fault (or a placeholder re-emit its own
        // stored body). With no id there is no stash to merge -- never a fault.
        visit(child, std::string_view{}, std::string_view{}, ObjectId{});
      }
    }
  }

  // One node's full inline body: its `{kind, kind_version, params}` leaf plus EITHER the
  // core-owned `composition` reference (a nesting content) OR, when `inputs()` is
  // non-empty, an order-preserving `inputs` array whose slots are each a `$ref` (shared)
  // or a nested inline body (recursion). Never both, and `inputs` is omitted when empty.
  json emit_definition(const Content* c, Emitter& em) {
    const MetaEntry& m = d_meta_of.at(c);
    expected<json, SerializeError> leaf =
        content_body_to_json(m.kind, m.kind_version, *c, d_codecs);
    if (!leaf) {
      em.fail(leaf.error());
      return json::object();
    }
    json body = std::move(*leaf);
    // The content tier's preserved siblings (doc 08 Principle 4): a standalone body's
    // unknown keys, and -- for a KNOWN kind -- the `params` interior the codec never
    // consumed, which recurses into the codec's freshly-produced `params` object. The
    // known side always wins (doc 08:96). A layer-position body carries no body-level
    // stash: its unknown siblings are LAYER fields (Decision 5), merged in `layer_json`.
    if (d_unknown != nullptr) {
      merge_unknown_fields(body, d_unknown->find(UnknownScope::Content, m.id));
    }
    // An EXTERNAL child is named by the kind's `params.ref` and by nothing else (doc 08
    // Principle 3): the core emits NEITHER a `composition` field NOR an `inputs` array
    // for it. The child's contents belong to the other document -- `visit` above already
    // refused to walk them, so they are in no `contents` table and its composition is in
    // no `compositions` table, and emitting a `composition` ordinal here would name an
    // entry that does not exist. Whether the reference actually LOADED is invisible from
    // here, which is Constraint 9's second half: a document saved with the widget file
    // missing is byte-identical to the same document saved with it present.
    if (!c->external_composition_ref().empty()) {
      return body;
    }
    // The core-owned child-composition reference (doc 08 Principle 7), appended AFTER
    // the codec returned and after the unknown merge, exactly as `inputs` is: the id is
    // re-derived from graph structure on every save, so a codec can neither write it nor
    // fight it (Constraint 1). `"0"` is the root -- that is how a Droste back-edge is
    // spelled. Absent when the content names no (resolvable) composition.
    const auto cit = d_comp_ids.find(c->composition_ref());
    if (cit != d_comp_ids.end()) {
      body["composition"] = cit->second;
      // Exclusive with `inputs`, and this is the side that wins: a nesting content's input
      // edges are a projection of the child composition this key already names, never
      // authored data (doc 08 P7). A kind names its child OR takes authored inputs, never
      // both -- the traversal above stopped for the same reason, and the reader rejects a
      // body carrying both.
      return body;
    }
    const std::span<const ContentRef> ins = c->inputs();
    if (!ins.empty()) {
      json arr = json::array();
      for (const ContentRef child : ins) {
        arr.push_back(child != nullptr ? emit_use(child, em) : json());
      }
      body["inputs"] = std::move(arr);
    }
    return body;
  }

  const ContentBodyProvider& d_provider;
  const ContentMetaProvider& d_meta;
  const CodecTable& d_codecs;
  const UnknownFieldStore* d_unknown;                      // content-tier stashes (nullable)
  const DocRoot* d_doc{nullptr};                           // set by build(); the composition source
  std::unordered_map<const Content*, int> d_counts;        // occurrences across the graph
  std::unordered_map<const Content*, MetaEntry> d_meta_of; // (kind, kind_version) per node
  std::unordered_map<const Content*, std::string> d_ids;   // shared content -> `$ref` id
  std::vector<const Content*> d_order;                     // first-encounter traversal order
  std::vector<const Content*> d_shared;                    // shared contents, in id order
  std::unordered_map<ObjectId, std::string> d_comp_ids;    // composition -> ordinal (visited set)
  std::vector<CompEntry> d_comps;                          // reachable compositions, root first
};

json layer_json(const LayerRecord& lr, ObjectId id, Emitter& em,
                const ContentBodyProvider* provider, ContentGraph* graph,
                const UnknownFieldStore* unknown) {
  json o = json::object();
  // Spatial + core placement -- always present.
  const Affine& t = lr.transform;
  o["transform"] = json::array({em.real(t.a, id), em.real(t.b, id), em.real(t.c, id),
                                em.real(t.d, id), em.real(t.tx, id), em.real(t.ty, id)});
  o["opacity"] = em.real(lr.opacity, id);
  o["visible"] = lr.visible();
  // Temporal + audio twins -- omit-when-default (Decision 2): a still layer stays
  // diff-clean; a non-still layer round-trips losslessly.
  if (lr.gain != 1.0) {
    o["gain"] = em.real(lr.gain, id);
  }
  if (!lr.audible()) {
    o["audible"] = false;
  }
  if (!(lr.span == TimeRange::all())) {
    o["span"] = json::array({static_cast<std::int64_t>(lr.span.start.flicks),
                             static_cast<std::int64_t>(lr.span.end.flicks)});
  }
  if (!(lr.time_map == TimeMap{})) {
    o["time_map"] = time_map_json(lr.time_map);
  }
  // Content position (serialize.kind_params + serialize.sharing): only through the
  // content-aware overload. The provider resolves this layer's bound content; the
  // graph emits its operator subtree beside the placement keys -- either the inline
  // `{kind, kind_version, params, inputs?}` body (its inputs walked from `inputs()`)
  // or, when the root content is itself shared across the document, a `{"$ref": id}`
  // into the `contents` table. The canonical key sort orders whichever keys land. The
  // no-provider path leaves the layer body-free, byte-identical to the pre-kind_params
  // goldens (Constraint 6).
  if (provider != nullptr && graph != nullptr) {
    const std::optional<ContentBody> body = (*provider)(lr.content);
    if (body.has_value()) {
      const json emitted = graph->emit_use(&body->content, em);
      for (auto it = emitted.begin(); it != emitted.end(); ++it) {
        o[it.key()] = it.value();
      }
    }
  }
  // The layer tier's preserved siblings LAST, so both the core's placement keys and the
  // spliced content body win any collision (doc 08:96). This is also where an inline
  // body's own unknown siblings land -- they are indistinguishable from unknown layer
  // fields and were recorded as such (Decision 5) -- and where an unknown key nested
  // inside `time_map` merges back into the writer's own `time_map` object.
  if (unknown != nullptr) {
    merge_unknown_fields(o, unknown->find(UnknownScope::Layer, id));
  }
  return o;
}

json composition_json(const DocRoot& doc, ObjectId comp_id, const CompositionRecord& comp,
                      Emitter& em, const ContentBodyProvider* provider, ContentGraph* graph,
                      const UnknownFieldStore* unknown) {
  json o = json::object();
  // canvas hint (doc 01): `[x, y, w, h]`, extents as integers.
  o["canvas"] =
      json::array({std::int64_t{0}, std::int64_t{0}, em.integer_extent(comp.canvas_w, comp_id),
                   em.integer_extent(comp.canvas_h, comp_id)});
  if (!(comp.working_space == k_working_rgba32f)) {
    o["working_space"] = working_space_json(comp.working_space);
  }
  if (!(comp.working_audio_format == k_working_audio)) {
    o["working_audio_format"] = working_audio_format_json(comp.working_audio_format);
  }
  // Layers bottom-to-top (the composition-scoped order accessor, model.hpp:83).
  json layers = json::array();
  doc.for_each_layer_in(comp_id, [&](ObjectId lid) {
    const LayerRecord* lr = doc.find_layer(lid);
    if (lr != nullptr) {
      layers.push_back(layer_json(*lr, lid, em, provider, graph, unknown));
    }
  });
  o["layers"] = std::move(layers);
  // The composition tier's preserved siblings, including the residuals nested back under
  // `working_space` / `working_audio_format` -- where the never-shadow rule bites for
  // real: the reader ignores `working_space.primaries` while the writer emits it, so the
  // preserved copy loses to the live one.
  if (unknown != nullptr) {
    merge_unknown_fields(o, unknown->find(UnknownScope::Composition, comp_id));
  }
  return o;
}

// The shared serialization core (Constraint 6): identical envelope + composition +
// placement walk for both public overloads; `provider`/`meta`/`codecs` are null on
// the content-free path (byte-identical to today) and non-null on the content-aware
// one, where the operator graph + shared `contents` table are emitted.
expected<std::string, SerializeError> serialize_impl(const DocRoot& doc,
                                                     const ContentBodyProvider* provider,
                                                     const ContentMetaProvider* meta,
                                                     const CodecTable* codecs,
                                                     const UnknownFieldStore* unknown) {
  Emitter em;
  json root = json::object();
  json envelope = json::object();
  envelope["format"] = std::int64_t{1}; // format major only (Decision 5)
  root["arbc"] = std::move(envelope);

  ObjectId comp_id;
  const CompositionRecord* comp = nullptr;
  if (doc.find_first_composition(comp_id, comp)) {
    // The content-aware path pre-passes the whole graph (occurrence counts + shared
    // ids) BEFORE emitting layers, so a shared root/input emits `{"$ref": id}` and the
    // definition lands once in `contents` (Constraint 2). The `std::optional` holds the
    // graph only when all three content seams are supplied.
    std::optional<ContentGraph> graph;
    if (provider != nullptr && meta != nullptr && codecs != nullptr) {
      graph.emplace(*provider, *meta, *codecs, unknown);
      graph->build(doc, comp_id);
    }
    ContentGraph* graph_ptr = graph.has_value() ? &*graph : nullptr;
    root["composition"] = composition_json(doc, comp_id, *comp, em, provider, graph_ptr, unknown);
    if (graph_ptr != nullptr) {
      // The non-root compositions the walk reached, keyed by their ordinals `"1"`..`"N"`
      // (doc 08 Principle 7, Decision 2): the root keeps its canonical home at
      // `root["composition"]` and holds the reserved ordinal `"0"`, so it is never a key
      // here. A document with no nesting reaches only the root and emits no
      // `compositions` key at all. A composition no reference reaches is simply never
      // enqueued -- dropped on save, the canonicalization Principle 4 blesses.
      const std::vector<ContentGraph::CompEntry>& comps = graph_ptr->compositions();
      if (comps.size() > 1) {
        json table = json::object();
        for (std::size_t i = 1; i < comps.size(); ++i) {
          table[std::to_string(i)] = composition_json(doc, comps[i].id, *comps[i].record, em,
                                                      provider, graph_ptr, unknown);
        }
        root["compositions"] = std::move(table);
      }
      // The shared-content table at document root, beside `composition` (omitted when
      // nothing is shared -- the canonical key sort places `composition` <
      // `compositions` < `contents`).
      if (std::optional<json> contents = graph_ptr->emit_contents(em); contents.has_value()) {
        root["contents"] = std::move(*contents);
      }
    }
  }

  // The document tier's preserved siblings, merged after `composition`/`contents` so both
  // win any collision; the `arbc` envelope's own unknowns ride nested under "arbc" and
  // recurse into the envelope object the core just built. The `contents` and
  // `compositions` tables are NOT sibling surfaces -- an entry no reference reaches is
  // dropped, the same canonicalization that renumbers hand-authored ids (Decision 5,
  // doc 08 Principle 4).
  if (unknown != nullptr) {
    merge_unknown_fields(root, unknown->find(UnknownScope::Document, ObjectId{}));
  }

  if (!em.ok()) {
    return unexpected(em.error());
  }

  // Non-throwing canonical dump: ascending-byte-order keys come free from
  // nlohmann's std::map-backed object, and numbers use the library's shortest
  // round-trip serialization. `error_handler_t::replace` guarantees no exception
  // crosses the API (there is no untrusted string in scope, so it never actually
  // substitutes). A trailing newline keeps the artifact diff-clean under VCS.
  std::string out = root.dump(2, ' ', false, json::error_handler_t::replace);
  out.push_back('\n');
  return out;
}

} // namespace

expected<std::string, SerializeError> serialize_document(const DocRoot& doc) {
  return serialize_impl(doc, /*provider=*/nullptr, /*meta=*/nullptr, /*codecs=*/nullptr,
                        /*unknown=*/nullptr);
}

expected<std::string, SerializeError> serialize_document(const DocRoot& doc,
                                                         const ContentBodyProvider& provider,
                                                         const ContentMetaProvider& meta,
                                                         const CodecTable& codecs,
                                                         const UnknownFieldStore* unknown) {
  return serialize_impl(doc, &provider, &meta, &codecs, unknown);
}

} // namespace arbc
