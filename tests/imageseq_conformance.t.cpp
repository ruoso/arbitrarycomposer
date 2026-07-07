// org.arbc.imageseq contract conformance (doc 16, contract.conformance_suite,
// Decision 2: each reference kind wires its own arbc::contract_tests run). Runs
// the public arbc-testing suite over a fresh imageseq content built from the
// checked-in fixture sequence. imageseq answers via RenderResult.provided rather
// than filling the target, so the suite -- which compares the target -- exercises
// imageseq's metadata + settlement contract (stability, time_extent,
// achieved_time honesty, scale honesty, bounds/extent, the single-settle
// RenderCompletion path); the decoded pixels are validated through the
// compositor's provided-surface consume path in the temporal / provided-surface
// drivers.
//
// arbc-testing precedes `arbc` on the link line so its unresolved contract
// symbols resolve from the umbrella (static-archive link order); the impl
// archive precedes both so ImageSeqContent resolves.

#include <arbc/contract/content.hpp>
#include <arbc/testing/contract_tests.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/imageseq_fixtures.hpp"

#include <memory>

using namespace arbc;

namespace {

testing::ContentFactory imageseq_factory() {
  return []() -> std::unique_ptr<Content> { return imageseq::testfix::make_content(); };
}

} // namespace

// enforces: 03-layer-plugin-interface#render-pure-over-pinned-state
// enforces: 03-layer-plugin-interface#render-scale-honest
// enforces: 03-layer-plugin-interface#render-time-honest
// enforces: 03-layer-plugin-interface#render-within-declared-bounds
// enforces: 03-layer-plugin-interface#render-inline-or-async
// enforces: 03-layer-plugin-interface#render-completion-settles-once
// enforces: 03-layer-plugin-interface#capture-restore-roundtrip
// enforces: 03-layer-plugin-interface#facet-consistency
// enforces: 03-layer-plugin-interface#leaf-content-has-no-operator-graph
TEST_CASE("org.arbc.imageseq passes the contract conformance suite") {
  // Timed, snapshot-insensitive leaf: the default Options run every family
  // except the static-time-invariant branch (stability() == Timed selects the
  // achieved-time-reporting branch of check_time_honesty instead).
  arbc::contract_tests(imageseq_factory());
}
