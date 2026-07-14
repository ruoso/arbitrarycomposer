// The out-of-tree conformance consumer (quality.testing_artifact).
//
// This translation unit is compiled by a FOREIGN CMake project
// (tests/consumer/CMakeLists.txt) against a STAGED INSTALL PREFIX: it sees
// <prefix>/include/arbc/... and libarbc.a + libarbc-testing.a, and nothing else of
// arbitrarycomposer. It is the plugin author's three-line story from
// tests/tone_conformance.t.cpp told from outside -- define a Content of your own,
// hand a factory to arbc::contract_tests, and the suite proves your kind honors the
// doc-03 contract. Doc 16:31-44 calls that "the reason plugin quality scales without
// review capacity"; until this file compiled, it was a promise no outsider could
// keep.
//
// The content is deliberately NOT one of the reference kinds (testing_artifact D6):
// instantiating org.arbc.tone would prove that kind's headers install, not that the
// conformance surface is usable by someone with a kind we have never seen.

#include <arbc/base/geometry.hpp>
#include <arbc/base/time.hpp>
#include <arbc/contract/content.hpp>
#include <arbc/media/pixel_format.hpp>
#include <arbc/surface/surface.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>

namespace {

using namespace arbc;

// A flat-fill leaf: Static (hence no time_extent), unbounded, insensitive to its
// snapshot, settling inline and faithfully at whatever scale is asked. The smallest
// Content a third party can write that the whole umbrella suite still has something
// to say about.
class FlatFill final : public Content {
public:
  explicit FlatFill(float base) : d_base(base) {}

  std::optional<Rect> bounds() const override { return std::nullopt; }
  Stability stability() const override { return Stability::Static; }
  std::optional<TimeRange> time_extent() const override { return std::nullopt; }

  std::optional<RenderResult> render(const RenderRequest& request,
                                     std::shared_ptr<RenderCompletion>) override {
    // A deterministic, position-varying constant: a function of neither the
    // snapshot nor the time, so render purity and the capture/restore round-trip
    // hold by construction and the suite is checking something real.
    const std::span<float> pixels = request.target.span<PixelFormat::Rgba32fLinearPremul>();
    for (std::size_t i = 0; i < pixels.size(); ++i) {
      pixels[i] = d_base + static_cast<float>(i);
    }
    return RenderResult{request.scale, true, std::nullopt};
  }

private:
  float d_base;
};

testing::ContentFactory flat_fill_factory() {
  return []() -> std::unique_ptr<Content> { return std::make_unique<FlatFill>(0.25F); };
}

} // namespace

// enforces: 17-internal-components#arbc-testing-links-out-of-tree
TEST_CASE("a foreign project runs the conformance suite against an installed arbc") {
  arbc::contract_tests(flat_fill_factory());
}
