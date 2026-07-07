// The permanent end-to-end test of the runtime plugin path (doc 17:150-159,
// Decision 1). Performs the full path with NO in-repo shortcut: build
// arbc-plugin-imageseq -> dlopen the .so -> resolve and call
// extern "C" arbc_plugin_register -> obtain the org.arbc.imageseq factory from
// the Registry -> construct the content -> run a render. runtime.plugin_loading
// (M8) builds its production scan-and-load loader on this same seam.
//
// Boundary-only: this binary does NOT link the plugin's impl archive; it reaches
// the kind solely across the extern "C" seam. The plugin path and the fixture
// directory arrive as compile definitions.

#include <arbc/backend_cpu/cpu_backend.hpp>
#include <arbc/base/geometry.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/contract/registry.hpp>
#include <arbc/media/surface_format.hpp>

#include <catch2/catch_test_macros.hpp>

#include <dlfcn.h>

#include <memory>

using namespace arbc;

namespace {
constexpr const char* k_kind_id = "org.arbc.imageseq";
}

// enforces: 03-layer-plugin-interface#plugin-registers-through-extern-c-entry
TEST_CASE("org.arbc.imageseq registers and constructs across the extern \"C\" boundary") {
  void* handle = dlopen(ARBC_IMAGESEQ_PLUGIN_FILE, RTLD_NOW | RTLD_LOCAL);
  REQUIRE(handle != nullptr);

  using RegisterFn = void (*)(Registry&);
  // NOLINTNEXTLINE(*-reinterpret-cast): the extern "C" entry point ABI.
  auto register_fn = reinterpret_cast<RegisterFn>(dlsym(handle, "arbc_plugin_register"));
  REQUIRE(register_fn != nullptr);

  {
    Registry registry;
    register_fn(registry);

    // The entry point populated the registry with the kind id and metadata.
    REQUIRE(registry.size() == 1);
    const ContentFactory* factory = registry.factory(k_kind_id);
    REQUIRE(factory != nullptr);
    const KindMetadata* meta = registry.metadata(k_kind_id);
    REQUIRE(meta != nullptr);
    REQUIRE_FALSE(meta->human_name.empty());

    // The factory constructs a working Timed content over the fixtures.
    expected<std::unique_ptr<Content>, std::string> content = (*factory)(ARBC_IMAGESEQ_FIXTURE_DIR);
    REQUIRE(content.has_value());
    REQUIRE((*content)->stability() == Stability::Timed);
    REQUIRE((*content)->time_extent().has_value());

    // A render across the boundary settles a frame with an achieved_time and a
    // provided surface.
    CpuBackend backend;
    auto target = backend.make_surface(2, 2, k_working_rgba32f);
    REQUIRE(target.has_value());
    auto done = std::make_shared<RenderCompletion>();
    const RenderRequest request{
        Rect{0.0, 0.0, 2.0, 2.0}, 1.0, Time{0}, StateHandle{}, **target, Exactness::Exact,
        Deadline::none()};
    const std::optional<RenderResult> result = (*content)->render(request, done);
    REQUIRE(result.has_value());
    REQUIRE(result->achieved_time.has_value());
    REQUIRE(result->provided.has_value());

    // Errors are VALUES across the boundary, never exceptions (doc 03:177-180):
    // a missing source directory yields an error value from the factory...
    REQUIRE_FALSE((*factory)("/no/such/imageseq/directory/zzz").has_value());
    // ...and a duplicate registration is an error value, not a throw.
    const expected<std::monostate, RegistryError> dup = registry.add(k_kind_id, *factory);
    REQUIRE_FALSE(dup.has_value());
    REQUIRE(dup.error() == RegistryError::DuplicateId);
  } // registry (holding the .so's factory) and the content are destroyed here...

  dlclose(handle); // ...before the code backing them is unmapped.
}
