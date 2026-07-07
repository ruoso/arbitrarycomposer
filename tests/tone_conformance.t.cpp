// Contract conformance driver for org.arbc.tone (doc 16: content kinds run the
// public arbc-testing suite; contract.conformance_suite Decision 2 -- each
// reference kind wires its own arbc::contract_tests run). Runs the property
// families over a fresh ToneContent. Because factory()->audio() != nullptr, the
// umbrella auto-runs both audio families (check_audio_facet_consistency and
// check_audio_async) alongside the visual families over tone's culled stub.
//
// Cross-component (pulls kind_tone + arbc-testing), so it lives in tests/ -- a
// src/kind_tone/t/ TU may not include <arbc/testing/...> without tripping the
// doc-17 include-hygiene check (testing is not in kind_tone's dependency
// closure); tests/ is exempt from that scan. arbc-testing precedes `arbc` on
// the link line so its unresolved contract symbols resolve from the umbrella.

#include <arbc/contract/content.hpp>
#include <arbc/kind_tone/tone_content.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

using namespace arbc;

testing::ContentFactory tone_factory() {
  return []() -> std::unique_ptr<Content> { return std::make_unique<ToneContent>(440, 0.5F); };
}

} // namespace

// enforces: 03-layer-plugin-interface#audio-facet-optional
// enforces: 03-layer-plugin-interface#audio-facet-consistent
// enforces: 03-layer-plugin-interface#static-time-invariant
// enforces: 03-layer-plugin-interface#facet-consistency
TEST_CASE("org.arbc.tone passes the contract conformance suite") {
  arbc::contract_tests(tone_factory());
}
