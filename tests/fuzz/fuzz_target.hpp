#pragma once

// serialize.format_tests: the ONE shared fuzz-target function over the .arbc JSON
// loader (doc 08 -- untrusted input by definition; doc 16:79 fuzzing charter). Two
// drivers wrap it: a portable corpus-replay Catch2 test (per-push brief, gcc + ASan)
// and a -fsanitize=fuzzer LLVMFuzzerTestOneInput entry (nightly long, clang). Factored
// here so a single body carries both the no-throw/no-crash invariant (Constraint 1)
// and the differential-determinism invariant (Constraint 2) -- the established
// dual-driver pattern (LLVM's StandaloneFuzzTargetMain) that keeps a fuzz target alive
// as a portable regression test (Decision D1).
//
// Fuzzes the RICHEST loader surface (Decision D4): the runtime content-aware
// load_document over the built-in codec table (solid/tone/fade/crossfade), so the
// params/contents codec parsing -- the most format-specific, most-likely-to-fault
// code -- is exercised, not just the content-free envelope.
//
// That richest surface now includes the every-tier unknown-field stash (doc 08
// Principle 4, serialize.unknown_field_preservation): the `UnknownFieldStore` is owned by
// `Document` and threaded through the runtime load/save pair internally, so the
// load->serialize->load->serialize chain below already drives it end to end -- the
// residual subtraction on arbitrary keys, the canonical dump of the stash, its re-parse
// at save, and the never-shadow merge -- with no extra plumbing here. A stash that
// failed to re-parse, or a merge that reordered a key, would break the differential-
// determinism invariant below rather than pass silently. The corpus seeds
// `unknown_fields_all_tiers` / `unknown_shadowing_known` / `unknown_in_known_params` /
// `unknown_nested_in_time_map` seed exactly that surface.

#include <arbc/contract/registry.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/document_serialize.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <variant>

// A determinism-invariant guard shared by both drivers. On violation it prints and
// aborts: under libFuzzer the abort is a reported crash; under the corpus-replay
// ctest binary it is a non-zero exit that fails the lane. Deliberately NOT an
// assert() -- it must fire under NDEBUG (Release/coverage) too.
inline void arbc_fuzz_require(bool condition, const char* message) {
  if (!condition) {
    // GCOV_EXCL_START -- the determinism-violation branch: unreachable while the loader
    // upholds its contract (a hit is a finding, which aborts the run, not a covered path).
    std::fprintf(stderr, "arbc_fuzz: loader invariant violated: %s\n", message);
    std::abort();
    // GCOV_EXCL_STOP
  }
}

// Feed arbitrary bytes to the .arbc loader and enforce its two format promises:
//
//   1. load_document returns an error VALUE or succeeds -- it never throws a
//      JSON/C++ exception, aborts, or reads out of bounds (doc 08:143-149). A throw
//      escaping this call is caught by the corpus-replay driver / crashes libFuzzer.
//   2. On any SUCCESSFUL load the output is deterministic -- including the unknown-field
//      stash, which rides `Document` through both halves of the chain:
//        serialize(load(serialize(load(x)))) == serialize(load(x))
//      -- re-serialize succeeds, re-loading that output succeeds, and re-serializing
//      it is byte-identical (doc 08 Principle 5/6). Every accepted input becomes a
//      determinism probe, not just a crash probe (Decision D2).
//
// Always returns 0 (the libFuzzer contract). A malformed/hostile input that the
// loader rejects as a value is an in-contract outcome, not a finding.
inline int arbc_fuzz_load_document(const uint8_t* data, std::size_t size) {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  arbc::Document doc;
  arbc::KindBridge bridge;
  arbc::Registry registry;
  const arbc::expected<std::monostate, arbc::ReaderError> loaded =
      arbc::load_document(bytes, doc, bridge, registry);
  if (!loaded.has_value()) {
    return 0; // an error value is a valid, in-contract outcome (Constraint 1)
  }

  // A successful load must re-serialize to a canonical fixed point (Constraint 2).
  const arbc::expected<std::string, arbc::SerializeError> s1 = arbc::save_document(doc, bridge);
  arbc_fuzz_require(s1.has_value(), "a loaded document failed to re-serialize");

  arbc::Document doc2;
  arbc::KindBridge bridge2;
  arbc::Registry registry2;
  const arbc::expected<std::monostate, arbc::ReaderError> reloaded =
      arbc::load_document(*s1, doc2, bridge2, registry2);
  arbc_fuzz_require(reloaded.has_value(), "canonical re-serialized output failed to re-load");

  const arbc::expected<std::string, arbc::SerializeError> s2 = arbc::save_document(doc2, bridge2);
  arbc_fuzz_require(s2.has_value(), "the re-loaded document failed to re-serialize");
  arbc_fuzz_require(*s1 == *s2, "re-serialization is not byte-stable (a determinism break)");
  return 0;
}
