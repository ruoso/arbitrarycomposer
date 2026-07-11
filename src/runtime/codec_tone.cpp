// The org.arbc.tone built-in codec (runtime.document_serialize). A runtime TU that
// alone sees both the concrete kind type (L4 kind_tone) and nlohmann::json (via L4
// serialize, PRIVATE link) -- the sanctioned home for a built-in codec (Decision 3).
// Encodes/decodes only the kind's `params` `{frequency_hz, amplitude}`; no nlohmann
// exception escapes (Constraint 1).

#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/runtime/builtin_codecs.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <memory>

namespace arbc {
namespace {

using json = nlohmann::json;

unexpected<ReaderError> read_fail(ReaderError::Kind kind, std::string path) {
  return unexpected(ReaderError{kind, std::move(path), ObjectId{}});
}

// Serialize a `ToneContent`'s `{frequency_hz, amplitude}`. `frequency_hz` is an
// integer number of cycles/second; `amplitude` scales the unit waveform.
expected<json, SerializeError> serialize_tone(const Content& content) {
  const auto* tone = dynamic_cast<const ToneContent*>(&content);
  if (tone == nullptr) {
    return unexpected(SerializeError{SerializeError::Kind::CodecFailed, ObjectId{}});
  }
  json params = json::object();
  params["frequency_hz"] = static_cast<std::int64_t>(tone->frequency_hz());
  params["amplitude"] = tone->amplitude();
  return params;
}

// Reconstruct a `ToneContent` from `{frequency_hz, amplitude}`. `params` is
// guaranteed an object by the core routing. A missing/mistyped or out-of-`uint32`
// `frequency_hz`, or a missing/mistyped `amplitude`, is a MalformedField value -- no
// exception, no partial construction. The leaf kind ignores `inputs`/`ctx`.
expected<std::unique_ptr<Content>, ReaderError>
deserialize_tone(const json& params, std::span<const ContentRef> /*inputs*/,
                 ObjectId /*composition*/, LoadContext& /*ctx*/) {
  const auto fit = params.find("frequency_hz");
  if (fit == params.end() || !fit->is_number_integer()) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/frequency_hz");
  }
  const std::int64_t freq = fit->get<std::int64_t>();
  if (freq < 0 || freq > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/frequency_hz");
  }
  const auto ait = params.find("amplitude");
  if (ait == params.end() || !ait->is_number()) {
    return read_fail(ReaderError::Kind::MalformedField, "/params/amplitude");
  }
  return std::unique_ptr<Content>(std::make_unique<ToneContent>(
      static_cast<std::uint32_t>(freq), static_cast<float>(ait->get<double>())));
}

} // namespace

Codec tone_codec() { return Codec{serialize_tone, deserialize_tone}; }

} // namespace arbc
