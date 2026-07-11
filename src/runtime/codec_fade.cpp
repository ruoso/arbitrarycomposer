// The org.arbc.fade built-in operator codec (runtime.operator_codecs). A runtime TU
// that alone sees both the concrete kind type (L4 kind_fade) and nlohmann::json (via
// L4 serialize, PRIVATE link) -- the sanctioned home for a built-in codec (doc 08
// Principle 1, doc 17:60, Decision 2). Encodes/decodes only the kind's `params`; the
// core routing (`content_body_to_json`/`content_body_from_json`) owns the
// `{kind, kind_version}` frame and the `inputs` limb (re-derived on save from
// `Content::inputs()`, never stored here -- Constraint 1). No nlohmann exception
// escapes (Constraint 1); malformed input is a `ReaderError` value.

#include <arbc/base/time.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>
#include <arbc/runtime/operator_binding.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// The stable string literal for the (v1: only) fade shape (Constraint 3). A new
// shape is an additive string; an unknown token fails closed on read.
const char* shape_token(FadeShape shape) {
  switch (shape) {
  case FadeShape::Linear:
    return "linear";
  }
  return "linear";
}

// A `FadeWindow` as `{"end": <int flicks>, "start": <int flicks>}` -- flick instants
// are integer core scalars (doc 08 Principle 5). Canonical key sort orders them.
json window_json(const FadeWindow& w) {
  json o = json::object();
  o["start"] = static_cast<std::int64_t>(w.start.flicks);
  o["end"] = static_cast<std::int64_t>(w.end.flicks);
  return o;
}

// Serialize a `FadeContent`'s immutable `FadeParams` as
// `{"shape": "linear", "in"?: {...}, "out"?: {...}}` -- each optional window omitted
// when absent (omit-when-default, doc 08 Principle 6). A non-fade content is a codec/
// kind-routing mismatch -> CodecFailed as a value.
expected<json, SerializeError> serialize_fade(const Content& content) {
  const auto* fade = dynamic_cast<const FadeContent*>(&content);
  if (fade == nullptr) {
    return unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
  }
  const FadeParams& p = fade->params();
  json params = json::object();
  params["shape"] = shape_token(p.shape);
  if (p.in.has_value()) {
    params["in"] = window_json(*p.in);
  }
  if (p.out.has_value()) {
    params["out"] = window_json(*p.out);
  }
  return params;
}

// Parse an optional `FadeWindow` at `key`: absent -> nullopt (omit-when-default); a
// present-but-malformed window (not an object, or non-integer start/end) is a
// MalformedField value. The out-param is set only on a present, well-formed window.
expected<std::monostate, ReaderError> parse_window(const json& params, const char* key,
                                                   std::optional<FadeWindow>& out) {
  const auto it = params.find(key);
  if (it == params.end()) {
    return std::monostate{};
  }
  if (!it->is_object()) {
    return read_fail(ReaderError::Kind::MalformedField, std::string("/params/") + key);
  }
  const auto sit = it->find("start");
  const auto eit = it->find("end");
  if (sit == it->end() || !sit->is_number_integer() || eit == it->end() ||
      !eit->is_number_integer()) {
    return read_fail(ReaderError::Kind::MalformedField, std::string("/params/") + key);
  }
  out = FadeWindow{Time{sit->get<std::int64_t>()}, Time{eit->get<std::int64_t>()}};
  return std::monostate{};
}

// Reconstruct a `FadeContent` from a `params` object and the single already-built
// input edge, adopted at construction (Constraint 2). `params` is guaranteed an
// object by the core routing. A missing/unknown `shape`, a malformed window, or wrong
// input arity is a `ReaderError` value -- no exception, no partial construction. The
// `params` are validated BEFORE the input-arity check so that a malformed 0-input
// operator body faults before any child is bound, leaving the model unmutated
// (Constraint 2; the read recursion binds input children ahead of this call).
expected<std::unique_ptr<Content>, ReaderError> deserialize_fade(const json& params,
                                                                 std::span<const ContentRef> inputs,
                                                                 ObjectId /*composition*/,
                                                                 LoadContext& /*ctx*/) {
  const auto shit = params.find("shape");
  if (shit == params.end() || !shit->is_string() || shit->get<std::string>() != "linear") {
    return read_fail(ReaderError::Kind::MalformedField, "/params/shape");
  }
  FadeParams p;
  p.shape = FadeShape::Linear;
  if (auto r = parse_window(params, "in", p.in); !r) {
    return unexpected(r.error());
  }
  if (auto r = parse_window(params, "out", p.out); !r) {
    return unexpected(r.error());
  }
  if (inputs.size() != 1) {
    return read_fail(ReaderError::Kind::MalformedField, "/inputs");
  }
  return std::unique_ptr<Content>(std::make_unique<FadeContent>(inputs[0], p));
}

} // namespace

Codec fade_codec() { return Codec{serialize_fade, deserialize_fade}; }

void register_fade_binder() {
  // The typed match lives here, the one runtime TU that names `FadeContent`
  // (doc 17:60), so the render drivers stay kind-agnostic (Decision 2). `try_attach`
  // dynamic-casts exactly as `serialize_fade` above; `detach` runs only after a
  // matched attach, so a `static_cast` is sound.
  // Fade needs only the two services its `attach` declares; the `BindContext`'s
  // resolver and pin are for the kinds that read document membership (nested).
  register_operator_binder(
      FadeContent::kind_id,
      OperatorBinder{[](Content& c, const BindContext& ctx) -> bool {
                       if (auto* fade = dynamic_cast<FadeContent*>(&c)) {
                         fade->attach(ctx.pull, ctx.backend);
                         return true;
                       }
                       return false;
                     },
                     [](Content& c) noexcept { static_cast<FadeContent&>(c).detach(); }});
}

} // namespace arbc
