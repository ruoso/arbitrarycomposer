// kinds.dual_build -- the dual-build dlopen proof (doc 17:129-132, doc 03:227-234).
//
// The six kinds that link into `libarbc` (org.arbc.{solid,tone,raster,nested,fade,
// crossfade}) no longer exercise the runtime plugin path at all (doc 17:167-174):
// only imageseq does, and imageseq is the easiest possible case -- a Timed source
// over an opaque directory config, no service injection, no Editable facet. This
// driver closes that gap. Each of the six also builds as a CI-only MODULE
// (tests/ci_plugins/*_ci_plugin.cpp), and everything below reaches those kinds
// SOLELY across the `extern "C" arbc_plugin_register` boundary, through the
// production `arbc::PluginHost`.
//
// The one deliberate exception is the byte-equality proof: it also constructs the
// same content in-lib, from the same config, through the same shared builder
// (tests/ci_plugins/ci_kinds.hpp), so the only difference between the two instances
// is which image the object lives in -- which is precisely the variable under test.
//
// Lifetime (Constraint 6): every plugin-derived `Content` is scoped strictly inside
// its `PluginHost`, so the host's destroy-order contract (plugin_host.hpp:154-155,
// `d_handles` after `d_registry`) unmaps each image only once every factory and
// every content built from it is gone.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/expected.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/base/ids.hpp>
#include <arbc/base/time.hpp>
#include <arbc/base/transform.hpp>
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
#include <arbc/model/model.hpp>
#include <arbc/model/records.hpp>
#include <arbc/runtime/plugin_host.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include "ci_plugins/ci_kinds.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace arbc;

constexpr int k_target_edge = 4;
constexpr double k_target_extent = 4.0;
constexpr std::int64_t k_fps = Time::flicks_per_second;

// One config string no kind accepts -- a malformed config must be an error VALUE
// on every one of the six factories, never a throw across the boundary (doc
// 03:176-183, Constraint 5).
constexpr const char* k_garbage_config = "!! not a config !!";

// The platform ARBC_PLUGIN_PATH directory-list separator, mirroring the loader's.
#if defined(_WIN32)
void set_plugin_path(const char* value) { ::_putenv_s("ARBC_PLUGIN_PATH", value); }
void unset_plugin_path() { ::_putenv_s("ARBC_PLUGIN_PATH", ""); }
#else
void set_plugin_path(const char* value) { ::setenv("ARBC_PLUGIN_PATH", value, 1); }
void unset_plugin_path() { ::unsetenv("ARBC_PLUGIN_PATH"); }
#endif

// RAII save/restore of ARBC_PLUGIN_PATH so the scan case does not leak its value
// into sibling cases in this binary (mirrors tests/plugin_loading.t.cpp).
class ScopedPluginPath {
public:
  ScopedPluginPath() {
    const char* prior = std::getenv("ARBC_PLUGIN_PATH");
    if (prior != nullptr) {
      d_had = true;
      d_prior = prior;
    }
  }
  ScopedPluginPath(const ScopedPluginPath&) = delete;
  ScopedPluginPath& operator=(const ScopedPluginPath&) = delete;
  ~ScopedPluginPath() {
    if (d_had) {
      set_plugin_path(d_prior.c_str());
    } else {
      unset_plugin_path();
    }
  }

private:
  bool d_had = false;
  std::string d_prior;
};

// The host-side PullService double the operator conformance drivers already inject
// (tests/fade_conformance.t.cpp:31-58). It is implemented HERE, in the test binary's
// image, so a plugin-image operator calling `pull`/`pull_audio` dispatches back into
// the host -- the plugin -> host leg of Acceptance 4.
class InlineAudioPull final : public PullService {
public:
  void pull(ContentRef input, const RenderRequest& request,
            std::shared_ptr<RenderCompletion> done) override {
    if (input == nullptr) {
      done->fail(RenderError::ContentFailed);
      return;
    }
    const std::optional<RenderResult> r = input->render(request, done);
    if (r.has_value()) {
      done->complete(*r);
    }
  }
  void pull_audio(ContentRef input, const AudioRequest& request,
                  std::shared_ptr<AudioCompletion> done) override {
    AudioFacet* af = input != nullptr ? input->audio() : nullptr;
    if (af == nullptr) {
      done->fail(RenderError::ResourceUnavailable);
      return;
    }
    const std::optional<AudioResult> r = af->render_audio(request, done);
    if (r.has_value()) {
      done->complete(*r);
    } else if (!done->settled()) {
      done->fail(RenderError::ContentFailed);
    }
  }
};

// Everything a plugin-image content borrows lives here, in the host image: the pull
// service, the backend, the pinned DocRoot and the child resolver. The same fixture
// also builds the in-lib reference instances the equivalence assertion compares
// against, through the same `arbc_ci` builders the plugin modules call.
struct Host {
  CpuBackend backend;
  InlineAudioPull pull;
  Model model;
  SolidContent child_a{Rgba{0.50F, 0.25F, 0.125F, 1.0F}, Rect{0.0, 0.0, 8.0, 8.0}};
  SolidContent child_b{Rgba{0.20F, 0.40F, 0.10F, 0.75F}, Rect{0.0, 0.0, 8.0, 8.0}};
  std::unordered_map<ObjectId, Content*> binding;
  ObjectId comp{};
  DocStatePtr doc;
  arbc_ci::InputOwner owner; // owns the IN-LIB operators' input edges

  Host() {
    auto tx = model.transact("dual-build child");
    comp = tx.add_composition(8.0, 8.0);
    const ObjectId ca = tx.add_content(1);
    const ObjectId cb = tx.add_content(1);
    const ObjectId la = tx.add_layer(ca, Affine::identity());
    const ObjectId lb = tx.add_layer(cb, Affine::translation(1.0, 1.0));
    tx.attach_layer(comp, la);
    tx.attach_layer(comp, lb);
    tx.commit();
    binding[ca] = &child_a;
    binding[cb] = &child_b;
    doc = model.current();
  }

  NestedResolver resolver() {
    return [this](ObjectId id) -> Content* {
      const auto it = binding.find(id);
      return it != binding.end() ? it->second : nullptr;
    };
  }

  // Inject the host's services into a content that may live in another image. The
  // `dynamic_cast` is itself part of the proof: a kind constructed in the plugin
  // image must be recognizable from the host image, exactly as the in-lib operator
  // binder relies on (src/runtime/operator_binding.cpp, register_fade_binder). The
  // three service-free kinds need nothing.
  void attach_services(std::string_view kind_id, Content& content) {
    if (kind_id == NestedContent::kind_id) {
      auto* nested = dynamic_cast<NestedContent*>(&content);
      REQUIRE(nested != nullptr);
      nested->attach(pull, backend, resolver(), *doc);
    } else if (kind_id == FadeContent::kind_id) {
      auto* fade = dynamic_cast<FadeContent*>(&content);
      REQUIRE(fade != nullptr);
      fade->attach(pull, backend);
    } else if (kind_id == CrossfadeContent::kind_id) {
      auto* crossfade = dynamic_cast<CrossfadeContent*>(&content);
      REQUIRE(crossfade != nullptr);
      crossfade->attach(pull, backend);
    }
  }

  // The in-lib twin of what a plugin factory builds: same builder, same config, but
  // constructed inside the test binary. This function is the ONLY place the driver
  // reaches a kind other than across `extern "C"`.
  std::unique_ptr<Content> make_in_lib(std::string_view kind_id, ContentConfig config) {
    arbc_ci::Made made = arbc_ci::made_error("unknown kind id");
    if (kind_id == SolidContent::kind_id) {
      made = arbc_ci::make_solid(config);
    } else if (kind_id == ToneContent::kind_id) {
      made = arbc_ci::make_tone(config);
    } else if (kind_id == RasterContent::kind_id) {
      made = arbc_ci::make_raster(config);
    } else if (kind_id == NestedContent::kind_id) {
      made = arbc_ci::make_nested(config);
    } else if (kind_id == FadeContent::kind_id) {
      made = arbc_ci::make_fade(config, owner);
    } else if (kind_id == CrossfadeContent::kind_id) {
      made = arbc_ci::make_crossfade(config, owner);
    }
    REQUIRE(made.has_value());
    std::unique_ptr<Content> content = std::move(*made);
    attach_services(kind_id, *content);
    return content;
  }
};

// One CI module and the shape the driver drives it with.
struct Case {
  std::string_view id;
  const char* module;
  const char* human_name;
  std::string config;
  Time time;                // a request instant inside the kind's interesting range
  testing::Options options; // the SAME options this kind's in-lib conformance
                            // driver already uses (Acceptance 6)
};

// The six, in the order the aggregate-host case loads them.
std::vector<Case> ci_cases(const Host& host) {
  // Leaf kinds (solid/tone/raster) run the suite on its defaults, exactly as
  // tests/{contract,tone,raster}_conformance.t.cpp do. Raster leaves
  // snapshot_sensitive false: it ignores the suite's fabricated snapshot handles
  // (they name no interned version), so its pixels do not vary with the handle --
  // the rationale raster_conformance.t.cpp records.
  const testing::Options leaf;

  // Operator kinds (nested/fade/crossfade) mirror their in-lib drivers.
  testing::Options op;
  op.snapshot_sensitive = false;
  op.operator_graph = true;

  std::vector<Case> cases;
  cases.push_back({SolidContent::kind_id, ARBC_CI_PLUGIN_SOLID_FILE, "Solid Fill",
                   std::string(arbc_ci::k_solid_config), Time::zero(), leaf});
  cases.push_back({ToneContent::kind_id, ARBC_CI_PLUGIN_TONE_FILE, "Tone Generator",
                   std::string(arbc_ci::k_tone_config), Time::zero(), leaf});
  cases.push_back({RasterContent::kind_id, ARBC_CI_PLUGIN_RASTER_FILE, "Raster Image",
                   std::string(arbc_ci::k_raster_config), Time::zero(), leaf});
  // The child composition is minted by the HOST's model, so nested's config -- the
  // only thing that can cross the entry point -- is built at run time.
  cases.push_back({NestedContent::kind_id, ARBC_CI_PLUGIN_NESTED_FILE, "Nested Composition",
                   std::to_string(host.comp.value), Time::zero(), op});
  // 5s rides the fade-in ramp (windows 0-10s / 100-110s): envelope 0.5, so render
  // takes the temp+composite path rather than the pass-through.
  cases.push_back({FadeContent::kind_id, ARBC_CI_PLUGIN_FADE_FILE, "Fade",
                   std::string(arbc_ci::k_fade_config), Time{5 * k_fps}, op});
  // 15s sits mid-window (10s + 10s): w == 0.5, the dissolve path.
  cases.push_back({CrossfadeContent::kind_id, ARBC_CI_PLUGIN_CROSSFADE_FILE, "Crossfade",
                   std::string(arbc_ci::k_crossfade_config), Time{15 * k_fps}, op});
  return cases;
}

// Settle one render along whichever of the two legal paths the content answers on
// (inline value, or nullopt + a settled RenderCompletion).
std::optional<RenderResult> settle(std::optional<RenderResult> inline_result,
                                   const std::shared_ptr<RenderCompletion>& done) {
  if (inline_result.has_value()) {
    return inline_result;
  }
  std::optional<expected<RenderResult, RenderError>> settled = done->take();
  if (!settled.has_value() || !settled->has_value()) {
    return std::nullopt;
  }
  return **settled;
}

// The pixels a render actually produced: the caller's target, unless the content
// answered with its own provided surface (doc 09) -- then those are the bytes a
// compositor would consume.
std::span<std::byte> produced_pixels(const RenderResult& result, Surface& target) {
  if (result.provided.has_value()) {
    return result.provided->surface().cpu_bytes();
  }
  return target.cpu_bytes();
}

// Acceptance 3, the core proof: a plugin-image content and an identically-configured
// in-lib content, driven through the SAME backend, the SAME request and the SAME
// injected services, produce byte-identical pixels and equal RenderResult fields.
// Byte-exact, no tolerance (doc 16).
void require_render_equivalent(CpuBackend& backend, Content& plugin_side, Content& in_lib,
                               Time time) {
  auto plugin_target = backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
  auto in_lib_target = backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
  REQUIRE(plugin_target.has_value());
  REQUIRE(in_lib_target.has_value());

  const Rect region = Rect::from_size(k_target_extent, k_target_extent);
  auto plugin_done = std::make_shared<RenderCompletion>();
  auto in_lib_done = std::make_shared<RenderCompletion>();
  const RenderRequest plugin_request{
      region, 1.0, time, StateHandle{}, **plugin_target, Exactness::Exact, Deadline::none()};
  const RenderRequest in_lib_request{
      region, 1.0, time, StateHandle{}, **in_lib_target, Exactness::Exact, Deadline::none()};

  const std::optional<RenderResult> plugin_result =
      settle(plugin_side.render(plugin_request, plugin_done), plugin_done);
  const std::optional<RenderResult> in_lib_result =
      settle(in_lib.render(in_lib_request, in_lib_done), in_lib_done);

  REQUIRE(plugin_result.has_value());
  REQUIRE(in_lib_result.has_value());
  REQUIRE(plugin_result->achieved_scale == in_lib_result->achieved_scale);
  REQUIRE(plugin_result->exact == in_lib_result->exact);
  REQUIRE(plugin_result->achieved_time.has_value() == in_lib_result->achieved_time.has_value());
  if (plugin_result->achieved_time.has_value()) {
    REQUIRE(plugin_result->achieved_time->flicks == in_lib_result->achieved_time->flicks);
  }
  REQUIRE(plugin_result->provided.has_value() == in_lib_result->provided.has_value());

  const std::span<std::byte> plugin_pixels = produced_pixels(*plugin_result, **plugin_target);
  const std::span<std::byte> in_lib_pixels = produced_pixels(*in_lib_result, **in_lib_target);
  REQUIRE(plugin_pixels.size() == in_lib_pixels.size());
  REQUIRE_FALSE(plugin_pixels.empty());
  REQUIRE(std::memcmp(plugin_pixels.data(), in_lib_pixels.data(), plugin_pixels.size()) == 0);
}

bool same_bounds(const std::optional<Rect>& a, const std::optional<Rect>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return !a.has_value() || (a->x0 == b->x0 && a->y0 == b->y0 && a->x1 == b->x1 && a->y1 == b->y1);
}

bool same_extent(const std::optional<TimeRange>& a, const std::optional<TimeRange>& b) {
  if (a.has_value() != b.has_value()) {
    return false;
  }
  return !a.has_value() || (a->start.flicks == b->start.flicks && a->end.flicks == b->end.flicks);
}

// Load one CI module into a fresh host and hand back its factory. The registry grew
// by exactly one entry, and that entry is this kind (Constraint 4): an accidental
// static registrar, or a registration dragged in transitively by another kind's
// objects, fails right here.
const ContentFactory& require_sole_registration(PluginHost& host, const Case& c) {
  REQUIRE(host.load_plugin(c.module).has_value());
  REQUIRE(host.registry().size() == 1);
  const std::vector<std::string_view> ids = host.registry().ids();
  REQUIRE(ids.size() == 1);
  REQUIRE(ids[0] == c.id);

  const KindMetadata* meta = host.registry().metadata(c.id);
  REQUIRE(meta != nullptr);
  REQUIRE(meta->human_name == c.human_name);
  REQUIRE(meta->version == "1");

  const ContentFactory* factory = host.registry().factory(c.id);
  REQUIRE(factory != nullptr);
  return *factory;
}

// Adapt an `arbc::ContentFactory` (config -> expected<unique_ptr<Content>, string>)
// obtained across the dlopen boundary into an `arbc::testing::ContentFactory`
// (() -> unique_ptr<Content>), closing over the config and -- for the three
// service-injected kinds -- over the host doubles and the `attach` call. The suite
// then runs unchanged over a plugin-image content (Acceptance 6).
testing::ContentFactory adapt(Host& host, const ContentFactory& factory, const Case& c) {
  return [&host, &factory, &c]() -> std::unique_ptr<Content> {
    arbc_ci::Made made = factory(c.config);
    REQUIRE(made.has_value());
    std::unique_ptr<Content> content = std::move(*made);
    host.attach_services(c.id, *content);
    return content;
  };
}

} // namespace

// enforces: 03-layer-plugin-interface#plugin-registers-through-extern-c-entry
// enforces: 03-layer-plugin-interface#registry-resolves-kind-id-to-factory-and-metadata
// enforces: 03-layer-plugin-interface#loader-errors-are-values
TEST_CASE("each in-lib kind loads as a CI module and registers exactly its own kind id") {
  Host host;
  for (const Case& c : ci_cases(host)) {
    // A fresh PluginHost per kind, so `size() == 1` is a statement about THIS
    // module and nothing else.
    PluginHost plugin_host;
    const ContentFactory& factory = require_sole_registration(plugin_host, c);

    // Errors are values across the boundary (Constraint 5): a malformed config
    // faults as a value, under REQUIRE_NOTHROW.
    REQUIRE_NOTHROW([&] {
      const arbc_ci::Made bad = factory(k_garbage_config);
      REQUIRE_FALSE(bad.has_value());
      REQUIRE_FALSE(bad.error().empty());
    }());

    // A duplicate registration of the same id is a RegistryError value, not a throw.
    const expected<std::monostate, RegistryError> duplicate =
        plugin_host.registry().add(c.id, factory);
    REQUIRE_FALSE(duplicate.has_value());
    REQUIRE(duplicate.error() == RegistryError::DuplicateId);

    // The content the boundary factory yields describes itself exactly as the in-lib
    // instance does.
    arbc_ci::Made made = factory(c.config);
    REQUIRE(made.has_value());
    const std::unique_ptr<Content> plugin_side = std::move(*made);
    host.attach_services(c.id, *plugin_side);
    const std::unique_ptr<Content> in_lib = host.make_in_lib(c.id, c.config);

    REQUIRE(same_bounds(plugin_side->bounds(), in_lib->bounds()));
    REQUIRE(plugin_side->stability() == in_lib->stability());
    REQUIRE(same_extent(plugin_side->time_extent(), in_lib->time_extent()));
  }
}

TEST_CASE("one PluginHost aggregates the six CI modules plus imageseq without collision") {
  Host host;
  const std::vector<Case> cases = ci_cases(host);

  PluginHost plugin_host;
  for (const Case& c : cases) {
    REQUIRE(plugin_host.load_plugin(c.module).has_value());
  }
  REQUIRE(plugin_host.load_plugin(ARBC_IMAGESEQ_PLUGIN_FILE).has_value());

  const std::vector<std::string_view> ids = plugin_host.registry().ids();
  REQUIRE(ids.size() == 7);
  for (std::size_t i = 0; i < cases.size(); ++i) {
    REQUIRE(ids[i] == cases[i].id); // registration order
  }
  REQUIRE(ids[6] == std::string_view{"org.arbc.imageseq"});

  // Seven DISTINCT ids: no module dragged another's registration in with it.
  std::vector<std::string_view> sorted = ids;
  std::sort(sorted.begin(), sorted.end());
  REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

  // Re-loading a module registers nothing new: the loader infers DuplicateId from
  // the registry not growing (the entry point returns void).
  const expected<std::monostate, PluginLoadError> again = plugin_host.load_plugin(cases[0].module);
  REQUIRE_FALSE(again.has_value());
  REQUIRE(again.error().code == PluginLoadError::Code::DuplicateId);
  REQUIRE(plugin_host.registry().size() == 7);
}

// enforces: 03-layer-plugin-interface#plugin-path-scan-is-opt-in
TEST_CASE("the opt-in ARBC_PLUGIN_PATH scan over the CI plugin directory loads exactly six") {
  const ScopedPluginPath guard;
  Host host;

  SECTION("unset: zero loads, zero filesystem access") {
    unset_plugin_path();
    PluginHost plugin_host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = plugin_host.scan_plugin_path());
    REQUIRE(report.loaded == 0);
    REQUIRE(report.entries.empty());
    REQUIRE(plugin_host.registry().size() == 0);
  }

  SECTION("set: the dedicated ci_plugins/ directory yields exactly the six kinds") {
    // The six modules own an output directory of their own (Constraint 7), so this
    // sweep finds them and nothing else -- not arbc-plugin-imageseq, not
    // arbc-plugin-noentry.
    set_plugin_path(ARBC_CI_PLUGIN_DIR);
    PluginHost plugin_host;
    PluginScanReport report;
    REQUIRE_NOTHROW(report = plugin_host.scan_plugin_path());

    REQUIRE(report.loaded == 6);
    REQUIRE(report.entries.size() == 6);
    for (const PluginScanEntry& entry : report.entries) {
      REQUIRE(entry.outcome == PluginScanEntry::Outcome::Loaded);
      REQUIRE(entry.diagnostic.empty());
    }
    // Deterministic lexicographic order (plugin_host.hpp).
    REQUIRE(std::is_sorted(
        report.entries.begin(), report.entries.end(),
        [](const PluginScanEntry& a, const PluginScanEntry& b) { return a.path < b.path; }));

    REQUIRE(plugin_host.registry().size() == 6);
    for (const Case& c : ci_cases(host)) {
      REQUIRE(plugin_host.registry().factory(c.id) != nullptr);
    }
  }
}

// enforces: 17-internal-components#in-lib-kinds-dual-build-through-plugin-entry
TEST_CASE("a plugin-image kind renders byte-identically to the in-lib kind") {
  Host host;
  for (const Case& c : ci_cases(host)) {
    PluginHost plugin_host;
    const ContentFactory& factory = require_sole_registration(plugin_host, c);

    arbc_ci::Made made = factory(c.config);
    REQUIRE(made.has_value());
    const std::unique_ptr<Content> plugin_side = std::move(*made);
    host.attach_services(c.id, *plugin_side);

    const std::unique_ptr<Content> in_lib = host.make_in_lib(c.id, c.config);

    require_render_equivalent(host.backend, *plugin_side, *in_lib, c.time);
  }
  // `plugin_side` dies at each iteration's end; `plugin_host` at the next -- so no
  // Content outlives the image that defines its vtable.
}

// enforces: 17-internal-components#in-lib-kinds-dual-build-through-plugin-entry
TEST_CASE("host services inject into a plugin-image content and dispatch crosses both ways") {
  Host host;
  for (const Case& c : ci_cases(host)) {
    const bool needs_services = c.id == NestedContent::kind_id || c.id == FadeContent::kind_id ||
                                c.id == CrossfadeContent::kind_id;
    if (!needs_services) {
      continue;
    }

    PluginHost plugin_host;
    const ContentFactory& factory = require_sole_registration(plugin_host, c);

    arbc_ci::Made made = factory(c.config);
    REQUIRE(made.has_value());
    const std::unique_ptr<Content> content = std::move(*made);

    // Constructed UNATTACHED across the boundary: the v1 Registry factory carries no
    // service handles at all. The host injects them afterwards, exactly as the
    // runtime binders do in-lib. (NestedContent exposes no `attached()`.)
    auto* fade = dynamic_cast<FadeContent*>(content.get());
    auto* crossfade = dynamic_cast<CrossfadeContent*>(content.get());
    if (fade != nullptr) {
      REQUIRE_FALSE(fade->attached());
    } else if (crossfade != nullptr) {
      REQUIRE_FALSE(crossfade->attached());
    } else {
      REQUIRE(c.id == NestedContent::kind_id);
    }
    host.attach_services(c.id, *content);

    // Render: host -> plugin through the Content vtable, and plugin -> host as the
    // operator's body calls PullService::pull on an interface implemented in THIS
    // image over inputs that live in the plugin's (fade/crossfade) or the host's
    // (nested, via the injected resolver).
    const std::unique_ptr<Content> in_lib = host.make_in_lib(c.id, c.config);
    require_render_equivalent(host.backend, *content, *in_lib, c.time);

    // detach() clears the borrowed services so no later render touches a released
    // one (fade_content.hpp:56-60). Observable across the boundary via attached();
    // a post-detach render is the kind's debug assert, not an error value, so it is
    // not driven here.
    if (fade != nullptr) {
      REQUIRE(fade->attached());
      fade->detach();
      REQUIRE_FALSE(fade->attached());
    } else if (crossfade != nullptr) {
      REQUIRE(crossfade->attached());
      crossfade->detach();
      REQUIRE_FALSE(crossfade->attached());
    }
  }
}

// enforces: 17-internal-components#in-lib-kinds-dual-build-through-plugin-entry
TEST_CASE("facets survive the plugin boundary") {
  Host host;
  const std::vector<Case> cases = ci_cases(host);

  SECTION("org.arbc.tone exposes its AudioFacet across the boundary") {
    const Case& c = cases[1];
    REQUIRE(c.id == ToneContent::kind_id);
    PluginHost plugin_host;
    const ContentFactory& factory = require_sole_registration(plugin_host, c);

    arbc_ci::Made made = factory(c.config);
    REQUIRE(made.has_value());
    const std::unique_ptr<Content> content = std::move(*made);

    AudioFacet* facet = content->audio();
    REQUIRE(facet != nullptr);
    REQUIRE(content->audio() == facet); // stable pointer identity across calls

    constexpr std::uint32_t k_frames = 64;
    constexpr std::uint32_t k_rate = 48000;
    const std::uint32_t channels = channel_count(ChannelLayout::Stereo);
    std::vector<float> samples(static_cast<std::size_t>(k_frames) * channels, 0.0F);
    AudioBlock block{samples.data(), k_frames, ChannelLayout::Stereo, k_rate};
    const std::int64_t window_flicks =
        static_cast<std::int64_t>(k_frames) * (k_fps / static_cast<std::int64_t>(k_rate));
    const AudioRequest request{TimeRange{Time::zero(), Time{window_flicks}},
                               k_rate,
                               ChannelLayout::Stereo,
                               block,
                               Exactness::Exact,
                               StateHandle{},
                               std::nullopt};

    auto done = std::make_shared<AudioCompletion>();
    const std::optional<AudioResult> result = facet->render_audio(request, done);
    // Exactly one settle path: an inline result, or a settled completion -- never
    // both, never neither.
    REQUIRE(result.has_value() != done->settled());
    if (!result.has_value()) {
      const std::optional<expected<AudioResult, RenderError>> settled = done->take();
      REQUIRE(settled.has_value());
      REQUIRE(settled->has_value());
    }
  }

  SECTION("org.arbc.raster exposes its Editable second base subobject across the boundary") {
    const Case& c = cases[2];
    REQUIRE(c.id == RasterContent::kind_id);
    PluginHost plugin_host;
    const ContentFactory& factory = require_sole_registration(plugin_host, c);

    arbc_ci::Made made = factory(c.config);
    REQUIRE(made.has_value());
    const std::unique_ptr<Content> content = std::move(*made);

    Editable* editable = content->editable();
    REQUIRE(editable != nullptr);
    REQUIRE(content->editable() == editable);
    // RasterContent : public Content, public Editable (raster_content.hpp:225), so
    // the Editable subobject sits at a NON-ZERO offset. The plugin image performed
    // that adjustment inside editable(); the host consumes the adjusted pointer.
    REQUIRE(static_cast<void*>(editable) != static_cast<void*>(content.get()));

    // capture() -> render -> restore() round-trip across the boundary reproduces the
    // pixels byte-for-byte.
    const StateHandle captured = editable->capture();
    auto first = host.backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
    auto second = host.backend.make_surface(k_target_edge, k_target_edge, k_working_rgba32f);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());

    const Rect region = Rect::from_size(k_target_extent, k_target_extent);
    auto done_first = std::make_shared<RenderCompletion>();
    const RenderRequest first_request{
        region, 1.0, Time::zero(), captured, **first, Exactness::Exact, Deadline::none()};
    REQUIRE(settle(content->render(first_request, done_first), done_first).has_value());

    editable->restore(captured);

    auto done_second = std::make_shared<RenderCompletion>();
    const RenderRequest second_request{
        region, 1.0, Time::zero(), captured, **second, Exactness::Exact, Deadline::none()};
    REQUIRE(settle(content->render(second_request, done_second), done_second).has_value());

    const std::span<const std::byte> a = (*first)->cpu_bytes();
    const std::span<const std::byte> b = (*second)->cpu_bytes();
    REQUIRE(a.size() == b.size());
    REQUIRE(std::memcmp(a.data(), b.data(), a.size()) == 0);
  }
}

// enforces: 17-internal-components#in-lib-kinds-dual-build-through-plugin-entry
TEST_CASE("each plugin-obtained factory passes the contract conformance suite") {
  Host host;
  for (const Case& c : ci_cases(host)) {
    PluginHost plugin_host;
    const ContentFactory& factory = require_sole_registration(plugin_host, c);

    // The same suite, the same seed, the same options the kind's in-lib conformance
    // driver already uses -- run over a factory obtained across the dlopen boundary.
    // Every content the suite builds dies inside contract_tests(), so none outlives
    // `plugin_host`.
    arbc::contract_tests(adapt(host, factory, c), c.options);
  }
}
