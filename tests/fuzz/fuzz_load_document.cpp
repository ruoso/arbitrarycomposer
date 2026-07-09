// serialize.format_tests: the coverage-guided libFuzzer entry over the .arbc loader
// (the nightly "long" run, Decision D1). Built ONLY under the ARBC_FUZZER CMake option
// (the `fuzz` preset), which adds -fsanitize=fuzzer to this target on Clang -- GCC has
// no libFuzzer, so the per-push gcc lanes never see this file (tests/CMakeLists.txt
// guards it). It forwards verbatim to the shared fuzz target; all invariant logic
// lives there so the portable corpus-replay driver enforces exactly the same thing.

#include "fuzz_target.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  return arbc_fuzz_load_document(data, size);
}
