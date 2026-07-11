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
// NOTE for `serialize.compositions_table` (Constraint 7): that sibling task introduces
// `compositions` at the ROOT and `composition` on a content BODY. Whichever of the two
// lands second must add those names here -- otherwise they would be classified as
// unknown fields, stashed, AND emitted by the core, i.e. double-emitted.
//
// `working_space.primaries` is deliberately ABSENT from `k_working_space_keys`: the
// reader does not read it (it has a single value today and re-supplies the default),
// while the writer DOES emit it. So it is an unknown at load and a known at save -- the
// live never-shadow case (doc 08:96, Constraint 4).
constexpr std::string_view k_root_keys[] = {"arbc", "composition", "contents"};
constexpr std::string_view k_envelope_keys[] = {"format"};
constexpr std::string_view k_composition_keys[] = {"canvas", "layers", "working_audio_format",
                                                   "working_space"};
constexpr std::string_view k_working_space_keys[] = {"format", "premultiplied", "transfer"};
constexpr std::string_view k_working_audio_format_keys[] = {"channels", "sample_rate"};
constexpr std::string_view k_layer_keys[] = {"audible",  "gain",      "opacity", "span",
                                             "time_map", "transform", "visible"};
constexpr std::string_view k_time_map_keys[] = {"in", "offset", "rate"};
constexpr std::string_view k_body_keys[] = {"$ref", "inputs", "kind", "kind_version", "params"};

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
  RefResolver(const json* contents, const CodecTable& codecs, const Registry& registry,
              LoadContext& ctx, const ContentSink& sink, UnknownFieldStore& unknown)
      : d_contents(contents), d_codecs(codecs), d_registry(registry), d_ctx(ctx), d_sink(sink),
        d_unknown(unknown) {}

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
    // `params_residual` is Decision 4: the routing runs the codec's OWN serializer back
    // over the content it just built and hands us `params_in - params_out` -- exactly the
    // keys the codec did not consume, frozen at load time so a later edit that clears an
    // optional param can never resurrect it. Empty for an unknown kind (the placeholder
    // already holds the whole body verbatim) and for a codec that cannot re-serialize.
    json params_residual;
    expected<std::unique_ptr<Content>, ReaderError> produced =
        content_body_from_json(body, input_ptrs, d_codecs, d_registry, d_ctx, &params_residual);
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
  const CodecTable& d_codecs;
  const Registry& d_registry;
  LoadContext& d_ctx;
  const ContentSink& d_sink;
  UnknownFieldStore& d_unknown;                         // content-scope stashes, by ObjectId
  std::unordered_map<std::string, SunkContent> d_cache; // shared id -> one sunk node
  std::unordered_set<std::string> d_in_progress;        // ids on the resolution stack
};

} // namespace

expected<std::monostate, ReaderError>
load_document(std::string_view input, const Registry& registry, const CodecTable& codecs,
              LoadContext& ctx, const ContentSink& sink, Model& into, UnknownFieldStore* unknown) {
  // Non-throwing parse (serialize.json_dep discipline): a syntax error yields a
  // discarded value, never a thrown exception (Constraint 3).
  const json root = json::parse(input.begin(), input.end(), /*cb=*/nullptr,
                                /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    return fail(ReaderError::Kind::MalformedJson, "");
  }
  if (!root.is_object()) {
    return fail(ReaderError::Kind::MalformedField, ""); // a non-object envelope
  }

  // Envelope: `{"arbc":{"format":<major>}}`. A missing key is MissingRequiredField;
  // a present-but-mistyped one is MalformedField; a known-but-unsupported major is
  // a clean rejection with no document mutation (Constraint 4, Principle 4).
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

  // Parse the whole composition into an intermediate BEFORE touching the model, so
  // any error leaves the target `Model` unmutated (revision 0, empty).
  CompData comp;
  bool has_composition = false;
  const auto cit = root.find("composition");
  if (cit != root.end()) {
    if (!cit->is_object()) {
      return fail(ReaderError::Kind::MalformedField, "/composition");
    }
    has_composition = true;
    parse_composition(*cit, comp);
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

  // Reconstruct each layer's content graph BEFORE touching the model (Decision 5, 7),
  // so a malformed body, a dangling `$ref`, or a `$ref` cycle returns a `ReaderError`
  // with the target `Model` still empty. Routing happens only when a `ContentSink` is
  // installed to take ownership of every produced node; the content-free overload
  // passes a null sink and an empty codec table, so a placement-only document is
  // byte-identical to before. The recursion routes by kind id through `codecs` (known
  // -> live Content; unknown -> placeholder), resolves `$ref`/`inputs` against
  // `contents_table` with intra-document dedup, and wires input edges bottom-up
  // (registry/ctx threaded through).
  // Every tier's unknown siblings are staged here and published to the caller's store
  // ONLY on a fully successful load, mirroring the "model unmutated on error" discipline
  // (Decision 3; Constraint 10 -- a stash is never an error path of its own).
  UnknownFieldStore staged;
  staged.set(UnknownScope::Document, ObjectId{}, [&] {
    json root_unknown = unknown_residual(root, k_root_keys);
    if (json env = unknown_residual_at(root, "arbc", k_envelope_keys); !env.empty()) {
      root_unknown["arbc"] = std::move(env); // the envelope's own unknown siblings
    }
    return to_unknown_fields(root_unknown);
  }());

  if (sink) {
    RefResolver resolver(contents_table, codecs, registry, ctx, sink, staged);
    for (LayerData& ld : comp.layers) {
      if (!ld.has_content) {
        continue;
      }
      expected<SunkContent, ReaderError> produced =
          resolver.resolve(ld.content_body, /*layer_position=*/true);
      if (!produced) {
        return unexpected(produced.error()); // model unmutated: nothing installed yet
      }
      ld.content_id = produced->id;
      ld.has_content_id = true;
    }
  }

  // Install the reconstructed graph as the version-0 baseline with an empty
  // journal (Decision 3). The nodes are already sunk (owned by the caller) in the
  // resolution phase above; a layer root binds its sunk `content_id` (the same id for
  // two layers sharing one content, Constraint 3), a placement-only layer binds the
  // invalid `ObjectId{}` placeholder exactly as the content-free path does.
  const expected<std::monostate, PoolError> installed =
      into.load_baseline([&](Model::Transaction& txn) {
        if (!has_composition) {
          return;
        }
        const ObjectId cid = txn.add_composition(comp.canvas_w, comp.canvas_h);
        // The composition/layer stashes can only be keyed here: their ObjectIds are
        // minted by this transaction (Decision 3).
        staged.set(UnknownScope::Composition, cid, to_unknown_fields(comp.unknown));
        if (comp.has_working_space) {
          txn.set_working_space(cid, comp.working_space);
        }
        if (comp.has_working_audio_format) {
          txn.set_working_audio_format(cid, comp.working_audio_format);
        }
        for (const LayerData& ld : comp.layers) {
          const ObjectId content_id = ld.has_content_id ? ld.content_id : ObjectId{};
          const ObjectId lid = txn.add_layer(content_id, ld.transform, ld.opacity);
          staged.set(UnknownScope::Layer, lid, to_unknown_fields(ld.unknown));
          // Re-apply exactly the omit-on-default twins the writer would have
          // emitted; a field left at its default stays diff-clean (Constraint 2).
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
      });
  if (!installed) {
    // Arena exhaustion during the baseline install (a resource error, not a
    // format one); the model is left unmutated. Surfaced on the reader's error
    // channel so no allocation failure escapes as an exception (doc 10).
    return fail(ReaderError::Kind::MalformedField, "/composition");
  }
  // The load succeeded: publish the staged unknowns wholesale. A `nullptr` store is
  // today's lossy behavior, so no existing caller changes shape (Decision 3).
  if (unknown != nullptr) {
    *unknown = std::move(staged);
  }
  return std::monostate{};
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
