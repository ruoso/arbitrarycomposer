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
#include <arbc/serialize/load_context.hpp> // AssetSource (the deferring lane)

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

// The DEFERRING asset source the second lane below drives (runtime.async_external_load). It
// records each `request()` and answers nothing until `fire_all()` -- which is the only way a
// fuzz input can reach the PENDING state at all, since a load with no source installed fires
// the continuation immediately with empty bytes and is therefore always UNAVAILABLE. It serves
// the fuzz input itself for every URI, so the same hostile bytes are also parsed as a CHILD
// composition, through `load_composition`'s ordinary-transaction install rather than
// `load_baseline`'s -- a code path the corpus could not otherwise reach. Termination is the
// loader's own: a self-resolving reference dedups, and any chain is capped at
// `k_external_ref_depth_cap`.
class ArbcFuzzDeferringSource final : public arbc::AssetSource {
public:
  explicit ArbcFuzzDeferringSource(std::string_view bytes) : d_bytes(bytes) {}

  void request(std::string_view, std::function<void(std::string_view)> on_ready) override {
    d_outstanding.push_back(std::move(on_ready));
  }

  std::size_t fire_all() {
    std::vector<std::function<void(std::string_view)>> firing;
    firing.swap(d_outstanding);
    for (const std::function<void(std::string_view)>& on_ready : firing) {
      on_ready(d_bytes);
    }
    return firing.size();
  }

private:
  std::string_view d_bytes;
  std::vector<std::function<void(std::string_view)>> d_outstanding;
};

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

  // The DEFERRING lane (runtime.async_external_load): the same bytes, loaded through a source
  // that answers NOTHING inside `request()`. Every external reference in the input is therefore
  // PENDING -- a valid child id naming no composition record, the state a null-source load can
  // never produce -- and the loader must not fault on it, on hostile input or on any other
  // (`08-serialization#loader-never-faults-on-hostile-input`).
  //
  // Two promises ride this lane. First, the SAVE does not depend on load state: a document
  // whose child is pending saves byte-identically to one whose child was never fetched at all
  // -- pending is the first state where `composition_ref()` is valid while the record is
  // ABSENT, so it is the first state that could have leaked a dangling `"composition"` id into
  // the format. Second, the arrival edge itself: firing the callbacks and settling parses those
  // same bytes as a CHILD composition, installs it through an ordinary transaction, and must
  // STILL leave the save byte-identical -- the model gained a child; the document did not.
  arbc::Document doc3;
  arbc::KindBridge bridge3;
  arbc::Registry registry3;
  ArbcFuzzDeferringSource deferring(bytes);
  const arbc::expected<std::monostate, arbc::ReaderError> pending =
      arbc::load_document(bytes, doc3, bridge3, registry3, "fuzz/input.arbc", &deferring);
  arbc_fuzz_require(pending.has_value(), "bytes that loaded with no source failed with one");

  const arbc::expected<std::string, arbc::SerializeError> s3 = arbc::save_document(doc3, bridge3);
  arbc_fuzz_require(s3.has_value(), "a document with a PENDING external child failed to save");
  arbc_fuzz_require(*s1 == *s3, "the save depends on LOAD state (a pending child leaked)");

  // Drive the arrivals to quiescence. Each round can discover at most one new link per pending
  // reference, and the loader's depth cap bounds the chain, so this terminates.
  while (deferring.fire_all() > 0) {
    static_cast<void>(arbc::settle_external_loads(doc3, bridge3, registry3));
  }
  const arbc::expected<std::string, arbc::SerializeError> s4 = arbc::save_document(doc3, bridge3);
  arbc_fuzz_require(s4.has_value(), "a document with a SETTLED external child failed to save");
  arbc_fuzz_require(*s1 == *s4, "the save depends on load state (a settled child was inlined)");
  return 0;
}
