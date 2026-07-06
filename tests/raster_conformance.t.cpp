// Contract conformance driver for org.arbc.raster (doc 16: content kinds run the
// public arbc-testing suite; refinement Acceptance "Conformance run"). Runs the
// property families over a fresh RasterContent built from a fixed decoded buffer.
// Cross-component (pulls kind_raster + arbc-testing), so it lives in tests/ --
// a src/kind_raster/t/ TU may not include <arbc/testing/...> without tripping the
// doc-17 include-hygiene check (testing is not in kind_raster's dependency
// closure); tests/ is exempt from that scan.
//
// Raster ignores the suite's fabricated snapshot handles (they name no interned
// version), rendering its base buffer, so `snapshot_sensitive` stays false; its
// real snapshot purity is proven by src/kind_raster/t and the concurrency stress.

#include <arbc/contract/content.hpp>
#include <arbc/kind_raster/raster_content.hpp>
#include <arbc/media/surface_format.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace arbc;

// A fixed 8x8 premultiplied linear rgba32f buffer (bounds exceed the suite's 4x4
// target, so a right-of-bounds region is genuinely outside). Deterministic
// gradient, non-transparent.
DecodedImage raster_image() {
  DecodedImage img;
  img.width = 8;
  img.height = 8;
  img.format = k_working_rgba32f;
  std::vector<float> f(static_cast<std::size_t>(8 * 8 * 4));
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const std::size_t o = (static_cast<std::size_t>(y) * 8U + static_cast<std::size_t>(x)) * 4U;
      const float a = 0.5F + 0.5F * static_cast<float>(x) / 7.0F;
      f[o] = a * static_cast<float>(x) / 7.0F;
      f[o + 1] = a * static_cast<float>(y) / 7.0F;
      f[o + 2] = a * 0.25F;
      f[o + 3] = a;
    }
  }
  img.bytes.resize(f.size() * sizeof(float));
  std::memcpy(img.bytes.data(), f.data(), img.bytes.size());
  return img;
}

testing::ContentFactory raster_factory() {
  return
      []() -> std::unique_ptr<Content> { return std::make_unique<RasterContent>(raster_image()); };
}

} // namespace

// enforces: 03-layer-plugin-interface#render-scale-honest
// enforces: 03-layer-plugin-interface#render-within-declared-bounds
// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state
// enforces: 03-layer-plugin-interface#capture-restore-roundtrip
// enforces: 03-layer-plugin-interface#facet-consistency
// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
// enforces: 03-layer-plugin-interface#static-time-invariant
TEST_CASE("org.arbc.raster passes the contract conformance suite") {
  arbc::contract_tests(raster_factory());
}
