#include <arbc/base/rational_time.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/media/audio_block.hpp> // ChannelLayout
#include <arbc/media/audio_format.hpp>
#include <arbc/media/color_space.hpp>
#include <arbc/media/pixel_format.hpp> // to_string(PixelFormat)
#include <arbc/media/surface_format.hpp>
#include <arbc/model/records.hpp>
#include <arbc/serialize/writer.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

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

private:
  void fault(ObjectId owner) {
    if (d_ok) {
      d_ok = false;
      d_err = SerializeError{SerializeError::Kind::NonFiniteValue, owner};
    }
  }

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

json layer_json(const LayerRecord& lr, ObjectId id, Emitter& em) {
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
  return o;
}

json composition_json(const DocRoot& doc, ObjectId comp_id, const CompositionRecord& comp,
                      Emitter& em) {
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
      layers.push_back(layer_json(*lr, lid, em));
    }
  });
  o["layers"] = std::move(layers);
  return o;
}

} // namespace

expected<std::string, SerializeError> serialize_document(const DocRoot& doc) {
  Emitter em;
  json root = json::object();
  json envelope = json::object();
  envelope["format"] = std::int64_t{1}; // format major only (Decision 5)
  root["arbc"] = std::move(envelope);

  ObjectId comp_id;
  const CompositionRecord* comp = nullptr;
  if (doc.find_first_composition(comp_id, comp)) {
    root["composition"] = composition_json(doc, comp_id, *comp, em);
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

} // namespace arbc
