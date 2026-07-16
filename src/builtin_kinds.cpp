// src/builtin_kinds.cpp -- the L6 umbrella's built-in-kind Registry bootstrap
// (runtime.registry_bootstrap; doc 03 § Registry, doc 17:33/72). Compiled into
// the `arbc` target beside version.cpp: the bootstrap lives at L6 because its
// includes (kind headers at L4, builtin_kind_versions.hpp's constants at L5,
// the L3 `Registry`) are exactly the umbrella's "runtime + all" closure, and
// nothing below L6 may know about it -- `PluginHost` (L5) stays agnostic to
// whatever the host put in the registry it hands over.
//
// Factory semantics (refinement Decision 4): solid/tone/nested adopt the config
// grammars the CI dual-build pinned (tests/ci_plugins/ci_kinds.hpp --
// `ContentConfig` is kind-defined, and one kind must not grow two grammars);
// raster keeps the "WxH" grammar but constructs a TRANSPARENT raster of that
// extent (the production "new paint layer" semantic), not the CI test gradient;
// fade and crossfade -- whose input edges are raw `ContentRef`s that cannot
// travel a config string (doc 17:170-176) -- register factories that return an
// error value directing construction through document deserialize. Errors are
// values on every path (doc 03:176-183); nothing here throws.

#include <arbc/base/expected.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/builtin_kinds.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/runtime/builtin_kind_versions.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arbc {
namespace {

using Made = expected<std::unique_ptr<Content>, std::string>;

Made made_error(std::string message) { return unexpected<std::string>(std::move(message)); }

// strtod/strtoll over an owned copy (a string_view is not NUL-terminated), full
// consumption required -- the exact parsing behavior the CI dual-build grammars
// pinned (tests/ci_plugins/ci_kinds.hpp), so one kind never grows two grammars.
bool parse_double(std::string_view text, double& out) {
  if (text.empty()) {
    return false;
  }
  const std::string owned(text);
  char* end = nullptr;
  out = std::strtod(owned.c_str(), &end);
  return end == owned.c_str() + owned.size();
}

bool parse_int64(std::string_view text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  const std::string owned(text);
  char* end = nullptr;
  out = std::strtoll(owned.c_str(), &end, 10);
  return end == owned.c_str() + owned.size();
}

std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (true) {
    const std::size_t pos = text.find(separator, begin);
    if (pos == std::string_view::npos) {
      parts.push_back(text.substr(begin));
      return parts;
    }
    parts.push_back(text.substr(begin, pos - begin));
    begin = pos + 1;
  }
}

// "r,g,b,a" -- premultiplied working-space floats, unbounded extent.
Made make_solid(ContentConfig config) {
  const std::vector<std::string_view> fields = split(config, ',');
  if (fields.size() != 4) {
    return made_error("org.arbc.solid: expected \"r,g,b,a\"");
  }
  std::array<double, 4> channel{};
  for (std::size_t i = 0; i < channel.size(); ++i) {
    if (!parse_double(fields[i], channel[i])) {
      return made_error("org.arbc.solid: channel is not a number");
    }
  }
  return std::unique_ptr<Content>(std::make_unique<SolidContent>(
      Rgba{static_cast<float>(channel[0]), static_cast<float>(channel[1]),
           static_cast<float>(channel[2]), static_cast<float>(channel[3])}));
}

// "<frequency_hz>,<amplitude>".
Made make_tone(ContentConfig config) {
  const std::vector<std::string_view> fields = split(config, ',');
  if (fields.size() != 2) {
    return made_error("org.arbc.tone: expected \"<frequency_hz>,<amplitude>\"");
  }
  std::int64_t frequency = 0;
  if (!parse_int64(fields[0], frequency) || frequency <= 0 ||
      frequency > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return made_error("org.arbc.tone: frequency_hz is not a positive integer");
  }
  double amplitude = 0.0;
  if (!parse_double(fields[1], amplitude)) {
    return made_error("org.arbc.tone: amplitude is not a number");
  }
  return std::unique_ptr<Content>(std::make_unique<ToneContent>(
      static_cast<std::uint32_t>(frequency), static_cast<float>(amplitude)));
}

// "<width>x<height>" -- a TRANSPARENT raster of that extent (the production
// "new paint layer" semantic, refinement Decision 4), not the CI test gradient.
// `from_tiles` hands the fill a PRE-ZEROED per-tile buffer, and zeroed
// premultiplied RGBA is transparent black, so accepting every tile untouched
// builds the transparent pyramid with O(tile) transient memory.
Made make_raster(ContentConfig config) {
  const std::vector<std::string_view> fields = split(config, 'x');
  if (fields.size() != 2) {
    return made_error("org.arbc.raster: expected \"<width>x<height>\"");
  }
  std::int64_t width = 0;
  std::int64_t height = 0;
  if (!parse_int64(fields[0], width) || !parse_int64(fields[1], height)) {
    return made_error("org.arbc.raster: extent is not a number");
  }
  constexpr auto k_max_extent = static_cast<std::int64_t>(std::numeric_limits<int>::max());
  if (width < 1 || height < 1 || width > k_max_extent || height > k_max_extent) {
    return made_error("org.arbc.raster: extent must be a positive <width>x<height>");
  }
  std::unique_ptr<RasterContent> raster = RasterContent::from_tiles(
      static_cast<int>(width), static_cast<int>(height), k_default_tile_edge,
      [](std::size_t, std::span<float>) { return true; });
  if (raster == nullptr) {
    return made_error("org.arbc.raster: could not build the tile pyramid");
  }
  return std::unique_ptr<Content>(std::move(raster));
}

// A positive decimal child-composition ObjectId. Config-constructible because
// the child resolves HOST-SIDE at attach (doc 03 § Registry): the content is
// created unattached and describes as the empty placeholder until the host
// injects its services.
Made make_nested(ContentConfig config) {
  std::int64_t id = 0;
  if (!parse_int64(config, id) || id <= 0) {
    return made_error("org.arbc.nested: expected a positive decimal ObjectId");
  }
  return std::unique_ptr<Content>(
      std::make_unique<NestedContent>(ObjectId{static_cast<std::uint64_t>(id)}));
}

// fade/crossfade input edges are raw `ContentRef`s fixed at construction and
// cannot travel a config string (kinds.dual_build Decision 3, doc 17:170-176),
// so the honest production path is document deserialize -- the factory says so
// rather than silently owning inputs the document cannot see or serialize
// (refinement Decision 4).
Made refuse_config_construction(std::string_view kind_id) {
  return made_error(std::string(kind_id) +
                    ": input edges cannot travel ContentConfig; construct this operator "
                    "kind through document deserialize (doc 03 § Registry)");
}

} // namespace

void register_builtin_kinds(Registry& registry) {
  // Skip-on-duplicate: `Registry::add` is first-wins, and for a bootstrap a
  // `DuplicateId` is designed precedence -- a host's explicit registration or a
  // deliberately-first-loaded plugin keeps the id -- not an error (doc 03
  // § Registry; #explicit-host-registration-precedes-scan). Ids are non-empty
  // constants, so `DuplicateId` is the only reachable error and the result is
  // deliberately dropped: a partial overlap must not strand the remaining
  // kinds unregistered (refinement Decision 3).
  const auto add = [&registry](std::string_view id, ContentFactory factory, std::string human_name,
                               std::string version) {
    (void)registry.add(id, std::move(factory),
                       KindMetadata{std::move(human_name), std::move(version)});
  };
  // Registration order below is the enumeration order a host menu sees;
  // metadata versions reuse the persisted k_*_kind_version constants
  // (arbc/runtime/builtin_kind_versions.hpp) so registry metadata always
  // equals the serialized kind_version (refinement Constraint 6).
  add(SolidContent::kind_id, make_solid, "Solid Color", k_solid_kind_version);
  add(ToneContent::kind_id, make_tone, "Tone", k_tone_kind_version);
  add(RasterContent::kind_id, make_raster, "Raster", k_raster_kind_version);
  add(
      FadeContent::kind_id,
      [](ContentConfig) { return refuse_config_construction(FadeContent::kind_id); }, "Fade",
      k_fade_kind_version);
  add(
      CrossfadeContent::kind_id,
      [](ContentConfig) { return refuse_config_construction(CrossfadeContent::kind_id); },
      "Crossfade", k_crossfade_kind_version);
  add(NestedContent::kind_id, make_nested, "Nested Composition", k_nested_kind_version);
}

} // namespace arbc
