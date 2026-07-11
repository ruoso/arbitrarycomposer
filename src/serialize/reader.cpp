#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/media/audio_format.hpp>
#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/serialize/codec.hpp>       // the content-body routing this task lands
#include <arbc/serialize/deserialize.hpp> // the content-body read hook (serialize.reader)
#include <arbc/serialize/reader.hpp>
#include <arbc/serialize/unknown_json.hpp> // the residual core (doc 08 Principle 4)

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace arbc {
namespace {

using json = nlohmann::json;

// The only format major this reader knows (doc 08 Principle 4; the writer emits
// `{"arbc":{"format":1}}`). A document carrying any other major is rejected
// wholesale -- never partially loaded.
constexpr std::int64_t k_format_major = 1;

// --- The core-owned key set at each tier (Constraint 2) ----------------------
// Declared ONCE and used by BOTH the parse and the unknown-field subtraction, so
// adding a core-owned key is a one-line edit in exactly one place. Everything a tier
// does NOT name here is preserved verbatim in the `UnknownFieldStore` and merged back
// on save (doc 08:88-98 Principle 4).
//
// `compositions` (ROOT) and `composition` (content BODY) are core-owned as of
// `serialize.compositions_table` (doc 08 Principle 7) and named here for exactly that
// reason: were they absent, the core would classify them as unknown fields, stash them,
// AND re-emit its own re-derived values -- double-emitting both.
//
// `working_space.primaries` is deliberately ABSENT from `k_working_space_keys`: the
// reader does not read it (it has a single value today and re-supplies the default),
// while the writer DOES emit it. So it is an unknown at load and a known at save -- the
// live never-shadow case (doc 08:96, Constraint 4).
constexpr std::string_view k_root_keys[] = {"arbc", "composition", "compositions", "contents"};
constexpr std::string_view k_envelope_keys[] = {"format"};
constexpr std::string_view k_composition_keys[] = {"canvas", "layers", "working_audio_format",
                                                   "working_space"};
constexpr std::string_view k_working_space_keys[] = {"format", "premultiplied", "transfer"};
constexpr std::string_view k_working_audio_format_keys[] = {"channels", "sample_rate"};
constexpr std::string_view k_layer_keys[] = {"audible",  "gain",      "opacity", "span",
                                             "time_map", "transform", "visible"};
constexpr std::string_view k_time_map_keys[] = {"in", "offset", "rate"};
constexpr std::string_view k_body_keys[] = {"$ref", "composition",  "inputs",
                                            "kind", "kind_version", "params"};

unexpected<ReaderError> fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// --- Safe, fuzz-hardened field accessors ------------------------------------
// Every access type-checks before extracting. A present-but-mistyped known field
// is treated leniently as absent (its still/identity default is supplied), so no
// hostile input can crash the loader (the precondition serialize.format_tests'
// fuzzing relies on); a malformed *envelope* is still a hard ReaderError above.

double num_or(const json& o, const char* key, double dflt) {
  const auto it = o.find(key);
  return (it != o.end() && it->is_number()) ? it->get<double>() : dflt;
}

bool bool_or(const json& o, const char* key, bool dflt) {
  const auto it = o.find(key);
  return (it != o.end() && it->is_boolean()) ? it->get<bool>() : dflt;
}

std::int64_t int_or(const json& o, const char* key, std::int64_t dflt) {
  const auto it = o.find(key);
  return (it != o.end() && it->is_number_integer()) ? it->get<std::int64_t>() : dflt;
}

// --- Value-type inverses of the writer's emit helpers (writer.cpp) ----------

PixelFormat parse_pixel_format(const std::string& token) {
  if (token == "rgba16f-linear-premul") {
    return PixelFormat::Rgba16fLinearPremul;
  }
  if (token == "rgba8-srgb") {
    return PixelFormat::Rgba8Srgb;
  }
  return PixelFormat::Rgba32fLinearPremul; // the doc-07 default token + fallback
}

TransferFunction parse_transfer(const std::string& token) {
  return token == "srgb" ? TransferFunction::Srgb : TransferFunction::Linear;
}

// working_space: the inverse of `working_space_json` (writer.cpp:89-96). Absent
// keys fall back to the doc-07 default components (`k_working_rgba32f`).
SurfaceFormat parse_surface_format(const json& o) {
  SurfaceFormat f; // defaults == k_working_rgba32f (Rgba32fLinearPremul, linear-srgb, premul)
  const auto fit = o.find("format");
  if (fit != o.end() && fit->is_string()) {
    f.pixel_format = parse_pixel_format(fit->get<std::string>());
  }
  const auto pit = o.find("premultiplied");
  if (pit != o.end() && pit->is_boolean()) {
    f.premultiplied = pit->get<bool>() ? Premultiplied::Yes : Premultiplied::No;
  }
  // `primaries` has a single value ("srgb") today, so it re-supplies the default;
  // it is read (and validated) so an unknown token never mutates the format.
  const auto tit = o.find("transfer");
  if (tit != o.end() && tit->is_string()) {
    f.color_space.transfer = parse_transfer(tit->get<std::string>());
  }
  return f;
}

// working_audio_format: the inverse of `working_audio_format_json` (writer.cpp:100-105).
AudioFormat parse_audio_format(const json& o) {
  AudioFormat f; // defaults == k_working_audio (48000, Stereo)
  const auto sit = o.find("sample_rate");
  if (sit != o.end() && sit->is_number_integer()) {
    const std::int64_t sr = sit->get<std::int64_t>();
    if (sr > 0 && sr <= static_cast<std::int64_t>(UINT32_MAX)) {
      f.sample_rate = static_cast<std::uint32_t>(sr);
    }
  }
  const auto cit = o.find("channels");
  if (cit != o.end() && cit->is_string()) {
    f.layout = cit->get<std::string>() == "mono" ? ChannelLayout::Mono : ChannelLayout::Stereo;
  }
  return f;
}

Affine parse_transform(const json& o) {
  const auto it = o.find("transform");
  if (it == o.end() || !it->is_array() || it->size() != 6) {
    return Affine::identity();
  }
  const json& a = *it;
  for (const json& e : a) {
    if (!e.is_number()) {
      return Affine::identity();
    }
  }
  return Affine{a[0].get<double>(), a[1].get<double>(), a[2].get<double>(),
                a[3].get<double>(), a[4].get<double>(), a[5].get<double>()};
}

TimeRange parse_span(const json& o) {
  const auto it = o.find("span");
  if (it == o.end() || !it->is_array() || it->size() != 2 || !(*it)[0].is_number_integer() ||
      !(*it)[1].is_number_integer()) {
    return TimeRange::all();
  }
  return TimeRange{Time{(*it)[0].get<std::int64_t>()}, Time{(*it)[1].get<std::int64_t>()}};
}

TimeMap parse_time_map(const json& o) {
  const auto it = o.find("time_map");
  if (it == o.end() || !it->is_object()) {
    return TimeMap{};
  }
  const json& m = *it;
  std::int64_t num = 1;
  std::int64_t den = 1;
  const auto rit = m.find("rate");
  if (rit != m.end() && rit->is_array() && rit->size() == 2 && (*rit)[0].is_number_integer() &&
      (*rit)[1].is_number_integer()) {
    num = (*rit)[0].get<std::int64_t>();
    den = (*rit)[1].get<std::int64_t>();
  }
  // `Rational{num, den}` asserts `den != 0` and that neither arg is INT64_MIN; a
  // hostile rate that would trip either falls back to the identity map so no
  // assertion crosses the boundary.
  if (den == 0 || num == INT64_MIN || den == INT64_MIN) {
    return TimeMap{};
  }
  return TimeMap{Time{int_or(m, "in", 0)}, Rational{num, den}, Time{int_or(m, "offset", 0)}};
}

// The core-owned placement of one layer, reconstructed with every field at its
// still/identity default so an omitted key restores exactly what the writer's
// omit-on-default emitted (Constraint 2).
struct LayerData {
  Affine transform{Affine::identity()};
  double opacity{1.0};
  bool visible{true};
  double gain{1.0};
  bool audible{true};
  TimeRange span{TimeRange::all()};
  TimeMap time_map{};
  // The layer's content position, captured verbatim when the layer carries content:
  // either an inline body (`{kind, kind_version, params, inputs}`) or a shared-content
  // reference (`{"$ref": id}`, serialize.sharing). `content_id`/`has_content_id` are
  // the sunk root the read recursion resolves it to (the same ObjectId for two layers
  // sharing one `$ref` -- intra-document dedup, Constraint 3). A layer with no content
  // leaves them defaulted, so it binds `ObjectId{}` exactly as before.
  bool has_content{false};
  json content_body;
  bool has_content_id{false};
  ObjectId content_id{};
  // The layer tier's preserved-and-ignored siblings (doc 08 Principle 4): every key of
  // the layer object the core names neither as placement NOR as a content-body key,
  // plus the residual of the known `time_map` sub-object nested back under its key. An
  // inline body shares the layer's JSON object, so an unrecognized key there is
  // indistinguishable from an unrecognized layer field and is recorded HERE (doc
  // 08:101-105, Decision 5).
  json unknown;
};

struct CompData {
  double canvas_w{0.0};
  double canvas_h{0.0};
  bool has_working_space{false};
  SurfaceFormat working_space{};
  bool has_working_audio_format{false};
  AudioFormat working_audio_format{};
  std::vector<LayerData> layers;
  // The composition tier's residual, with the `working_space` / `working_audio_format`
  // sub-residuals nested back under their keys.
  json unknown;
};

// The content position lives BESIDE placement in the layer object (doc 08:29-36).
// It is EITHER a `{"$ref": id}` into the document `contents` table (serialize.sharing:
// a shared content whose body was hoisted to document root) OR an inline body: the
// core-owned `kind`/`kind_version`, the kind-owned `params`, and the core-owned
// `inputs` edges.
//
// The body is the layer object MINUS the core-owned placement keys -- NOT a whitelist
// of the four body keys (Constraint 1). The old 4-key filter truncated the body before
// anything downstream could preserve it, which is why an unknown-kind placeholder at a
// layer position silently dropped its unrecognized siblings. Subtracting placement
// instead of whitelisting body keys hands the placeholder every key it should hold,
// while keeping the core's own placement out of the opaque stash -- re-emitting a stale
// `opacity` or an explicitly-written default `gain` from the load-time body would fight
// both omit-on-default and the writer's live value (Decision 1's rejected alternative).
bool extract_content_body(const json& o, json& out) {
  if (!o.contains("$ref") && !o.contains("kind")) {
    return false; // a placement-only layer (the writer's v1 output) has no content
  }
  out = json::object();
  for (auto it = o.begin(); it != o.end(); ++it) {
    if (!names_key(k_layer_keys, std::string_view(it.key()))) {
      out[it.key()] = it.value();
    }
  }
  return true;
}

LayerData parse_layer(const json& o) {
  LayerData ld;
  ld.transform = parse_transform(o);
  ld.opacity = num_or(o, "opacity", 1.0);
  ld.visible = bool_or(o, "visible", true);
  ld.gain = num_or(o, "gain", 1.0);
  ld.audible = bool_or(o, "audible", true);
  ld.span = parse_span(o);
  ld.time_map = parse_time_map(o);
  ld.has_content = extract_content_body(o, ld.content_body);

  // The layer residual: neither a placement key nor a content-body key (Decision 5).
  ld.unknown = json::object();
  for (auto it = o.begin(); it != o.end(); ++it) {
    const std::string_view key(it.key());
    if (!names_key(k_layer_keys, key) && !names_key(k_body_keys, key)) {
      ld.unknown[it.key()] = it.value();
    }
  }
  if (json tm = unknown_residual_at(o, "time_map", k_time_map_keys); !tm.empty()) {
    ld.unknown["time_map"] = std::move(tm);
  }
  return ld;
}

void parse_composition(const json& c, CompData& out) {
  // canvas: `[x, y, w, h]` -- read w, h (the writer emits x == y == 0, integer
  // extents); an absent/short/mistyped array leaves the 0x0 default.
  const auto cvit = c.find("canvas");
  if (cvit != c.end() && cvit->is_array() && cvit->size() >= 4 && (*cvit)[2].is_number() &&
      (*cvit)[3].is_number()) {
    out.canvas_w = (*cvit)[2].get<double>();
    out.canvas_h = (*cvit)[3].get<double>();
  }
  const auto wsit = c.find("working_space");
  if (wsit != c.end() && wsit->is_object()) {
    out.has_working_space = true;
    out.working_space = parse_surface_format(*wsit);
  }
  const auto wait = c.find("working_audio_format");
  if (wait != c.end() && wait->is_object()) {
    out.has_working_audio_format = true;
    out.working_audio_format = parse_audio_format(*wait);
  }
  const auto lit = c.find("layers");
  if (lit != c.end() && lit->is_array()) {
    for (const json& lj : *lit) {
      if (lj.is_object()) {
        out.layers.push_back(parse_layer(lj));
      }
    }
  }

  // The composition residual, recursing into the two known sub-objects so an unknown key
  // INSIDE `working_space` / `working_audio_format` survives too (doc 08:92).
  out.unknown = unknown_residual(c, k_composition_keys);
  if (json ws = unknown_residual_at(c, "working_space", k_working_space_keys); !ws.empty()) {
    out.unknown["working_space"] = std::move(ws);
  }
  if (json wa = unknown_residual_at(c, "working_audio_format", k_working_audio_format_keys);
      !wa.empty()) {
    out.unknown["working_audio_format"] = std::move(wa);
  }
}

// The read face of doc 08 Principle 7 (serialize.compositions_table): the document is a
// GRAPH of compositions -- the root at `root["composition"]` holding the reserved
// ordinal `"0"`, plus the non-root ones in the `compositions` table under `"1"`..`"N"`
// -- and a nesting content names its child through the core-owned `"composition"` field
// on its body.
//
// This resolver exists to cut the ordering knot (Decision 4): a nesting `Content` cannot
// be constructed without its child's `ObjectId`, yet the child's `CompositionRecord`
// cannot exist before its layers, which cannot exist before their contents. So it
// ALLOCATES each reachable composition's id up front -- `Model::allocate_id()` is a bare
// monotonic counter bump that installs no record, so nothing is mutated and a failed load
// still leaves the model at revision 0 (Constraint 5) -- and merely ENQUEUES the body for
// the caller's breadth-first loop to build later.
//
// Allocate-then-enqueue is also exactly what makes a CYCLE terminate (Constraint 7): a
// back-edge to an in-flight composition returns its pre-allocated id IMMEDIATELY, without
// re-entering its layers -- so a legal Droste never trips `RefResolver`'s in-progress set
// and is never misreported as an (illegal) operator-input cycle. The two cycle notions
// keep entirely separate state.
//
// The root is allocated FIRST, so a loaded document always satisfies the model's
// lowest-id-is-root rule (Constraint 8, `find_first_composition`).
class CompResolver {
public:
  struct Pending {
    ObjectId id;
    const json* body;
  };

  CompResolver(const json* table, Model& model) : d_table(table), d_model(model) {}

  // Seed the walk with the root composition: ordinal `"0"`, the lowest id in the
  // document, and never a key in the `compositions` table. `seeded` is a caller-
  // PRE-ALLOCATED id (runtime.nested_external_ref Constraint 6) -- the external
  // loader hands the child's root id in, having recorded it in its resolved-URI map
  // BEFORE these bytes were parsed, which is what makes a cross-document cycle
  // terminate. An invalid `seeded` allocates one here, as it always did.
  ObjectId add_root(const json& body, ObjectId seeded) {
    const ObjectId id = seeded.valid() ? seeded : d_model.allocate_id();
    d_allocated.emplace("0", id);
    d_pending.push_back(Pending{id, &body});
    return id;
  }

  // The `ObjectId` a body's `"composition": "<key>"` names, allocating + enqueueing the
  // composition on first encounter. `"0"` resolves to the root (already allocated). A key
  // absent from the table is a dangling reference; a non-object entry is malformed. Both
  // are VALUES surfaced before any model mutation (Constraint 6).
  expected<ObjectId, ReaderError> resolve(const std::string& key) {
    if (const auto it = d_allocated.find(key); it != d_allocated.end()) {
      return it->second; // the Droste back-edge lands here, without re-entering the body
    }
    if (d_table == nullptr) {
      return fail(ReaderError::Kind::UnresolvableReference, "/compositions/" + key);
    }
    const auto bit = d_table->find(key);
    if (bit == d_table->end()) {
      return fail(ReaderError::Kind::UnresolvableReference, "/compositions/" + key);
    }
    if (!bit->is_object()) {
      return fail(ReaderError::Kind::MalformedField, "/compositions/" + key);
    }
    const ObjectId id = d_model.allocate_id();
    d_allocated.emplace(key, id);
    d_pending.push_back(Pending{id, &*bit});
    return id;
  }

  // The breadth-first worklist, growing while the caller walks it by index.
  std::size_t size() const { return d_pending.size(); }
  const Pending& at(std::size_t i) const { return d_pending[i]; }

private:
  const json* d_table;
  Model& d_model;
  std::unordered_map<std::string, ObjectId> d_allocated; // table key -> pre-allocated id
  std::vector<Pending> d_pending;                        // first-encounter order, root first
};

// The read face of doc 08 Principle 6 (serialize.sharing): resolve a content
// position -- an inline body or a `{"$ref": id}` into the document `contents` table
// -- into a live, sunk `Content`, wiring each node's input edges bottom-up and
// deduplicating shared references so one `contents` entry yields exactly ONE built
// `Content` shared by every use site (Constraint 3). A `$ref` that is dangling (id
// absent from `contents`) or closes an operator-input cycle is a
// `UnresolvableReference` value with no model mutation (Decision 7); the whole
// resolution runs BEFORE `load_baseline`, so any failure leaves the model empty.
class RefResolver {
public:
  RefResolver(const json* contents, CompResolver* comps, const CodecTable& codecs,
              const Registry& registry, LoadContext& ctx, const ContentSink& sink,
              UnknownFieldStore& unknown)
      : d_contents(contents), d_comps(comps), d_codecs(codecs), d_registry(registry), d_ctx(ctx),
        d_sink(sink), d_unknown(unknown) {}

  // Resolve one content position (inline body OR `{"$ref": id}`) to a sunk node.
  // `layer_position` is true only for a LAYER's content position, where the body shares
  // the layer's JSON object: its unknown siblings are recorded as LAYER fields by
  // `parse_layer`, so this node stashes only its `params` residual (Decision 5). A body
  // standing alone -- in the `contents` table or an `inputs` slot -- stashes both.
  expected<SunkContent, ReaderError> resolve(const json& node, bool layer_position = false) {
    if (const auto rit = node.find("$ref"); rit != node.end()) {
      // `$ref` ids are decimal-string handles the writer derives (Decision 2). A
      // non-string, an absent id, or a re-entry into an in-progress id (a cycle) is
      // an UnresolvableReference value -- never a throw, never a partial graph.
      if (!rit->is_string()) {
        return fail(ReaderError::Kind::UnresolvableReference, "/$ref");
      }
      const std::string id = rit->get<std::string>();
      if (const auto cit = d_cache.find(id); cit != d_cache.end()) {
        return cit->second; // intra-document dedup: one live Content per shared id
      }
      if (d_in_progress.count(id) != 0) {
        return fail(ReaderError::Kind::UnresolvableReference, "/contents/" + id); // cycle
      }
      if (d_contents == nullptr) {
        return fail(ReaderError::Kind::UnresolvableReference, "/contents/" + id); // no table
      }
      const auto bit = d_contents->find(id);
      if (bit == d_contents->end()) {
        return fail(ReaderError::Kind::UnresolvableReference, "/contents/" + id); // dangling
      }
      if (!bit->is_object()) {
        return fail(ReaderError::Kind::MalformedField, "/contents/" + id);
      }
      d_in_progress.insert(id);
      // A `contents`-table body always stands alone, however it is reached.
      expected<SunkContent, ReaderError> built = build(*bit, /*layer_position=*/false);
      d_in_progress.erase(id);
      if (!built) {
        return built;
      }
      d_cache.emplace(id, *built);
      return built;
    }
    return build(node, layer_position);
  }

private:
  // Build one inline body: resolve its input children FIRST (recursing, so shared
  // children dedup and the graph is built bottom-up), route the `{kind, params}` node
  // through the codec table with the built inputs in hand (Decision 4), and hand the
  // node to the sink, which owns it and yields its live `Content*`.
  expected<SunkContent, ReaderError> build(const json& body, bool layer_position) {
    // A kind names its child through `composition_ref()` OR takes authored `inputs`, never
    // both (doc 08 Principle 7's closing rule): a nesting content's input edges ARE the
    // child composition's layers, so a body carrying both edge sets asserts something the
    // format cannot express. Reject it rather than silently dropping one half -- and reject
    // it HERE, before either edge set is resolved, so no child is sunk and the model is
    // left unmutated. The writer has never emitted such a body.
    if (body.is_object() && body.contains("inputs") && body.contains("composition")) {
      return fail(ReaderError::Kind::MalformedField, "");
    }
    std::vector<ContentRef> input_ptrs;
    if (const auto iit = body.find("inputs"); iit != body.end()) {
      if (!iit->is_array()) {
        return fail(ReaderError::Kind::MalformedField, "/inputs");
      }
      for (const json& slot : *iit) {
        if (!slot.is_object()) {
          return fail(ReaderError::Kind::MalformedField, "/inputs"); // a slot is a body or $ref
        }
        expected<SunkContent, ReaderError> child = resolve(slot); // a slot body stands alone
        if (!child) {
          return unexpected(child.error());
        }
        input_ptrs.push_back(child->live);
      }
    }
    // The core-owned child-composition reference (doc 08 Principle 7): resolved to a
    // pre-allocated `ObjectId` HERE, before the node is constructed, because a nesting
    // kind takes its child at construction. The resolve never recurses into the child's
    // layers -- so a body that closes a composition cycle costs nothing and, crucially,
    // never re-enters `d_in_progress` below (Constraint 7). Threaded to a known kind's
    // codec and, for an unknown kind, onto its placeholder -- a missing plugin never
    // orphans the composition it embeds (Decision 6).
    ObjectId child_composition;
    if (const auto cit = body.find("composition"); cit != body.end()) {
      if (!cit->is_string()) {
        return fail(ReaderError::Kind::MalformedField, "/composition");
      }
      const std::string key = cit->get<std::string>();
      if (d_comps == nullptr) {
        return fail(ReaderError::Kind::UnresolvableReference, "/compositions/" + key);
      }
      expected<ObjectId, ReaderError> resolved = d_comps->resolve(key);
      if (!resolved) {
        return unexpected(resolved.error());
      }
      child_composition = *resolved;
    }
    // `params_residual` is Decision 4: the routing runs the codec's OWN serializer back
    // over the content it just built and hands us `params_in - params_out` -- exactly the
    // keys the codec did not consume, frozen at load time so a later edit that clears an
    // optional param can never resurrect it. Empty for an unknown kind (the placeholder
    // already holds the whole body verbatim) and for a codec that cannot re-serialize.
    json params_residual;
    expected<std::unique_ptr<Content>, ReaderError> produced = content_body_from_json(
        body, input_ptrs, child_composition, d_codecs, d_registry, d_ctx, &params_residual);
    if (!produced) {
      return unexpected(produced.error());
    }
    const SunkContent sunk = d_sink(std::move(*produced));

    // The content residual, keyed by the ObjectId the sink just minted (Decision 3):
    // never by `Content*`, so no entry can ever alias a later object.
    json stash = layer_position ? json::object() : unknown_residual(body, k_body_keys);
    if (!params_residual.is_null() && !params_residual.empty()) {
      stash["params"] = std::move(params_residual);
    }
    d_unknown.set(UnknownScope::Content, sunk.id, to_unknown_fields(stash));
    return sunk;
  }

  static unexpected<ReaderError> fail(ReaderError::Kind kind, std::string path) {
    return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
  }

  const json* d_contents;
  CompResolver* d_comps; // the composition id space (nullable: a document with no root)
  const CodecTable& d_codecs;
  const Registry& d_registry;
  LoadContext& d_ctx;
  const ContentSink& d_sink;
  UnknownFieldStore& d_unknown;                         // content-scope stashes, by ObjectId
  std::unordered_map<std::string, SunkContent> d_cache; // shared id -> one sunk node
  std::unordered_set<std::string> d_in_progress;        // ids on the resolution stack
};

// One composition of a document, resolved but not yet materialized into the model.
struct ResolvedComp {
  ObjectId id;
  CompData data;
};

// A whole document's composition graph, resolved into intermediates and ready to
// install. Split out of `load_document` (runtime.nested_external_ref) so an EXTERNAL
// child document can be read through exactly the same machinery and installed into an
// EXISTING model under a caller-seeded root id -- "the same mechanism plus a loader"
// (doc 05:47-52) is a promise about the reader as much as the compositor.
struct DocumentGraph {
  ObjectId root{}; // the root composition's id; invalid == the document has none
  std::vector<ResolvedComp> comps;
  UnknownFieldStore staged;
};

// Non-throwing parse + envelope check (serialize.json_dep discipline): a syntax error
// yields a discarded value, never a thrown exception (Constraint 3). Envelope:
// `{"arbc":{"format":<major>}}` -- a missing key is MissingRequiredField, a present-but-
// mistyped one is MalformedField, and a known-but-unsupported major is a clean rejection
// with no document mutation (Constraint 4, Principle 4).
expected<json, ReaderError> parse_document(std::string_view input) {
  json root = json::parse(input.begin(), input.end(), /*cb=*/nullptr,
                          /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    return fail(ReaderError::Kind::MalformedJson, "");
  }
  if (!root.is_object()) {
    return fail(ReaderError::Kind::MalformedField, ""); // a non-object envelope
  }
  const auto ait = root.find("arbc");
  if (ait == root.end()) {
    return fail(ReaderError::Kind::MissingRequiredField, "/arbc");
  }
  if (!ait->is_object()) {
    return fail(ReaderError::Kind::MalformedField, "/arbc");
  }
  const auto fit = ait->find("format");
  if (fit == ait->end()) {
    return fail(ReaderError::Kind::MissingRequiredField, "/arbc/format");
  }
  if (!fit->is_number_integer()) {
    return fail(ReaderError::Kind::MalformedField, "/arbc/format");
  }
  if (fit->get<std::int64_t>() != k_format_major) {
    return fail(ReaderError::Kind::UnknownFormatMajor, "/arbc/format");
  }
  return root;
}

// Resolve `root`'s whole composition graph into intermediates BEFORE touching the model
// (Decision 5, 7), so a malformed body, a dangling `$ref`, or a `$ref` cycle returns a
// `ReaderError` with no composition installed. `seeded_root` is the id the root
// composition must take: `Model::allocate_id()`'s next value for a fresh document, or --
// for an external child -- the id its embedding `NestedContent` already holds, which is
// how the loader cuts the cross-document cycle knot before parsing these bytes
// (compositions_table Decision 4, lifted across documents).
expected<DocumentGraph, ReaderError> resolve_graph(const json& root, const Registry& registry,
                                                   const CodecTable& codecs, LoadContext& ctx,
                                                   const ContentSink& sink, Model& into,
                                                   ObjectId seeded_root) {
  // The ROOT composition (doc 08 Principle 7: "a document holds exactly one root
  // `composition`").
  const auto cit = root.find("composition");
  if (cit != root.end() && !cit->is_object()) {
    return fail(ReaderError::Kind::MalformedField, "/composition");
  }
  const bool has_composition = cit != root.end();

  // The optional document-level `compositions` table (serialize.compositions_table):
  // the NON-ROOT compositions, keyed `"1"`..`"N"` (doc 08 Principle 7). A present-but-
  // non-object `compositions` is malformed. An entry keyed `"0"` claims the root's
  // reserved ordinal, which always resolves to the root -- so the entry could only ever
  // be shadowed into oblivion. Rejecting it beats silently deleting a composition its
  // author meant something by (Decision 3); a table entry that no reference reaches is a
  // different thing entirely and is simply ignored on load, dropped on save (Principle 4).
  const json* compositions_table = nullptr;
  if (const auto mit = root.find("compositions"); mit != root.end()) {
    if (!mit->is_object()) {
      return fail(ReaderError::Kind::MalformedField, "/compositions");
    }
    if (mit->contains("0")) {
      return fail(ReaderError::Kind::MalformedField, "/compositions/0");
    }
    compositions_table = &*mit;
  }

  // The optional document-level `contents` table (serialize.sharing): shared content
  // hoisted to document root, keyed by `$ref` id (doc 08 Principle 6). A present-but-
  // non-object `contents` is malformed; it lives beside `composition`.
  const json* contents_table = nullptr;
  if (const auto ctit = root.find("contents"); ctit != root.end()) {
    if (!ctit->is_object()) {
      return fail(ReaderError::Kind::MalformedField, "/contents");
    }
    contents_table = &*ctit;
  }

  // Routing happens only when a `ContentSink` is installed to take ownership of every
  // produced node; the content-free overload passes a null sink and an empty codec table,
  // so a placement-only document is byte-identical to before. The recursion routes by
  // kind id through `codecs` (known -> live Content; unknown -> placeholder), resolves
  // `$ref`/`inputs` against `contents_table` with intra-document dedup, and wires input
  // edges bottom-up (registry/ctx threaded through).
  // Every tier's unknown siblings are staged here and published to the caller's store
  // ONLY on a fully successful load, mirroring the "model unmutated on error" discipline
  // (Decision 3; Constraint 10 -- a stash is never an error path of its own).
  DocumentGraph out;
  out.staged.set(UnknownScope::Document, ObjectId{}, [&] {
    json root_unknown = unknown_residual(root, k_root_keys);
    if (json env = unknown_residual_at(root, "arbc", k_envelope_keys); !env.empty()) {
      root_unknown["arbc"] = std::move(env); // the envelope's own unknown siblings
    }
    return to_unknown_fields(root_unknown);
  }());

  // One breadth-first pass over the composition graph (compositions_table Constraint 3):
  // the root is seeded FIRST -- so, on a fresh document, it is the lowest-id composition,
  // the model's root rule, guaranteed rather than assumed (Constraint 8) -- then each
  // composition's layers are resolved in turn, and a nesting content's `"composition"`
  // field enqueues its child, which this same loop reaches. `d_pending` grows while we
  // walk it by index; a cycle simply re-uses an already-allocated id and enqueues
  // nothing, so the loop terminates. ONE `RefResolver` spans every composition, so a
  // content shared across two of them dedups into a single `contents` entry and a single
  // live node (Constraint 4).
  CompResolver comp_resolver(compositions_table, into);
  RefResolver resolver(contents_table, &comp_resolver, codecs, registry, ctx, sink, out.staged);
  if (has_composition) {
    out.root = comp_resolver.add_root(*cit, seeded_root);
    for (std::size_t i = 0; i < comp_resolver.size(); ++i) {
      // BY VALUE: `resolve` below can enqueue and reallocate the pending vector.
      const CompResolver::Pending pending = comp_resolver.at(i);
      ResolvedComp rc;
      rc.id = pending.id;
      parse_composition(*pending.body, rc.data);
      if (sink) {
        for (LayerData& ld : rc.data.layers) {
          if (!ld.has_content) {
            continue;
          }
          expected<SunkContent, ReaderError> produced =
              resolver.resolve(ld.content_body, /*layer_position=*/true);
          if (!produced) {
            return unexpected(produced.error()); // no composition installed yet
          }
          ld.content_id = produced->id;
          ld.has_content_id = true;
        }
      }
      out.comps.push_back(std::move(rc));
    }
  }
  return out;
}

// Materialize the resolved graph inside `txn`. The nodes are already sunk (owned by the
// caller) in the resolution phase; a layer root binds its sunk `content_id` (the same id
// for two layers sharing one content, Constraint 3), a placement-only layer binds the
// invalid `ObjectId{}` placeholder exactly as the content-free path does. Every reachable
// composition is materialized under the id the resolver pre-allocated for it -- the same
// id a nesting content already holds (Decision 4).
void install_graph(Model::Transaction& txn, DocumentGraph& g) {
  for (const ResolvedComp& rc : g.comps) {
    const ObjectId cid = txn.add_composition(rc.id, rc.data.canvas_w, rc.data.canvas_h);
    // The composition's own unknown siblings stash under its pre-allocated ObjectId; the
    // layer stashes can only be keyed here, since their ObjectIds are minted by this
    // transaction (Decision 3).
    g.staged.set(UnknownScope::Composition, cid, to_unknown_fields(rc.data.unknown));
    if (rc.data.has_working_space) {
      txn.set_working_space(cid, rc.data.working_space);
    }
    if (rc.data.has_working_audio_format) {
      txn.set_working_audio_format(cid, rc.data.working_audio_format);
    }
    for (const LayerData& ld : rc.data.layers) {
      const ObjectId content_id = ld.has_content_id ? ld.content_id : ObjectId{};
      const ObjectId lid = txn.add_layer(content_id, ld.transform, ld.opacity);
      g.staged.set(UnknownScope::Layer, lid, to_unknown_fields(ld.unknown));
      // Re-apply exactly the omit-on-default twins the writer would have emitted; a
      // field left at its default stays diff-clean (Constraint 2).
      if (ld.gain != 1.0) {
        txn.set_gain(lid, ld.gain);
      }
      if (!ld.visible) {
        txn.set_visible(lid, false);
      }
      if (!ld.audible) {
        txn.set_audible(lid, false);
      }
      if (!(ld.span == TimeRange::all())) {
        txn.set_span(lid, ld.span);
      }
      if (!(ld.time_map == TimeMap{})) {
        txn.set_time_map(lid, ld.time_map);
      }
      txn.attach_layer(cid, lid);
    }
  }
}

} // namespace

expected<std::monostate, ReaderError>
load_document(std::string_view input, const Registry& registry, const CodecTable& codecs,
              LoadContext& ctx, const ContentSink& sink, Model& into, UnknownFieldStore* unknown,
              ObjectId root_composition) {
  const expected<json, ReaderError> root = parse_document(input);
  if (!root) {
    return unexpected(root.error());
  }
  // The caller may PRE-ALLOCATE the root composition's id (runtime.nested_external_ref
  // Constraint 6): the external loader needs it in hand before the read begins, so it can
  // seed its resolved-URI map with "this document -> this composition" and collapse a
  // document that references ITSELF onto the in-document Droste case exactly. An invalid
  // id means "allocate one", which is what every other caller passes and what keeps the
  // root the lowest-id composition (Constraint 8).
  expected<DocumentGraph, ReaderError> graph =
      resolve_graph(*root, registry, codecs, ctx, sink, into, root_composition);
  if (!graph) {
    return unexpected(graph.error());
  }

  // Install the reconstructed graph as the version-0 baseline with an empty journal
  // (Decision 3).
  const expected<std::monostate, PoolError> installed =
      into.load_baseline([&](Model::Transaction& txn) { install_graph(txn, *graph); });
  if (!installed) {
    // Arena exhaustion during the baseline install (a resource error, not a
    // format one); the model is left unmutated. Surfaced on the reader's error
    // channel so no allocation failure escapes as an exception (doc 10).
    return fail(ReaderError::Kind::MalformedField, "/composition");
  }
  // The load succeeded: publish the staged unknowns wholesale. A `nullptr` store is
  // today's lossy behavior, so no existing caller changes shape (Decision 3).
  if (unknown != nullptr) {
    *unknown = std::move(graph->staged);
  }
  return std::monostate{};
}

expected<ObjectId, ReaderError> load_composition(std::string_view input, const Registry& registry,
                                                 const CodecTable& codecs, LoadContext& ctx,
                                                 const ContentSink& sink, Model& into,
                                                 ObjectId root_composition,
                                                 std::span<const Damage> damage) {
  // The same read as `load_document`, with two differences that are the whole point
  // (runtime.nested_external_ref Decision 1): the graph lands in an EXISTING model under
  // a caller-seeded root id, through an ordinary transaction rather than
  // `load_baseline` -- which would republish the host document at revision 0 and discard
  // it -- and the model ROOT is left alone, because `find_first_composition` names the
  // lowest-id composition and the host's root was allocated before any child's.
  //
  // The child's unknown-field residuals are deliberately DROPPED: they belong to the
  // other document, which this one never re-emits (its contents are named by URI, doc 08
  // Principle 3), and the document-scope stash would collide with the host's own.
  const expected<json, ReaderError> root = parse_document(input);
  if (!root) {
    return unexpected(root.error());
  }
  expected<DocumentGraph, ReaderError> graph =
      resolve_graph(*root, registry, codecs, ctx, sink, into, root_composition);
  if (!graph) {
    return unexpected(graph.error());
  }
  if (!graph->root.valid()) {
    // A document with no root composition has nothing to embed. Not an error in itself
    // -- the bytes are legal -- but there is no child, so the caller reports the
    // reference unavailable (Decision 6).
    return ObjectId{};
  }
  Model::Transaction txn = into.transact("load external composition");
  install_graph(txn, *graph);
  // The caller's damage rides THIS commit, not a later one (runtime.async_external_load
  // Decision 3): a structural publish whose damage set is empty is a revision the frame loop
  // has no reason to wake for.
  for (const Damage& d : damage) {
    txn.add_damage(d);
  }
  if (!txn.commit()) {
    return fail(ReaderError::Kind::MalformedField, "/composition"); // arena exhaustion
  }
  return graph->root;
}

// The content-free entry point: reconstruct only the envelope, composition, and
// core-owned placement (its historical scope). An empty codec table and a null
// content sink make the content-aware path above a no-op -- every layer binds
// `ObjectId{}` -- so a placement-only document loads byte-identically to before. It
// carries no `UnknownFieldStore` either: with no store to hand a stash to, and no
// content-aware writer overload to merge it back, preservation rides the content-aware
// pair only (Decision 3).
expected<std::monostate, ReaderError>
load_document(std::string_view input, const Registry& registry, LoadContext& ctx, Model& into) {
  const CodecTable no_codecs;
  return load_document(input, registry, no_codecs, ctx, ContentSink{}, into, /*unknown=*/nullptr);
}

} // namespace arbc
