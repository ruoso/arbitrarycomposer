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

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace arbc {
namespace {

using json = nlohmann::json;

// The only format major this reader knows (doc 08 Principle 4; the writer emits
// `{"arbc":{"format":1}}`). A document carrying any other major is rejected
// wholesale -- never partially loaded.
constexpr std::int64_t k_format_major = 1;

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
  // The layer's content body (`{kind, kind_version, params, inputs}`), captured
  // verbatim when the layer carries a `kind`; `content` is the live Content the
  // routing reconstructs from it (serialize.kind_params). A layer with no `kind`
  // leaves both defaulted, so it binds `ObjectId{}` exactly as before.
  bool has_content{false};
  json content_body;
  std::unique_ptr<Content> content;
};

struct CompData {
  double canvas_w{0.0};
  double canvas_h{0.0};
  bool has_working_space{false};
  SurfaceFormat working_space{};
  bool has_working_audio_format{false};
  AudioFormat working_audio_format{};
  std::vector<LayerData> layers;
};

// The content body lives BESIDE placement in the layer object (doc 08:29-36): the
// core-owned `kind`/`kind_version`, the kind-owned `params`, and the core-owned
// `inputs` edges. Captured verbatim (all four keys when present) so an unknown kind
// re-serializes byte-equivalent; placement keys stay the writer's separate concern.
bool extract_content_body(const json& o, json& out) {
  if (!o.contains("kind")) {
    return false; // a placement-only layer (the writer's v1 output) has no content body
  }
  out = json::object();
  for (const char* key : {"kind", "kind_version", "params", "inputs"}) {
    const auto it = o.find(key);
    if (it != o.end()) {
      out[key] = *it;
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
}

} // namespace

expected<std::monostate, ReaderError> load_document(std::string_view input,
                                                    const Registry& registry,
                                                    const CodecTable& codecs, LoadContext& ctx,
                                                    const ContentSink& sink, Model& into) {
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

  // Reconstruct each layer's content body BEFORE touching the model (Decision 5),
  // so a malformed body returns a `ReaderError` with the target `Model` still empty.
  // Routing happens only when a `ContentSink` is installed to take ownership of the
  // produced content; the content-free overload passes a null sink and an empty
  // codec table, so a placement-only document is byte-identical to before. Dispatch
  // is by kind id through `codecs` (known -> live Content; unknown -> placeholder),
  // with `registry`/`ctx` threaded into the routing (no longer discarded).
  if (sink) {
    for (LayerData& ld : comp.layers) {
      if (!ld.has_content) {
        continue;
      }
      expected<std::unique_ptr<Content>, ReaderError> produced =
          content_body_from_json(ld.content_body, codecs, registry, ctx);
      if (!produced) {
        return unexpected(produced.error()); // model unmutated: nothing installed yet
      }
      ld.content = std::move(*produced);
    }
  }

  // Install the reconstructed graph as the version-0 baseline with an empty
  // journal (Decision 3). A layer with a reconstructed content hands it to the sink
  // and binds the returned id; a placement-only layer binds the invalid `ObjectId{}`
  // placeholder exactly as the content-free path does.
  const expected<std::monostate, PoolError> installed =
      into.load_baseline([&](Model::Transaction& txn) {
        if (!has_composition) {
          return;
        }
        const ObjectId cid = txn.add_composition(comp.canvas_w, comp.canvas_h);
        if (comp.has_working_space) {
          txn.set_working_space(cid, comp.working_space);
        }
        if (comp.has_working_audio_format) {
          txn.set_working_audio_format(cid, comp.working_audio_format);
        }
        for (LayerData& ld : comp.layers) {
          const ObjectId content_id = ld.content ? sink(std::move(ld.content)) : ObjectId{};
          const ObjectId lid = txn.add_layer(content_id, ld.transform, ld.opacity);
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
  return std::monostate{};
}

// The content-free entry point: reconstruct only the envelope, composition, and
// core-owned placement (its historical scope). An empty codec table and a null
// content sink make the content-aware path above a no-op -- every layer binds
// `ObjectId{}` -- so a placement-only document loads byte-identically to before.
expected<std::monostate, ReaderError>
load_document(std::string_view input, const Registry& registry, LoadContext& ctx, Model& into) {
  const CodecTable no_codecs;
  return load_document(input, registry, no_codecs, ctx, ContentSink{}, into);
}

} // namespace arbc
