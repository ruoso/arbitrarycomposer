// serialize.format_tests: the portable corpus-replay regression -- the per-push
// "brief" run of the loader fuzzer (doc 16:79) that does NOT depend on clang
// (Decision D1, Constraint 4). It drives the SAME shared arbc_fuzz_load_document over
// every checked-in seed under tests/fuzz/corpus/load_document/ (path injected via the
// ARBC_FUZZ_CORPUS_DIR compile definition), asserting the loader never throws and
// re-serializes deterministically. Rides the existing dev/asan ctest lanes, so it is
// the enforced safety net for the no-exceptions-across-the-boundary promise
// (doc 08:143-149) whether or not a coverage-guided libFuzzer build is available.
//
// Cross-component: reaches the loader only through the public arbc/runtime + serialize
// headers (via the shared fuzz target), so it links the umbrella `arbc` and lives
// under top-level tests/ outside the levelized graph (Constraint 5).

#include <catch2/catch_test_macros.hpp>

#include "fuzz/fuzz_target.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef ARBC_FUZZ_CORPUS_DIR
#error "ARBC_FUZZ_CORPUS_DIR must be defined (the checked-in seed corpus directory)"
#endif

namespace {

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                   std::istreambuf_iterator<char>());
}

} // namespace

// enforces: 08-serialization#loader-never-faults-on-hostile-input
TEST_CASE("the loader survives every checked-in fuzz seed: no throw, deterministic re-save") {
  const std::filesystem::path corpus_dir{ARBC_FUZZ_CORPUS_DIR};
  REQUIRE(std::filesystem::is_directory(corpus_dir));

  int seeds = 0;
  for (const auto& entry : std::filesystem::directory_iterator(corpus_dir)) {
    if (!entry.is_regular_file()) {
      continue; // GCOV_EXCL_LINE: the checked-in corpus is all regular files
    }
    ++seeds;
    const std::vector<std::uint8_t> bytes = read_bytes(entry.path());
    INFO("corpus seed: " << entry.path().filename().string());
    // Constraint 1: no exception crosses the loader boundary on any seed (well-formed
    // or hostile). Constraint 2: the differential-determinism invariant is enforced
    // INSIDE the shared target (it aborts the process on a byte-stability break, which
    // ctest reports as a lane failure -- exactly libFuzzer's crash semantics).
    CHECK_NOTHROW(arbc_fuzz_load_document(bytes.data(), bytes.size()));
  }

  // The seed corpus is committed and non-empty (Constraint 6, doc 17:187-188).
  CHECK(seeds > 0);
}
