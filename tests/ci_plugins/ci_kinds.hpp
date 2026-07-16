#pragma once

// Shared construction surface for the `kinds.dual_build` CI-only plugin modules
// (`tests/ci_plugins/*_ci_plugin.cpp`) and their driver (`tests/dual_build.t.cpp`).
//
// Each of the six in-lib kinds also builds as a tiny CI-only MODULE exposing the
// doc-03 `extern "C" arbc_plugin_register(Registry&)` entry point and registering
// exactly that one kind id (doc 17:129-132). Nothing here ships: no `arbc_*`
// component includes this header, no target that installs links these TUs.
//
// The config each kind accepts is minimal, opaque and kind-defined (doc
// 03:203-207) -- no JSON, no shared grammar, no decoder (Constraint 3). The
// subject of the dual-build is the ENTRY POINT, not the parameter grammar.
//
// The driver includes this header too, so the in-lib reference instance that the
// byte-equality assertion compares against is configured by the SAME code the
// plugin image runs. The only difference between the two instances is which image
// the object lives in -- which is exactly the variable under test.
//
// Errors are values across the boundary (doc 03:176-183): every builder below
// returns `expected<unique_ptr<Content>, string>` and no path throws.

#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/kind_crossfade/crossfade_content.hpp>
#include <arbc/kind_fade/fade_content.hpp>
#include <arbc/kind_nested/nested_content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/kind_solid/solid_content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/media/audio_block.hpp>
#include <arbc/media/surface_format.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace arbc_ci {

using namespace arbc;

using Made = expected<std::unique_ptr<Content>, std::string>;

inline Made made_error(std::string message) { return unexpected<std::string>(std::move(message)); }

// --- Config parsing. `strto*` rather than `std::from_chars` because
//     floating-point `from_chars` is not available on every standard library the
//     CI lanes build against; neither throws, which is what the boundary needs.

inline bool parse_double(std::string_view text, double& out) {
  const std::string owned(text);
  if (owned.empty()) {
    return false;
  }
  char* end = nullptr;
  const double value = std::strtod(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size()) {
    return false; // trailing garbage, or nothing consumed
  }
  out = value;
  return true;
}

inline bool parse_int64(std::string_view text, std::int64_t& out) {
  const std::string owned(text);
  if (owned.empty()) {
    return false;
  }
  char* end = nullptr;
  const long long value = std::strtoll(owned.c_str(), &end, 10);
  if (end != owned.c_str() + owned.size()) {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

inline std::vector<std::string_view> split(std::string_view text, char separator) {
  std::vector<std::string_view> fields;
  std::size_t start = 0;
  while (true) {
    const std::size_t pos = text.find(separator, start);
    if (pos == std::string_view::npos) {
      fields.push_back(text.substr(start));
      return fields;
    }
    fields.push_back(text.substr(start, pos - start));
    start = pos + 1;
  }
}

inline Time seconds(std::int64_t whole) { return Time{whole * Time::flicks_per_second}; }

// --- Plugin-owned input edges (Decision 3).
//
// `ContentRef` is a raw borrowed `Content*` (contract/content.hpp:212) and the v1
// `ContentFactory` config is a `string_view`, so an operator's input edge cannot
// travel the entry point at all: whoever builds the operator must also own its
// inputs. Inside a plugin module the owner is a module-local static destroyed at
// image unload -- i.e. strictly AFTER the operator that borrows it, because
// `PluginHost` dlcloses last (runtime/plugin_host.hpp:154-156). This remains the
// BARE-FACTORY story by design (doc 17:165-167 -- the factory carries no input
// edges): a plugin operator participating in a DOCUMENT adopts core-owned inputs
// through its registered kind codec's `deserialize` span instead
// (`runtime.plugin_operator_registration`; passthrough_ci_plugin.cpp).
class InputOwner {
public:
  InputOwner() = default;
  InputOwner(const InputOwner&) = delete;
  InputOwner& operator=(const InputOwner&) = delete;

  template <class T, class... Args> T* make(Args&&... args) {
    auto owned = std::make_unique<T>(std::forward<Args>(args)...);
    T* borrowed = owned.get();
    d_owned.push_back(std::move(owned));
    return borrowed;
  }

private:
  std::vector<std::unique_ptr<Content>> d_owned;
};

// A leaf with an opaque visual (an embedded org.arbc.solid fill) and a BOUNDED
// `Timed` audio signal -- the exact shape `tests/crossfade_conformance.t.cpp`
// gives crossfade, reproduced here so the plugin-side crossfade can own inputs of
// the same shape. It is required, not decorative: `CrossfadeAudioFacet` is always
// `Timed` and its extent is the pure union of its inputs' (crossfade_content.cpp:249-273),
// so over audio-less (solid) or `Static`-audio (tone) inputs it would declare
// `Timed` with a `nullopt` extent and fail the suite's facet-coherence family.
// Unbounded visual bounds (`nullopt`) so it fills the target and does not
// self-clip, exactly as `SolidContent` does.
class BoundedAudioLeaf final : public Content {
public:
  static constexpr std::int64_t k_audio_end = 3 * Time::flicks_per_second;

  BoundedAudioLeaf(Rgba color, float audio_value) : d_solid(color), d_audio(audio_value) {}

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }
  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion> done) override {
    return d_solid.render(request, done);
  }
  AudioFacet* audio() override { return &d_audio; }

private:
  class Audio final : public AudioFacet {
  public:
    explicit Audio(float value) : d_value(value) {}
    std::optional<TimeRange> audio_extent() const override {
      return TimeRange{Time::zero(), Time{k_audio_end}};
    }
    Stability audio_stability() const override { return Stability::Timed; }
    std::optional<AudioResult> render_audio(const AudioRequest& request,
                                            std::shared_ptr<AudioCompletion>) override {
      const std::uint32_t channels = channel_count(request.layout);
      const std::int64_t flicks_per_frame =
          Time::flicks_per_second / static_cast<std::int64_t>(request.sample_rate);
      for (std::uint32_t f = 0; f < request.target.frames; ++f) {
        const std::int64_t t =
            request.window.start.flicks + static_cast<std::int64_t>(f) * flicks_per_frame;
        const float v = (t >= 0 && t < k_audio_end) ? d_value : 0.0F; // silent past the extent
        for (std::uint32_t c = 0; c < channels; ++c) {
          request.target.samples[static_cast<std::size_t>(f) * channels + c] = v;
        }
      }
      return AudioResult{request.sample_rate, true};
    }

  private:
    float d_value;
  };

  SolidContent d_solid;
  Audio d_audio;
};

// --- The six kinds' canonical configs and builders.
//
// Each `k_*_config` is the string the driver hands the plugin factory AND the
// string it hands the in-lib builder, so the two instances are configured
// identically by construction. `org.arbc.nested`'s config names a host-minted
// `ObjectId`, so it is built at run time rather than fixed here.

inline constexpr std::string_view k_solid_config = "0.5,0.25,0.125,1";
inline constexpr std::string_view k_tone_config = "440,0.5";
inline constexpr std::string_view k_raster_config = "8x8";
inline constexpr std::string_view k_fade_config = "0,10,100,110";
inline constexpr std::string_view k_crossfade_config = "10,10";

// Premultiplied working-space floats: "r,g,b,a". Unbounded (no `bounds` rect) --
// a bounded `SolidContent` does not self-clip and would fail the conformance
// suite's out-of-bounds transparency family.
inline Made make_solid(ContentConfig config) {
  const std::vector<std::string_view> fields = split(config, ',');
  if (fields.size() != 4) {
    return made_error("org.arbc.solid: expected \"r,g,b,a\"");
  }
  double channel[4] = {0.0, 0.0, 0.0, 0.0};
  for (std::size_t i = 0; i < 4; ++i) {
    if (!parse_double(fields[i], channel[i])) {
      return made_error("org.arbc.solid: channel is not a number");
    }
  }
  return std::unique_ptr<Content>(std::make_unique<SolidContent>(
      Rgba{static_cast<float>(channel[0]), static_cast<float>(channel[1]),
           static_cast<float>(channel[2]), static_cast<float>(channel[3])}));
}

// "<frequency_hz>,<amplitude>".
inline Made make_tone(ContentConfig config) {
  const std::vector<std::string_view> fields = split(config, ',');
  if (fields.size() != 2) {
    return made_error("org.arbc.tone: expected \"<frequency_hz>,<amplitude>\"");
  }
  std::int64_t frequency = 0;
  double amplitude = 0.0;
  if (!parse_int64(fields[0], frequency) || frequency <= 0 ||
      frequency > std::int64_t{0xFFFFFFFF}) {
    return made_error("org.arbc.tone: frequency_hz is not a positive integer");
  }
  if (!parse_double(fields[1], amplitude)) {
    return made_error("org.arbc.tone: amplitude is not a number");
  }
  return std::unique_ptr<Content>(std::make_unique<ToneContent>(
      static_cast<std::uint32_t>(frequency), static_cast<float>(amplitude)));
}

// A deterministic gradient synthesized arithmetically -- the CI plugins carry no
// decoder (Constraint 3; doc 17:169's codec line applied to CI glue). Mirrors the
// buffer `tests/raster_conformance.t.cpp` builds in-process.
inline DecodedImage raster_gradient(int width, int height) {
  DecodedImage image;
  image.width = width;
  image.height = height;
  image.format = k_working_rgba32f;

  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
  std::vector<float> pixels(count);
  const float last_x = static_cast<float>(width - 1);
  const float last_y = static_cast<float>(height - 1);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                             static_cast<std::size_t>(x)) *
                            4U;
      const float a = 0.5F + 0.5F * static_cast<float>(x) / last_x;
      pixels[o] = a * static_cast<float>(x) / last_x;
      pixels[o + 1] = a * static_cast<float>(y) / last_y;
      pixels[o + 2] = a * 0.25F;
      pixels[o + 3] = a;
    }
  }
  const auto* src = reinterpret_cast<const std::byte*>(pixels.data());
  image.bytes.assign(src, src + pixels.size() * sizeof(float));
  return image;
}

// "<width>x<height>".
inline Made make_raster(ContentConfig config) {
  const std::vector<std::string_view> fields = split(config, 'x');
  if (fields.size() != 2) {
    return made_error("org.arbc.raster: expected \"<width>x<height>\"");
  }
  std::int64_t width = 0;
  std::int64_t height = 0;
  if (!parse_int64(fields[0], width) || !parse_int64(fields[1], height)) {
    return made_error("org.arbc.raster: extent is not a number");
  }
  // Lower bound 2 so the gradient's `x / (width - 1)` normalization is defined;
  // upper bound so a hostile config cannot ask CI for a gigapixel synthesis.
  if (width < 2 || height < 2 || width > 64 || height > 64) {
    return made_error("org.arbc.raster: extent out of range [2, 64]");
  }
  return std::unique_ptr<Content>(std::make_unique<RasterContent>(
      raster_gradient(static_cast<int>(width), static_cast<int>(height))));
}

// The child composition's `ObjectId` in decimal. Nothing else can cross: the child
// is resolved host-side through the `NestedResolver` the host injects at `attach`.
inline Made make_nested(ContentConfig config) {
  std::int64_t id = 0;
  if (!parse_int64(config, id) || id <= 0) {
    return made_error("org.arbc.nested: expected a positive decimal ObjectId");
  }
  return std::unique_ptr<Content>(
      std::make_unique<NestedContent>(ObjectId{static_cast<std::uint64_t>(id)}));
}

// The fade's one input edge, owned by whoever builds it (Decision 3). A fixed
// unbounded solid, so an in-lib fade and a plugin-image fade wrap byte-identical
// inputs. Mirrors `tests/fade_conformance.t.cpp`'s SolidFixture.
inline constexpr Rgba k_fade_input_color{0.50F, 0.25F, 0.125F, 1.0F};

// "<in_start>,<in_end>,<out_start>,<out_end>", whole seconds. Whole seconds rather
// than flicks so the config is legible and the flick conversion is exact integer
// arithmetic -- identical in both images.
inline Made make_fade(ContentConfig config, InputOwner& owner) {
  const std::vector<std::string_view> fields = split(config, ',');
  if (fields.size() != 4) {
    return made_error("org.arbc.fade: expected \"in_start,in_end,out_start,out_end\" (seconds)");
  }
  std::int64_t bound[4] = {0, 0, 0, 0};
  for (std::size_t i = 0; i < 4; ++i) {
    if (!parse_int64(fields[i], bound[i])) {
      return made_error("org.arbc.fade: window bound is not a number");
    }
  }
  FadeParams params;
  params.shape = FadeShape::Linear;
  params.in = FadeWindow{seconds(bound[0]), seconds(bound[1])};
  params.out = FadeWindow{seconds(bound[2]), seconds(bound[3])};

  ContentRef input = owner.make<SolidContent>(k_fade_input_color);
  return std::unique_ptr<Content>(std::make_unique<FadeContent>(input, params));
}

// Crossfade's two input edges, owned by whoever builds it. Bounded-audio leaves
// (see `BoundedAudioLeaf`), distinctly colored and distinctly voiced.
inline constexpr Rgba k_crossfade_from_color{0.50F, 0.25F, 0.125F, 1.0F};
inline constexpr Rgba k_crossfade_to_color{0.125F, 0.375F, 0.750F, 1.0F};

// "<start>,<duration>", whole seconds.
inline Made make_crossfade(ContentConfig config, InputOwner& owner) {
  const std::vector<std::string_view> fields = split(config, ',');
  if (fields.size() != 2) {
    return made_error("org.arbc.crossfade: expected \"<start>,<duration>\" (seconds)");
  }
  std::int64_t start = 0;
  std::int64_t duration = 0;
  if (!parse_int64(fields[0], start) || !parse_int64(fields[1], duration) || duration <= 0) {
    return made_error(
        "org.arbc.crossfade: window is not a number, or the duration is not positive");
  }
  CrossfadeParams params;
  params.shape = CrossfadeShape::Linear;
  params.start = seconds(start);
  params.duration = seconds(duration);

  ContentRef from = owner.make<BoundedAudioLeaf>(k_crossfade_from_color, 0.25F);
  ContentRef to = owner.make<BoundedAudioLeaf>(k_crossfade_to_color, -0.50F);
  return std::unique_ptr<Content>(std::make_unique<CrossfadeContent>(from, to, params));
}

// The metadata every CI module publishes beside its factory. `version` is "1" (the
// kind's v1 params shape); `human_name` is the kind's display name.
inline KindMetadata metadata(std::string human_name) {
  return KindMetadata{std::move(human_name), "1"};
}

} // namespace arbc_ci
