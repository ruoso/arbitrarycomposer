#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/media/audio_format.hpp>
#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp> // to_string(PixelFormat)
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/serialize/codec.hpp> // the content-body write routing this task lands
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

// The write face of doc 08 Principle 6 (serialize.sharing): the operator graph
// rooted at the document's layer contents, emitted structurally. A single pre-pass
// counts every `Content*`'s occurrences across the reachable graph (all layer roots
// bottom-to-top, each depth-first over `inputs()` in declared order) and records its
// `(kind, kind_version)`; content reached two or more times is *shared* and hoisted
// once into a document-level `contents` table under a deterministic first-encounter
// ordinal id, referenced by `{"$ref": id}` at every use site (Constraint 2). The
// same traversal order that drives counting drives id assignment, so output is
// byte-stable across runs and re-serializations (doc 08 Principle 5).
class ContentGraph {
public:
  ContentGraph(const ContentBodyProvider& provider, const ContentMetaProvider& meta,
               const CodecTable& codecs)
      : d_provider(provider), d_meta(meta), d_codecs(codecs) {}

  // Pre-pass over the document graph: count occurrences, record metadata, assign the
  // shared-content ids. Must run before any layer or `contents` emission.
  void build(const DocRoot& doc, ObjectId comp_id) {
    doc.for_each_layer_in(comp_id, [&](ObjectId lid) {
      const LayerRecord* lr = doc.find_layer(lid);
      if (lr == nullptr) {
        return;
      }
      const std::optional<ContentBody> body = d_provider(lr->content);
      if (!body.has_value()) {
        return;
      }
      visit(&body->content, body->kind, body->kind_version);
    });
    std::size_t next = 0;
    for (const Content* c : d_order) {
      if (d_counts[c] >= 2) {
        d_ids.emplace(c, std::to_string(next++));
        d_shared.push_back(c);
      }
    }
  }

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
  };

  void visit(const Content* c, std::string_view kind, std::string_view kind_version) {
    const auto [it, inserted] = d_counts.try_emplace(c, 0);
    it->second += 1;
    if (inserted) {
      d_meta_of.emplace(c, MetaEntry{std::string(kind), std::string(kind_version)});
      d_order.push_back(c);
    }
    if (it->second > 1) {
      return; // already descended into this subtree; count the extra edge, don't recurse
    }
    for (const ContentRef child : c->inputs()) {
      if (child == nullptr) {
        continue;
      }
      const std::optional<ContentMeta> m = d_meta(*child);
      if (m.has_value()) {
        visit(child, m->kind, m->kind_version);
      } else {
        visit(child, std::string_view{}, std::string_view{}); // let content_body_to_json fault
      }
    }
  }

  // One node's full inline body: its `{kind, kind_version, params}` leaf plus, when
  // `inputs()` is non-empty, an order-preserving `inputs` array whose slots are each
  // a `$ref` (shared) or a nested inline body (recursion). Omitted when empty.
  json emit_definition(const Content* c, Emitter& em) {
    const MetaEntry& m = d_meta_of.at(c);
    expected<json, SerializeError> leaf =
        content_body_to_json(m.kind, m.kind_version, *c, d_codecs);
    if (!leaf) {
      em.fail(leaf.error());
      return json::object();
    }
    json body = std::move(*leaf);
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
  std::unordered_map<const Content*, int> d_counts;        // occurrences across the graph
  std::unordered_map<const Content*, MetaEntry> d_meta_of; // (kind, kind_version) per node
  std::unordered_map<const Content*, std::string> d_ids;   // shared content -> `$ref` id
  std::vector<const Content*> d_order;                     // first-encounter traversal order
  std::vector<const Content*> d_shared;                    // shared contents, in id order
};

json layer_json(const LayerRecord& lr, ObjectId id, Emitter& em,
                const ContentBodyProvider* provider, ContentGraph* graph) {
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
  return o;
}

json composition_json(const DocRoot& doc, ObjectId comp_id, const CompositionRecord& comp,
                      Emitter& em, const ContentBodyProvider* provider, ContentGraph* graph) {
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
      layers.push_back(layer_json(*lr, lid, em, provider, graph));
    }
  });
  o["layers"] = std::move(layers);
  return o;
}

// The shared serialization core (Constraint 6): identical envelope + composition +
// placement walk for both public overloads; `provider`/`meta`/`codecs` are null on
// the content-free path (byte-identical to today) and non-null on the content-aware
// one, where the operator graph + shared `contents` table are emitted.
expected<std::string, SerializeError> serialize_impl(const DocRoot& doc,
                                                     const ContentBodyProvider* provider,
                                                     const ContentMetaProvider* meta,
                                                     const CodecTable* codecs) {
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
      graph.emplace(*provider, *meta, *codecs);
      graph->build(doc, comp_id);
    }
    ContentGraph* graph_ptr = graph.has_value() ? &*graph : nullptr;
    root["composition"] = composition_json(doc, comp_id, *comp, em, provider, graph_ptr);
    if (graph_ptr != nullptr) {
      // The shared-content table at document root, beside `composition` (omitted when
      // nothing is shared -- the canonical key sort places `composition` < `contents`).
      if (std::optional<json> contents = graph_ptr->emit_contents(em); contents.has_value()) {
        root["contents"] = std::move(*contents);
      }
    }
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
  return serialize_impl(doc, /*provider=*/nullptr, /*meta=*/nullptr, /*codecs=*/nullptr);
}

expected<std::string, SerializeError> serialize_document(const DocRoot& doc,
                                                         const ContentBodyProvider& provider,
                                                         const ContentMetaProvider& meta,
                                                         const CodecTable& codecs) {
  return serialize_impl(doc, &provider, &meta, &codecs);
}

} // namespace arbc
