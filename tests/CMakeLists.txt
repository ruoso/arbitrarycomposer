# Cross-component tests: integration, claims-register golden tests, and the
# harness smoke test. Component unit tests live in src/<component>/t/.

add_executable(arbc_smoke_t smoke.t.cpp)
target_link_libraries(arbc_smoke_t PRIVATE Catch2::Catch2WithMain
                                           arbc_build_flags)
catch_discover_tests(arbc_smoke_t)

# Contract conformance driver (doc 16, contract.conformance_suite): runs the
# public arbc-testing suite over one purpose-built double per contract branch
# plus the org.arbc.solid reference kind. Cross-component (pulls kind_solid), so
# it lives here. arbc-testing precedes `arbc` on the link line so its unresolved
# contract symbols resolve from the umbrella (static-archive link order).
# serialize.json_dep build-integration smoke probe: links the chosen JSON
# library (nlohmann/json) STANDALONE -- no arbc component, no src/serialize/ yet
# (serialize.writer stands that up) -- and proves the two capabilities the whole
# serialize format rests on: verbatim round-trip of an arbitrary/unknown-kind
# tree and a non-throwing parse path an L4 API surfaces as arbc::expected, plus
# the default object's sorted-key ordering. It is a build-integration probe
# carrying diff coverage; it takes no `enforces:` claim (doc-08 determinism /
# unknown-kind claims are owned by serialize.format_tests / serialize.kind_params).
add_executable(arbc_serialize_json_dep_smoke_t serialize_json_dep_smoke.t.cpp)
target_link_libraries(
  arbc_serialize_json_dep_smoke_t
  PRIVATE nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_json_dep_smoke_t)

# serialize.zstd_dep build-integration smoke test for the tile-blob compress/decompress
# seam. NOTE THE LINK LINE, which is the whole point: it links the umbrella `arbc` and
# NOT arbc_zstd, NOT zstd::*, deliberately deviating from the json_dep probe directly
# above (zstd_dep Decision 4). zstd is a COMPILED library, so unlike header-only
# nlohmann it cannot wave away the transitive-burden question -- and a probe that linked
# zstd standalone would exercise zstd while proving nothing about libarbc's interface.
# Linking `arbc` alone proves both halves by construction: it COMPILES only if no zstd
# type leaked into a public header, and LINKS only if the PRIVATE link onto
# arbc_serialize is genuinely right. It also names kind_raster's public tile geometry to
# size the realistic case. Carries the two doc-08 tile-blob claims.
add_executable(arbc_serialize_zstd_dep_smoke_t serialize_zstd_dep_smoke.t.cpp)
target_link_libraries(arbc_serialize_zstd_dep_smoke_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_zstd_dep_smoke_t)

# serialize.writer byte-exact canonical golden + pinned-version fidelity: builds a
# small document through the model transaction API, pins it, and asserts the
# canonical `.arbc` bytes byte-for-byte, then that a held pin ignores later edits.
# Cross-component (drives the model), so it links the umbrella `arbc`.
add_executable(arbc_serialize_writer_golden_t serialize_writer_golden.t.cpp)
target_link_libraries(arbc_serialize_writer_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_writer_golden_t)

# serialize.reader byte-exact load->save round-trip + version-0 baseline + unknown-
# format-major rejection: loads the writer's golden string and re-serializes it
# byte-for-byte (the reader is the exact inverse of the writer for core placement),
# proves a load installs a version-0 baseline whose undo is a no-op, and rejects an
# unknown format major with no document mutation. Cross-component (model + writer +
# journal), so it links the umbrella `arbc`.
add_executable(arbc_serialize_reader_roundtrip_t serialize_reader_roundtrip.t.cpp)
target_link_libraries(arbc_serialize_reader_roundtrip_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_reader_roundtrip_t)

# serialize.kind_params: the content-body codec seam, the unknown-kind
# PlaceholderContent verbatim round-trip, the placeholder input-0 pass-through
# render, and the L4 read/write routing (ContentSink + content-body provider). Names
# a test kind double AND nlohmann::json through the serialize-internal codec /
# placeholder headers, so it links the umbrella `arbc` plus nlohmann explicitly
# (nlohmann is PRIVATE to arbc_serialize and does not propagate to `arbc`).
add_executable(arbc_serialize_kind_params_t serialize_kind_params.t.cpp)
target_link_libraries(
  arbc_serialize_kind_params_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_kind_params_t)

# serialize.sharing: the L4 structural read/write of the operator graph -- the
# core-owned `inputs` array beside `params`, the document-level `contents` table with
# `{"$ref": id}` for shared content, intra-document dedup, and the dangling/cyclic-ref
# read-error discipline (doc 08 Principle 6). Like kind_params it names test kinds AND
# nlohmann::json through the serialize-internal codec/placeholder headers, so it links
# the umbrella `arbc` plus nlohmann explicitly (nlohmann is PRIVATE to arbc_serialize).
add_executable(arbc_serialize_sharing_t serialize_sharing.t.cpp)
target_link_libraries(
  arbc_serialize_sharing_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_sharing_t)

# serialize.compositions_table: the L4 read/write of the document's GRAPH of compositions
# -- the document-level `compositions` table beside `contents`, the core-owned
# `"composition"` field on a content body, the root's reserved ordinal `"0"`, legal Droste
# cycles (and their separation from illegal operator-input cycles), and the two new read
# errors (doc 08 Principle 7, doc 05). Names a nesting test kind AND nlohmann::json through
# the serialize-internal codec/placeholder headers, so it links the umbrella `arbc` plus
# nlohmann explicitly (nlohmann is PRIVATE to arbc_serialize).
add_executable(arbc_serialize_compositions_t serialize_compositions.t.cpp)
target_link_libraries(
  arbc_serialize_compositions_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_compositions_t)

# serialize.unknown_field_preservation: the every-tier unknown-field rules the corpus can
# only prove by byte-equality -- the never-shadow collision rule (working_space.primaries),
# the residual/merge core (recursion, atomicity, a malformed stash degrading to empty), and
# the known-kind `params` residual frozen at load so a cleared optional never resurrects.
# Names the serialize-internal unknown_json.hpp (which names nlohmann::json), so it links
# the umbrella `arbc` plus nlohmann explicitly (nlohmann is PRIVATE to arbc_serialize).
add_executable(arbc_serialize_unknown_fields_t serialize_unknown_fields.t.cpp)
target_link_libraries(
  arbc_serialize_unknown_fields_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_unknown_fields_t)

# serialize.format_tests determinism / load->save round-trip CORPUS test: over an
# inline corpus of representative canonical documents, asserts serialize(load(x)) == x
# byte-exact + re-serialization idempotence, unknown-kind/field verbatim preservation,
# and the hand-authored-id normalization case serialize.sharing deferred here. Drives
# the runtime content-aware load/save (builtin_codecs) + the content-free serialize
# API through public headers only, so it links the umbrella `arbc` (no nlohmann).
add_executable(arbc_serialize_determinism_corpus_t serialize_determinism_corpus.t.cpp)
target_link_libraries(arbc_serialize_determinism_corpus_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_determinism_corpus_t)

# serialize.format_tests loader fuzz corpus-replay: the portable per-push "brief" run
# of the libFuzzer harness (doc 16:79, Decision D1). Drives the shared
# arbc_fuzz_load_document (tests/fuzz/fuzz_target.hpp) over every checked-in seed under
# tests/fuzz/corpus/load_document/ (path via ARBC_FUZZ_CORPUS_DIR), asserting the loader
# never throws and re-serializes deterministically. Rides the existing dev/asan ctest
# lanes -- the enforced no-fault safety net that needs neither clang nor ARBC_FUZZER.
set(ARBC_FUZZ_CORPUS_DIR "${CMAKE_SOURCE_DIR}/tests/fuzz/corpus/load_document")
add_executable(arbc_fuzz_corpus_replay_t fuzz_corpus_replay.t.cpp)
target_link_libraries(arbc_fuzz_corpus_replay_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_fuzz_corpus_replay_t
                           PRIVATE ARBC_FUZZ_CORPUS_DIR="${ARBC_FUZZ_CORPUS_DIR}")
catch_discover_tests(arbc_fuzz_corpus_replay_t)

# serialize.format_tests coverage-guided libFuzzer entry (the nightly "long" run,
# Decision D1): the -fsanitize=fuzzer LLVMFuzzerTestOneInput forwarding to the SAME
# shared fuzz target. GCC has no libFuzzer, so this is gated behind the ARBC_FUZZER
# option (the `fuzz` preset) AND a Clang check -- the per-push gcc/msvc lanes never
# build it. libFuzzer supplies its own main(), so no Catch2 / catch_discover_tests; the
# nightly job runs the binary directly, time-bounded, over the checked-in corpus.
if(ARBC_FUZZER)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "ARBC_FUZZER requires Clang (-fsanitize=fuzzer is clang-only)")
  endif()
  add_executable(arbc_fuzz_load_document fuzz/fuzz_load_document.cpp)
  target_link_libraries(arbc_fuzz_load_document PRIVATE arbc arbc_build_flags)
  target_compile_options(arbc_fuzz_load_document PRIVATE -fsanitize=fuzzer)
  target_link_options(arbc_fuzz_load_document PRIVATE -fsanitize=fuzzer)
endif()

# serialize.writer concurrency stress (tier-6, tsan lane): a serialize worker reads
# a held pin while the single writer thread commits N transactions -- the pinned
# read races the mutator with no data race (serialize.writer Constraint 1).
add_executable(arbc_serialize_writer_concurrency_t serialize_writer_concurrency.t.cpp)
target_link_libraries(arbc_serialize_writer_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_serialize_writer_concurrency_t)

# runtime.document_serialize end-to-end byte-exact golden: builds a Document with an
# org.arbc.solid + org.arbc.tone content on placed layers, asserts the canonical
# `.arbc` bytes byte-for-byte through the runtime save facade, that a pinned snapshot
# ignores later edits, and that a reload->resave is byte-identical. Holds a CodecTable
# (builtin_codecs()) so it names nlohmann::json through the serialize-internal codec
# header -- links the umbrella `arbc` plus nlohmann explicitly (PRIVATE to arbc_runtime).
add_executable(arbc_document_serialize_golden_t document_serialize_golden.t.cpp)
target_link_libraries(
  arbc_document_serialize_golden_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_document_serialize_golden_t)

# serialize.compositions_table through the L5 sinks: a live Document holding TWO
# compositions -- built with a test-double nesting kind interned through the KindBridge and
# its codec injected into serialize_snapshot's CodecTable (tests/ sits outside the
# levelization graph, format_tests D4) -- captures BOTH compositions' layer-root contents
# into the ContentSnapshot and emits them, and the same document round-trips through
# runtime::load_document -> save_document. Holds a CodecTable, so it links nlohmann.
add_executable(arbc_document_compositions_golden_t document_compositions_golden.t.cpp)
target_link_libraries(
  arbc_document_compositions_golden_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_document_compositions_golden_t)

# runtime.document_serialize concurrency stress (tier-6, tsan lane): a background
# thread re-emits a pinned ContentSnapshot while the single writer thread churns
# add_content/attach mutations -- the captured save emits off-thread with no data race
# (Decision 6). Like the golden it holds a CodecTable, so it links nlohmann explicitly.
add_executable(arbc_document_serialize_concurrency_t document_serialize_concurrency.t.cpp)
target_link_libraries(
  arbc_document_serialize_concurrency_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_document_serialize_concurrency_t)

# runtime.operator_codecs end-to-end byte-exact golden: builds a Document with an
# operator graph (org.arbc.fade over a shared input + org.arbc.crossfade over that
# shared input plus a second), asserts the canonical `.arbc` bytes byte-for-byte
# through the runtime save facade -- exercising the `inputs` array, the `contents`
# table + `$ref` dedup, and the input-child reverse map -- that a pinned snapshot
# ignores later edits, and that a reload->resave is byte-identical. Holds a CodecTable
# (builtin_codecs()) so it names nlohmann::json through the serialize-internal codec
# header -- links the umbrella `arbc` plus nlohmann explicitly (PRIVATE to arbc_runtime).
add_executable(arbc_operator_codecs_golden_t operator_codecs_golden.t.cpp)
target_link_libraries(
  arbc_operator_codecs_golden_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_operator_codecs_golden_t)

# runtime.operator_codecs concurrency stress (tier-6, tsan lane): a background thread
# re-emits a pinned operator-graph ContentSnapshot -- incl. the input-child reverse-map
# capture -- while the single writer thread churns add_content/attach mutations. The
# captured save emits off-thread with no data race (Constraint 10). Holds a CodecTable,
# so it links nlohmann explicitly.
add_executable(arbc_operator_codecs_concurrency_t operator_codecs_concurrency.t.cpp)
target_link_libraries(
  arbc_operator_codecs_concurrency_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_operator_codecs_concurrency_t)

# runtime.nested_codec end-to-end byte-exact goldens: a REAL org.arbc.nested content (not the
# test double serialize.compositions_table proved the format with) round-trips through a live
# Document -- plain nesting, a Droste self-cycle, a shared child -- and the attach-invariance
# proof that pins Decision 1: a save taken while a live OperatorBindingScope makes nested's
# derived inputs() non-empty is byte-identical to the unbound save. Needs a real Backend and a
# real binding scope, so it is cross-component; holds no CodecTable of its own but names
# save_document/load_document, and links nlohmann for symmetry with its siblings.
add_executable(arbc_nested_codec_golden_t nested_codec_golden.t.cpp)
target_link_libraries(
  arbc_nested_codec_golden_t
  PRIVATE arbc nlohmann_json::nlohmann_json Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_codec_golden_t)

# runtime.nested_external_ref end-to-end: M8's promise ("external nested projects load") over a
# REAL FilesystemAssetSource and real files in a temp project directory. The pixel golden is an
# ORACLE -- the external scene must render byte-identically to the same scene authored
# in-document (doc 05:47-52) -- and the save-side half pins the data-loss hazard shut: a loaded
# external child must NOT be hoisted into the compositions/contents tables. Needs a real Backend
# and a real OperatorBindingScope, so it is cross-component.
add_executable(arbc_nested_external_ref_golden_t nested_external_ref_golden.t.cpp)
target_link_libraries(arbc_nested_external_ref_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_external_ref_golden_t)

# runtime.nested_external_ref loader behaviours, over an IN-MEMORY AssetSource double: the dedup
# counter (two refs to one URI cost ONE request), the A->B->A cross-document cycle and the
# self-reference (allocate-before-parse cuts the knot), the unavailable-is-not-an-error
# placeholder path, and the hostile-chain depth cap. Deterministic -- no temp files, no timing
# assertions, counters only (doc 16:54-62). Drives a live binding for the cycle render, so it is
# cross-component like its golden sibling.
add_executable(arbc_nested_external_ref_t nested_external_ref.t.cpp)
target_link_libraries(arbc_nested_external_ref_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_external_ref_t)

# runtime.async_external_load loader behaviours, over a DEFERRING AssetSource double whose
# on_ready fires only when the test says so: the pending child (a VALID id naming no record, at
# revision 0), the writer-thread settle that installs it on a LATER revision with damage on the
# embedding content, the deferring cycle and grandchild chain, the depth cap counting
# PRE-deferral depth, and a callback outliving its Document. Arrival is a thing the test
# SCHEDULES, so nothing is timing-dependent -- counters only (doc 16:54-62). Drives a live
# binding for the render-does-not-fault proofs, so it is cross-component.
add_executable(arbc_async_external_load_t async_external_load.t.cpp)
target_link_libraries(arbc_async_external_load_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_async_external_load_t)

# runtime.async_external_load end-to-end: the placeholder REPLACED BY THE REAL PIXELS, live. The
# golden is an ORACLE -- a deferred external child must render byte-identically to the same
# scene loaded through an INLINE source -- plus the metadata memo's re-key across the arrival
# and the save-while-pending fixed point (the one place a valid-id-with-absent-record could have
# leaked into the format). Needs a real Backend and a real OperatorBindingScope.
add_executable(arbc_async_external_load_golden_t async_external_load_golden.t.cpp)
target_link_libraries(arbc_async_external_load_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_async_external_load_golden_t)

add_executable(arbc_contract_conformance_t contract_conformance.t.cpp)
target_link_libraries(
  arbc_contract_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_contract_conformance_t)

# org.arbc.raster contract conformance (pulls kind_raster + arbc-testing). Like
# the solid driver above, arbc-testing precedes `arbc` on the link line so its
# unresolved contract symbols resolve from the umbrella.
add_executable(arbc_raster_conformance_t raster_conformance.t.cpp)
target_link_libraries(
  arbc_raster_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_raster_conformance_t)

# org.arbc.tone contract conformance (pulls kind_tone + arbc-testing). Since
# factory()->audio() != nullptr the umbrella also runs the audio families;
# arbc-testing precedes `arbc` on the link line so its unresolved contract
# symbols resolve from the umbrella.
add_executable(arbc_tone_conformance_t tone_conformance.t.cpp)
target_link_libraries(
  arbc_tone_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_tone_conformance_t)

# org.arbc.nested contract conformance (pulls kind_nested + kind_solid +
# arbc-testing). arbc-testing precedes `arbc` on the link line (static-archive
# link order), as with the solid / raster drivers above.
add_executable(arbc_nested_conformance_t nested_conformance.t.cpp)
target_link_libraries(
  arbc_nested_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_conformance_t)

# org.arbc.fade contract conformance (pulls kind_fade + kind_solid + kind_tone +
# arbc-testing). Fade is the reference single-input operator: contract_tests with
# operator_graph on, plus the granular operator damage-covers / identity-faithful
# families, over a fade fronted by an inline PullService + CpuBackend. Since
# factory()->audio() != nullptr the umbrella also runs the audio families.
# arbc-testing precedes `arbc` on the link line (static-archive link order).
add_executable(arbc_fade_conformance_t fade_conformance.t.cpp)
target_link_libraries(
  arbc_fade_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_fade_conformance_t)

# org.arbc.fade byte-exact goldens (visual premultiplied-RGBA attenuation at a
# partial envelope through the real CpuBackend composite; audio per-frame gain
# ramp over a tone) plus the both-facets fully-closed / fully-open behavioral
# cases. Cross-component (drives CpuBackend + kind_solid + kind_tone), so like
# nested_goldens it lives here and links the umbrella `arbc`.
add_executable(arbc_fade_goldens_t fade_goldens.t.cpp)
target_link_libraries(arbc_fade_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_fade_goldens_t)

# compositor.pull_service "tiled == whole" byte-exact golden: a region spanning a
# 2x2 block of k_tile_size tiles, pulled through the live PullServiceImpl, equals
# the same position-dependent leaf (kind_raster gradient) rendered whole into a
# region-sized surface -- pinning the multi-tile deliver's seam-freedom
# (Constraint 6). Cross-component (CpuBackend + kind_raster + compositor), so like
# nested_goldens it lives here and links the umbrella `arbc`.
add_executable(arbc_pull_multitile_golden_t pull_multitile_golden.t.cpp)
target_link_libraries(arbc_pull_multitile_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_pull_multitile_golden_t)

# org.arbc.fade identity behavioral counter: a real fade driven through the
# compositor's PullServiceImpl records operator_renders delta 0 when its envelope
# is exactly 1 at the request time (identity short-circuit) and delta 1 otherwise.
# Cross-component (compositor + kind_fade), so it links the umbrella `arbc`.
add_executable(arbc_fade_identity_counter_t fade_identity_counter.t.cpp)
target_link_libraries(arbc_fade_identity_counter_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_fade_identity_counter_t)

# operators.fade_runtime_binding end-to-end: builds a Document with a fade layer and
# drives it through the real offline (SequenceRenderer, inline + parallel) and export
# (ExportMonitor) drivers with the PRODUCTION attach injection, asserting it renders
# to completion (would abort without the wiring), byte-exact to the manually-attached
# golden, with the identity/pull counters and RAII teardown. Cross-component (drives
# CpuBackend + kind_fade + runtime drivers), so like fade_goldens it lives here and
# links the umbrella `arbc`.
add_executable(arbc_fade_runtime_binding_t fade_runtime_binding.t.cpp)
target_link_libraries(arbc_fade_runtime_binding_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_fade_runtime_binding_t)

# Canonical operator conformance driver (pulls kind_fade + kind_solid +
# arbc-testing). The operator-agnostic collection point: over each reference
# operator factory it runs identity honesty + damage-mapping honesty (the shipped
# arbc-testing families) plus the net-new pull-routing property (its L4 helper
# operator_conformance.hpp, which needs the live PullServiceImpl + counters).
# arbc-testing precedes `arbc` on the link line (static-archive link order).
add_executable(arbc_operator_conformance_t operator_conformance.t.cpp)
target_link_libraries(
  arbc_operator_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_operator_conformance_t)

# org.arbc.crossfade contract conformance (pulls kind_crossfade + kind_solid +
# arbc-testing). Crossfade is the reference TWO-input operator: contract_tests
# with operator_graph on, plus the granular operator damage-covers /
# identity-faithful families, over a crossfade fronted by an inline PullService +
# CpuBackend. Since factory()->audio() != nullptr the umbrella also runs the
# audio families (bounded-audio input doubles keep the Timed union extent
# bounded). arbc-testing precedes `arbc` on the link line (static-archive order).
add_executable(arbc_crossfade_conformance_t crossfade_conformance.t.cpp)
target_link_libraries(
  arbc_crossfade_conformance_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_crossfade_conformance_t)

# org.arbc.crossfade byte-exact goldens (visual source-over dissolve at w == 0.5
# through the real CpuBackend composite; audio per-frame complementary-weight mix
# over two tones) plus the endpoint pass-through cases (w == 0 == input 0, w == 1
# == input 1). Cross-component (drives CpuBackend + kind_solid + kind_tone), so
# like fade_goldens it lives here and links the umbrella `arbc`.
add_executable(arbc_crossfade_goldens_t crossfade_goldens.t.cpp)
target_link_libraries(arbc_crossfade_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_crossfade_goldens_t)

# org.arbc.crossfade identity behavioral counter: a real crossfade driven through
# the compositor's PullServiceImpl records operator_renders delta 0 at each
# endpoint (w == 0 / w == 1 identity short-circuit) and delta 1 mid-transition.
# Cross-component (compositor + kind_crossfade), so it links the umbrella `arbc`.
add_executable(arbc_crossfade_identity_counter_t crossfade_identity_counter.t.cpp)
target_link_libraries(arbc_crossfade_identity_counter_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_crossfade_identity_counter_t)

# runtime.interactive_pull_wiring: the interactive driver's operator contract, pinned
# byte-exact against the export driver's frame of the same document (doc 02:40-41's
# "two drivers over the same core"). An identity endpoint delivers its terminal
# input's pixels instead of compositing blank, issues zero operator renders, degrades
# nowhere, and keys each operator input under its OWN cache identity. Cross-component
# (runtime + compositor + kind_crossfade/kind_fade/kind_solid + backend_cpu), so it
# links the umbrella `arbc`.
add_executable(arbc_interactive_operator_identity_golden_t
               interactive_operator_identity_golden.t.cpp)
target_link_libraries(arbc_interactive_operator_identity_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_interactive_operator_identity_golden_t)

# runtime.interactive_binder_wiring: the interactive driver BINDS its operators, per
# frame, against the frame-local pull service (doc 13 § "Binding is the render driver's
# obligation"). Pinned byte-exact against the export driver: a fade at an interior
# envelope and a crossfade at an interior weight -- neither an identity endpoint, so each
# runs its OWN render and pulls through the injected service -- and a nested scene, which
# has no input edges at all until attach. Also: the per-frame re-bind does not thrash
# nested's metadata memo, and a nesting's child layers carry distinct pull identities.
# Cross-component (runtime + compositor + kind_fade/kind_crossfade/kind_nested/kind_solid
# + backend_cpu), so it links the umbrella `arbc`.
add_executable(arbc_interactive_operator_binding_golden_t
               interactive_operator_binding_golden.t.cpp)
target_link_libraries(arbc_interactive_operator_binding_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_interactive_operator_binding_golden_t)

# runtime.interactive_pull_wiring, Constraint 6: an operator INPUT that answers
# asynchronously emits damage under its synthesized (non-layer-root) id, which only
# `route_operator_damage` can turn into a follow-up frame that re-drives the endpoint.
add_executable(arbc_interactive_operator_identity_t interactive_operator_identity.t.cpp)
target_link_libraries(arbc_interactive_operator_identity_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_interactive_operator_identity_t)

# runtime.operator_model_damage_routing: an EDIT to a content an operator consumes by
# `$ref` -- and which is not itself a layer -- matches no layer root, so unrouted it maps
# to an empty dirty region and the repaint frame never happens (doc 13:124-128's
# correctness bug). Routing it up the operator graph, and dropping the input's tiles under
# its PULL identity, is what makes a shared component editable. Cross-component (runtime +
# compositor + cache + kind_crossfade/kind_solid + backend_cpu), so it links `arbc`.
add_executable(arbc_interactive_model_damage_routing_t interactive_model_damage_routing.t.cpp)
target_link_libraries(arbc_interactive_model_damage_routing_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_interactive_model_damage_routing_t)

# runtime.pull_identity_disjoint_ids: synthesized pull identities are drawn from a reserved
# half of the ObjectId space the model allocator never issues, so damage naming a model id
# cannot evict a synthesized id's tiles (the cross-namespace eviction the old
# `1 + max(layer-root id)` seed allowed), and no synthesized id ever reaches the file
# format. Cross-component (runtime + compositor + cache + serialize + kind_crossfade/
# kind_solid + backend_cpu), so it links `arbc`.
add_executable(arbc_pull_identity_disjoint_ids_t pull_identity_disjoint_ids.t.cpp)
target_link_libraries(arbc_pull_identity_disjoint_ids_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_pull_identity_disjoint_ids_t)

# operators.crossfade_runtime_binding end-to-end: builds a Document with a crossfade
# over two inputs and drives it through the real offline (SequenceRenderer, inline +
# parallel) and export (ExportMonitor) drivers with the PRODUCTION attach injection,
# asserting it renders to completion (would abort without the wiring), byte-exact to
# the manually-attached golden, with the identity/pull counters and RAII teardown --
# the two-input analog of arbc_fade_runtime_binding_t. Cross-component (drives
# CpuBackend + kind_crossfade + runtime drivers), so like crossfade_goldens it lives
# here and links the umbrella `arbc`.
add_executable(arbc_crossfade_runtime_binding_t crossfade_runtime_binding.t.cpp)
target_link_libraries(arbc_crossfade_runtime_binding_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_crossfade_runtime_binding_t)

# runtime.operator_input_cache_identity: the visual byte-exact + parallel/TSan +
# distinct-input audio acceptance crossfade_runtime_binding deferred. Drives the real
# offline (SequenceRenderer, inline + parallel) and export (ExportMonitor) drivers over
# two DISTINCT inputs and pins the interior dissolve / distinct-tone mix byte-exact,
# now that the driver's id_of gives operator input children distinct cache ids.
# Cross-component (CpuBackend + kind_crossfade + runtime drivers), so like
# crossfade_goldens it lives here and links the umbrella `arbc`.
add_executable(arbc_crossfade_offline_dissolve_t crossfade_offline_dissolve.t.cpp)
target_link_libraries(arbc_crossfade_offline_dissolve_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_crossfade_offline_dissolve_t)

# org.arbc.nested byte-exact goldens: nested's output vs the compositor's own
# per-layer loop over the same layers (the rendering-is-recursion identity), plus
# snapshot consistency, budget flow-through, and Droste-cycle termination.
add_executable(arbc_nested_goldens_t nested_goldens.t.cpp)
target_link_libraries(arbc_nested_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_goldens_t)

# org.arbc.nested audio-facet byte-exact goldens + behavioral counters: nested's
# mixed samples vs directly mixing the same tones (the audio rendering-is-recursion
# identity), gain/remix/varispeed/two-level, Droste termination, and the one-pull-
# per-audible-child + snapshot/exactness-verbatim counters (pulls kind_tone).
add_executable(arbc_nested_audio_goldens_t nested_audio_goldens.t.cpp)
target_link_libraries(arbc_nested_audio_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_audio_goldens_t)

# kinds.nested_runtime_binding end-to-end: builds Documents holding an org.arbc.nested
# content and drives them through the real offline (SequenceRenderer, inline + parallel)
# and export (ExportMonitor) drivers with the PRODUCTION attach injection -- the live
# PullService/Backend, the id->Content resolver, and the driver's pinned DocRoot -- with
# no manual attach call. Asserts byte-exactness against the hand-attached oracle,
# transitive binding through the nested boundary, the metadata-memo counter the
# per-frame re-bind must not disturb, and RAII teardown. Cross-component (drives
# CpuBackend + kind_nested + kind_fade + runtime drivers), so like nested_goldens it
# lives here and links the umbrella `arbc`.
add_executable(arbc_nested_runtime_binding_t nested_runtime_binding.t.cpp)
target_link_libraries(arbc_nested_runtime_binding_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_runtime_binding_t)

# org.arbc.nested BELOW-RATE cross-rate reconstruction goldens
# (kinds.nested_audio_resampling): a below-rate child is band-limit-reconstructed
# by the shared arbc::media windowed-sinc kernel byte-identically to applying that
# kernel to the child's genuine native block (integer / non-integer / varispeed /
# two-level ratios), decisively not a nearest/hold, with the discovery+native pull
# counter and preserved achieved_rate/exact honesty; a honoring tone stays 1:1.
add_executable(arbc_nested_audio_resampling_goldens_t nested_audio_resampling_goldens.t.cpp)
target_link_libraries(arbc_nested_audio_resampling_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_audio_resampling_goldens_t)

# org.arbc.nested audio concurrency stress (tier-6, tsan lane): many concurrent
# render_audio calls of one nested audio scene through a stateless multi-thread
# PullService audio double are race-free and deterministic.
add_executable(arbc_nested_audio_concurrency_t nested_audio_concurrency.t.cpp)
target_link_libraries(arbc_nested_audio_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_audio_concurrency_t)

# arbc::audio-engine composition-mixer byte-exact goldens + behavioral counters
# through the REAL PullServiceImpl + org.arbc.tone / org.arbc.nested: additive
# multi-tone mix vs a hand-summed reference, nested-of-tones == flat (the audio
# rendering-is-recursion identity through real cache-first machinery), below-rate
# reconstruction honesty, and the zero/N pull_audio dispatch counters.
add_executable(arbc_audio_mix_goldens_t audio_mix_goldens.t.cpp)
target_link_libraries(arbc_audio_mix_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_audio_mix_goldens_t)

# PullServiceImpl::pull_audio concurrency stress (tier-6, tsan lane): many
# same/distinct-BlockKey pulls -- hits served inline on the monitor thread, misses
# dispatched onto a real worker pool -- settle race-free, each AudioCompletion
# exactly once, with no block-cache data race (the audio twin of the pull case).
add_executable(arbc_pull_audio_concurrency_t pull_audio_concurrency.t.cpp)
target_link_libraries(arbc_pull_audio_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_pull_audio_concurrency_t)

# Lookahead scheduler concurrency stress (tier-6, tsan lane): the audio worker pool
# + lookahead ring under concurrent worker fill + drain -- settle-once holds, the
# ring and BlockCache see no data race, and the drained samples equal the inline
# goldens (the audio twin of the render WorkerPool TSan case).
add_executable(arbc_audio_lookahead_concurrency_t audio_lookahead_concurrency.t.cpp)
target_link_libraries(arbc_audio_lookahead_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_audio_lookahead_concurrency_t)

# Device monitor + clock-mastering concurrency stress (tier-6, tsan lane,
# audio.device_monitor): a real fake device thread masters the transport through the
# monitor's owner thread while the pump and a simulated viewport read the lock-free
# playhead snapshot -- no data race on the published snapshot / delivered counter, the
# transport is mutated on one thread only, each fill settles once, and a reprimed drain
# equals the inline goldens (the transport's reserved audio-clock-master TSan coverage).
add_executable(arbc_device_monitor_concurrency_t device_monitor_concurrency.t.cpp)
target_link_libraries(arbc_device_monitor_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_device_monitor_concurrency_t)

# arbc::ExportMonitor byte-exact integration golden through the REAL PullServiceImpl
# + org.arbc.tone: an offline export of a multi-tone composition over a range equals
# summing the tone renders directly, and is identical between two block sizes (the
# offline sample-exact drive of the mix core, doc 12:187-191).
add_executable(arbc_audio_export_goldens_t audio_export_goldens.t.cpp)
target_link_libraries(arbc_audio_export_goldens_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_audio_export_goldens_t)

# Recursive lookahead fill goldens + counters (audio.lookahead_recursive_prefetch):
# a nested composition and a below-rate native contributor driven through the real
# LookaheadPump + AudioWorkerPool at worker_count>0 drain byte-identical to the inline
# recursion oracle -- the fill warmed the whole transitive contributor closure.
add_executable(arbc_audio_lookahead_recursive_t audio_lookahead_recursive.t.cpp)
target_link_libraries(arbc_audio_lookahead_recursive_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_audio_lookahead_recursive_t)

# Spatial nested-composition lookahead golden (audio.spatial_nested_warm_context): a
# Spatial monitor over a scene whose contributor is a nested composition holding an
# off-center child tone, driven through the real LookaheadPump + AudioWorkerPool at
# worker_count>0, drains byte-identical to the direct Spatial mix_composition oracle --
# the warming descent reconstructed the per-edge Spatialization the mixer pulls with, so
# the nested contributor warms Spatial (not Flat) with silence_mixed==0 and zero dispatch.
add_executable(arbc_audio_lookahead_spatial_nested_t audio_lookahead_spatial_nested.t.cpp)
target_link_libraries(arbc_audio_lookahead_spatial_nested_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_audio_lookahead_spatial_nested_t)

# Spatial block-cache-key disambiguation (audio.spatial_blockkey_disambiguation): a Spatial
# monitor embedding ONE nested composition TWICE at different positions but the same time map
# lands two distinct listener contexts on the same (content, revision, block index, rate);
# the trailing spatial-context digest keys them to distinct cache slots, so the shared-cache
# drain is byte-identical to a fresh per-embedding mix_composition oracle for worker_count
# 0 and 4 (pre-digest one embedding's block was served for both). Pins the two-monitor
# variant and the spatial_context_digest / BlockKey field-fold unit checks.
add_executable(arbc_audio_blockkey_spatial_disambiguation_t
               audio_blockkey_spatial_disambiguation.t.cpp)
target_link_libraries(arbc_audio_blockkey_spatial_disambiguation_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_audio_blockkey_spatial_disambiguation_t)

# org.arbc.nested two-level caching behavioral counters: a static subtree is
# served from the parent cache; only the spine re-renders (requests_issued).
add_executable(arbc_nested_cache_t nested_cache.t.cpp)
target_link_libraries(arbc_nested_cache_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_cache_t)

# org.arbc.nested concurrency stress (tier-6, tsan lane): many concurrent renders
# of one nested content on a real multi-worker WorkerPool are race-free and
# deterministic (render_thread_safe over a thread-confined target).
add_executable(arbc_nested_concurrency_t nested_concurrency.t.cpp)
target_link_libraries(arbc_nested_concurrency_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_nested_concurrency_t)

# Raster editable-state runtime binding (kind_raster + runtime): a raster added to
# a Document journals/budgets/undoes with no manual sink wiring. Cross-component,
# so it lives here rather than src/runtime/t/ -- doc 17 keeps `runtime` free of a
# concrete-kind edge, and tests/ is exempt from the include-hygiene scan.
add_executable(arbc_raster_runtime_binding_t raster_runtime_binding.t.cpp)
target_link_libraries(arbc_raster_runtime_binding_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_raster_runtime_binding_t)

# Raster render-vs-edit concurrency stress (tier-6, tsan lane): pinned-snapshot
# readers race an editor painting new versions.
add_executable(arbc_raster_concurrency_stress_t raster_concurrency_stress.t.cpp)
target_link_libraries(arbc_raster_concurrency_stress_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_raster_concurrency_stress_t)

add_executable(arbc_walking_skeleton_t walking_skeleton.t.cpp)
target_link_libraries(arbc_walking_skeleton_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_walking_skeleton_t)

add_executable(arbc_surface_pool_integration_t surface_pool_integration.t.cpp)
target_link_libraries(arbc_surface_pool_integration_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_surface_pool_integration_t)

add_executable(arbc_provided_surfaces_t provided_surfaces.t.cpp)
target_link_libraries(arbc_provided_surfaces_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_provided_surfaces_t)

add_executable(arbc_offline_working_space_t offline_working_space.t.cpp)
target_link_libraries(arbc_offline_working_space_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_offline_working_space_t)

add_executable(arbc_scale_ladder_golden_t scale_ladder_golden.t.cpp)
target_link_libraries(arbc_scale_ladder_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_scale_ladder_golden_t)

add_executable(arbc_tile_planning_golden_t tile_planning_golden.t.cpp)
target_link_libraries(arbc_tile_planning_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_tile_planning_golden_t)

add_executable(arbc_refinement_golden_t refinement_golden.t.cpp)
target_link_libraries(arbc_refinement_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_refinement_golden_t)

add_executable(arbc_anchored_viewports_golden_t anchored_viewports_golden.t.cpp)
target_link_libraries(arbc_anchored_viewports_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_anchored_viewports_golden_t)

add_executable(arbc_host_viewport_reanchor_golden_t host_viewport_reanchor_golden.t.cpp)
target_link_libraries(arbc_host_viewport_reanchor_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_host_viewport_reanchor_golden_t)

add_executable(arbc_damage_planning_golden_t damage_planning_golden.t.cpp)
target_link_libraries(arbc_damage_planning_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_damage_planning_golden_t)

add_executable(arbc_temporal_coalescing_golden_t temporal_coalescing_golden.t.cpp)
target_link_libraries(arbc_temporal_coalescing_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_temporal_coalescing_golden_t)

# Cross-component concurrency stress + litmus (design doc 16 tier 6). These
# combine model publish/pin (L2), pool reclamation/arena (L1), and runtime
# housekeeping (L5), so per doc 17:153 they live here in top-level tests/ and
# link the umbrella `arbc`. The brief TEST_CASEs run per-push (also under the
# `tsan` preset short-form); the `[.nightly]` sweeps are hidden and run only when
# a nightly job invokes the binary with the `[.nightly]` tag.
add_executable(arbc_stress_publish_pin_t stress_publish_pin.t.cpp)
target_link_libraries(arbc_stress_publish_pin_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_stress_publish_pin_t)

add_executable(arbc_stress_reclamation_queue_t stress_reclamation_queue.t.cpp)
target_link_libraries(arbc_stress_reclamation_queue_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_stress_reclamation_queue_t)

# runtime.housekeeping_document_wiring: the two cross-component legs of wiring the
# Housekeeper onto a live Document's arenas.
#
# The checkpoint cadence over a real workspace file (runtime Document + pool
# workspace file): the every-Nth-transaction writer trigger, the explicit host
# request, the still-document data-msync skip, and the open() durable-root round trip.
add_executable(arbc_document_workspace_checkpoint_t document_workspace_checkpoint.t.cpp)
target_link_libraries(arbc_document_workspace_checkpoint_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_document_workspace_checkpoint_t)

# The concurrency stress (tier-6, tsan lane): a live workspace-backed Document whose
# background housekeeping thread drains while a writer churns editable RASTER contents
# through the router's table and readers pin. It names a concrete kind (kind_raster --
# FakeEditable is not the production shape for this), so like the binding tests above
# it must live here rather than src/runtime/t/.
add_executable(arbc_stress_document_housekeeping_t stress_document_housekeeping.t.cpp)
target_link_libraries(arbc_stress_document_housekeeping_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_stress_document_housekeeping_t)

add_executable(arbc_arena_growth_litmus_t arena_growth_litmus.t.cpp)
target_link_libraries(arbc_arena_growth_litmus_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_arena_growth_litmus_t)

# compositor.pull_service async concurrency stress (tier-6, tsan lane): many
# concurrent worker-backed pulls settle race-free into the single-writer cache.
add_executable(arbc_pull_service_async_t pull_service_async.t.cpp)
target_link_libraries(arbc_pull_service_async_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_pull_service_async_t)

# runtime.worker_dispatch_leaf_only (tier-6, tsan lane): worker dispatch is leaf-only --
# an operator (fade, crossfade, nested) renders inline on the driver thread and only a
# leaf reaches a worker, because an operator's render re-enters the render-thread-confined
# PullService. Drives BOTH drivers at worker_count > 0 over a scene holding all three
# operator kinds, so it names the concrete kinds beside the runtime drivers, the worker
# pool and a real CpuBackend -- cross-component, so it links the umbrella `arbc`. Its
# `[.nightly]` seeded schedule sweep is registered in nightly.yml's tsan-full lane.
add_executable(arbc_worker_dispatch_leaf_only_t worker_dispatch_leaf_only.t.cpp)
target_link_libraries(arbc_worker_dispatch_leaf_only_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_worker_dispatch_leaf_only_t)

# compositor.refine_frame_composite_idempotence: a damage-gated frame CLEARS and CLIPS its
# device repaint region, so its pixels are what one full pass would have put there and
# compositing the same gated frame twice is a no-op. The golden is deterministic and
# byte-exact on a TRANSLUCENT nested scene -- the inverse of damage_planning_golden's
# deliberately opaque one, where a double source-over is invisible. The stress binary is
# the original repro: the interactive loop driven to quiescence under a real worker pool,
# byte-exact against the single-pass oracle on every iteration. Both name the concrete
# kinds beside the compositor driver and the runtime loop -- cross-component, so they link
# the umbrella `arbc`. The stress binary's `[.nightly]` seeded schedule sweep is
# registered in nightly.yml's tsan-full lane; its short form runs per-push under gcc-tsan.
add_executable(arbc_refine_idempotence_golden_t refine_idempotence_golden.t.cpp)
target_link_libraries(arbc_refine_idempotence_golden_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_refine_idempotence_golden_t)

# kinds.operator_async_placeholder_inexact: the transient-placeholder rule of the operator
# contract (doc 13) -- a placeholder rendered because a pull was DISPATCHED is inexact, a
# placeholder rendered because a pull FAILED is exact -- driven per operator kind (fade,
# crossfade, nested) and per facet against a pull double that leaves the completion
# unsettled. Names all three kinds plus the solid/tone leaves and a real CpuBackend, so it
# is cross-component and links the umbrella `arbc`.
add_executable(arbc_operator_async_placeholder_inexact_t operator_async_placeholder_inexact.t.cpp)
target_link_libraries(arbc_operator_async_placeholder_inexact_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_operator_async_placeholder_inexact_t)

add_executable(arbc_refine_idempotence_stress_t refine_idempotence_stress.t.cpp)
target_link_libraries(arbc_refine_idempotence_stress_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_refine_idempotence_stress_t)

# runtime.interactive_worker_count_default (tier-4 counters + tier-6 tsan lane): the
# shipped interactive default renders leaf misses OFF the frame thread, which is what makes
# doc 02:61-65's deadline promise keepable at all -- at `worker_count == 0` the pool IS the
# render, so the frame thread is inside a slow leaf when the deadline passes and there is
# nothing left to degrade to. Latch-parked leaf, injected already-expired clock, thread-id
# and counter observations only; plus the parallel-path invariants (byte-identity, no
# duplicate in-flight render) swept over every worker count the benchmark sweeps. Names the
# concrete kinds beside the runtime drivers, the worker pool and a real CpuBackend --
# cross-component, so it links the umbrella `arbc`.
add_executable(arbc_interactive_worker_default_t interactive_worker_default.t.cpp)
target_link_libraries(arbc_interactive_worker_default_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_interactive_worker_default_t)

# runtime.shared_worker_pool (tier-4 counters + tier-6 tsan lane): several
# InteractiveRenderers share ONE WorkerPool, so K viewports cost N threads and not K x N.
# The two invariants sharing breaks are what these drive through the real frame loop: the
# completion WAKE (a caller-owned drain cursor, so a parked renderer cannot consume a
# sibling's settle and spuriously expire) and the TEARDOWN (a per-owner drain, so a dying
# viewport purges its own queued work and waits out its own in-flight renders without
# stopping the pool or touching a sibling's). Latches, counters and an injected clock only.
add_executable(arbc_shared_worker_pool_t shared_worker_pool.t.cpp)
target_link_libraries(arbc_shared_worker_pool_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_shared_worker_pool_t)

# The shared pool's concurrency story: K = 4 renderers over one pool, each on its own thread
# with its own TileCache, constructed and destroyed MID-FLIGHT on a seeded schedule -- the
# only way to exercise the per-owner drain against a pool a sibling is actively using. The
# brief TEST_CASE runs per-push (also under the `tsan` preset short-form); its `[.nightly]`
# seed sweep is hidden and registered in nightly.yml's tsan-full lane.
add_executable(arbc_shared_worker_pool_stress_t shared_worker_pool_stress.t.cpp)
target_link_libraries(arbc_shared_worker_pool_stress_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_shared_worker_pool_stress_t)

# --- org.arbc.imageseq (kinds.imageseq_plugin) --------------------------------
# The out-of-lib reference Timed kind's enforcing tests. They live here (not in
# plugins/) because check_claims.py / check_levels.py scan src/tests/testing, not
# plugins/; the ones that construct the content link the plugin's impl archive
# (`arbc-plugin-imageseq-impl`), while the dlopen/containment drivers reach the
# built artifacts only through the paths passed as compile definitions. The
# checked-in fixture sequence lives under the plugin's test data (doc 17:171).
set(ARBC_IMAGESEQ_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/plugins/imageseq/t/fixtures")

# Contract conformance over org.arbc.imageseq (arbc-testing before arbc; impl
# archive before both so ImageSeqContent resolves).
add_executable(arbc_imageseq_conformance_t imageseq_conformance.t.cpp)
target_link_libraries(
  arbc_imageseq_conformance_t
  PRIVATE arbc-plugin-imageseq-impl arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_conformance_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_conformance_t)

# Content-local achieved_time contract + byte-exact goldens + decode counter.
add_executable(arbc_imageseq_achieved_time_t imageseq_achieved_time.t.cpp)
target_link_libraries(arbc_imageseq_achieved_time_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_achieved_time_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_achieved_time_t)

# Through-compositor achieved-time coalescing (counters + cache hits/misses).
add_executable(arbc_imageseq_temporal_t imageseq_temporal.t.cpp)
target_link_libraries(arbc_imageseq_temporal_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_temporal_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_temporal_t)

# Temporal prefetch ring + decoder pre-roll (playback_hint) behavioral counters.
add_executable(arbc_imageseq_prefetch_t imageseq_prefetch.t.cpp)
target_link_libraries(arbc_imageseq_prefetch_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_prefetch_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_prefetch_t)

# Provided-surface honored/released + advisory-hint byte-identity (constraint 6).
add_executable(arbc_imageseq_provided_surface_t imageseq_provided_surface.t.cpp)
target_link_libraries(arbc_imageseq_provided_surface_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_provided_surface_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_provided_surface_t)

# Per-content serialization (render_thread_safe() == false) stress (tsan lane).
add_executable(arbc_imageseq_concurrency_stress_t imageseq_concurrency_stress.t.cpp)
target_link_libraries(arbc_imageseq_concurrency_stress_t
                      PRIVATE arbc-plugin-imageseq-impl arbc Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(arbc_imageseq_concurrency_stress_t
                           PRIVATE ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_concurrency_stress_t)

# The permanent end-to-end dlopen plugin-path test: boundary-only (no impl link),
# reaching the kind solely through extern "C" arbc_plugin_register.
add_executable(arbc_imageseq_plugin_path_t imageseq_plugin_path.t.cpp)
target_link_libraries(arbc_imageseq_plugin_path_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags ${CMAKE_DL_LIBS})
target_compile_definitions(
  arbc_imageseq_plugin_path_t
  PRIVATE ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>"
          ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}")
catch_discover_tests(arbc_imageseq_plugin_path_t)

# An entry-point-less MODULE fixture for the production loader test: opens under
# dlopen but exposes no arbc_plugin_register, exercising the loader's
# MissingEntryPoint (explicit load) / SkippedNoEntry (scan) paths. Self-contained
# (no arbc link) so it resolves under RTLD_NOW with zero external symbols.
add_library(arbc-plugin-noentry MODULE fixtures/noentry_plugin.cpp)
target_link_libraries(arbc-plugin-noentry PRIVATE arbc_build_flags)

# The production host-side plugin loader end-to-end test: drives PluginHost over the
# real arbc-plugin-imageseq MODULE across the extern "C" boundary (no impl link),
# plus the opt-in ARBC_PLUGIN_PATH scan and the value-erroring failure modes.
add_executable(arbc_plugin_loading_t plugin_loading.t.cpp)
target_link_libraries(arbc_plugin_loading_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags ${CMAKE_DL_LIBS})
target_compile_definitions(
  arbc_plugin_loading_t
  PRIVATE ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>"
          ARBC_IMAGESEQ_PLUGIN_DIR="$<TARGET_FILE_DIR:arbc-plugin-imageseq>"
          ARBC_IMAGESEQ_FIXTURE_DIR="${ARBC_IMAGESEQ_FIXTURE_DIR}"
          ARBC_NOENTRY_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-noentry>")
catch_discover_tests(arbc_plugin_loading_t)

# The codec-line containment proof: no decode symbol in libarbc, present in the
# plugin's impl archive and loadable module (byte scan of the built artifacts).
add_executable(arbc_imageseq_containment_t imageseq_containment.t.cpp)
target_link_libraries(arbc_imageseq_containment_t
                      PRIVATE Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(
  arbc_imageseq_containment_t
  PRIVATE ARBC_LIBARBC_FILE="$<TARGET_FILE:arbc>"
          ARBC_IMAGESEQ_IMPL_FILE="$<TARGET_FILE:arbc-plugin-imageseq-impl>"
          ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>")
catch_discover_tests(arbc_imageseq_containment_t)

# --- arbc-plugin-miniaudio (audio.device_monitor reference backend) -----------
# The out-of-lib reference DeviceSink backend's tests. Like imageseq, they live
# here (not in plugins/) because check_claims.py / check_levels.py scan
# src/tests/testing, not plugins/; the smoke test constructs the sink through the
# plugin's impl archive, while the containment driver reaches the built artifacts
# through the paths passed as compile definitions.

# Build + hardware-gated smoke check: constructs the reference sink and opens/closes
# a stream when a playback device is present (skipped in headless CI).
add_executable(arbc_miniaudio_smoke_t miniaudio_smoke.t.cpp)
target_link_libraries(arbc_miniaudio_smoke_t
                      PRIVATE arbc-plugin-miniaudio-impl arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_miniaudio_smoke_t)

# The device-line containment proof: no backend symbol in libarbc / arbc-testing,
# present in the plugin's impl archive and loadable module (byte scan of artifacts).
add_executable(arbc_miniaudio_containment_t miniaudio_containment.t.cpp)
target_link_libraries(arbc_miniaudio_containment_t
                      PRIVATE Catch2::Catch2WithMain arbc_build_flags)
target_compile_definitions(
  arbc_miniaudio_containment_t
  PRIVATE ARBC_LIBARBC_FILE="$<TARGET_FILE:arbc>"
          ARBC_TESTING_FILE="$<TARGET_FILE:arbc-testing>"
          ARBC_MINIAUDIO_IMPL_FILE="$<TARGET_FILE:arbc-plugin-miniaudio-impl>"
          ARBC_MINIAUDIO_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-miniaudio>")
catch_discover_tests(arbc_miniaudio_containment_t)

# --- kinds.dual_build: the CI-only per-kind loadable modules -------------------
# Each of the six in-lib kinds ALSO builds as a tiny MODULE exposing the doc-03
# extern "C" arbc_plugin_register entry point, so the runtime plugin path -- which
# the in-lib kinds otherwise never travel (doc 17:167-174) -- stays honest
# (doc 17:129-132). Nothing here ships: declared under tests/ (guarded by
# BUILD_TESTING), never installed, and hand-rolled exactly as plugins/imageseq is
# (packaging.plugin_helper is scoped to third-party, post-install authors).
#
# They link the umbrella `arbc`, NOT the kind-* object libraries: nothing links
# object libraries except `arbc` (cmake/ArbcComponent.cmake:11-12).
#
# They get an output directory of their own so dual_build.t.cpp's ARBC_PLUGIN_PATH
# scan assertion sweeps exactly these six, and not arbc-plugin-imageseq /
# arbc-plugin-noentry / arbc-plugin-miniaudio.
foreach(arbc_ci_kind IN ITEMS solid tone raster nested fade crossfade)
  add_library(arbc-ci-plugin-${arbc_ci_kind} MODULE
              ci_plugins/${arbc_ci_kind}_ci_plugin.cpp)
  target_link_libraries(arbc-ci-plugin-${arbc_ci_kind} PRIVATE arbc arbc_build_flags)
  set_target_properties(
    arbc-ci-plugin-${arbc_ci_kind}
    PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ci_plugins"
               RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ci_plugins")
endforeach()

# The dual-build driver. Loads each CI module through the production PluginHost and
# proves the boundary: sole registration, factory + metadata, errors-as-values, the
# audio/editable facets, host-side service injection, byte-exact in-lib/plugin render
# equivalence, and the full arbc-testing conformance suite over a factory obtained
# across dlopen. It links no plugin -- the kinds are reached only through extern "C"
# (the byte-equality leg deliberately also builds them in-lib, from `arbc`).
# arbc-testing precedes arbc on the link line (static-archive link order).
add_executable(arbc_dual_build_t dual_build.t.cpp)
target_link_libraries(
  arbc_dual_build_t
  PRIVATE arbc-testing arbc Catch2::Catch2WithMain arbc_build_flags ${CMAKE_DL_LIBS})
target_compile_definitions(
  arbc_dual_build_t
  PRIVATE ARBC_CI_PLUGIN_SOLID_FILE="$<TARGET_FILE:arbc-ci-plugin-solid>"
          ARBC_CI_PLUGIN_TONE_FILE="$<TARGET_FILE:arbc-ci-plugin-tone>"
          ARBC_CI_PLUGIN_RASTER_FILE="$<TARGET_FILE:arbc-ci-plugin-raster>"
          ARBC_CI_PLUGIN_NESTED_FILE="$<TARGET_FILE:arbc-ci-plugin-nested>"
          ARBC_CI_PLUGIN_FADE_FILE="$<TARGET_FILE:arbc-ci-plugin-fade>"
          ARBC_CI_PLUGIN_CROSSFADE_FILE="$<TARGET_FILE:arbc-ci-plugin-crossfade>"
          ARBC_CI_PLUGIN_DIR="$<TARGET_FILE_DIR:arbc-ci-plugin-solid>"
          ARBC_IMAGESEQ_PLUGIN_FILE="$<TARGET_FILE:arbc-plugin-imageseq>")
# $<TARGET_FILE:...> in COMPILE_DEFINITIONS names a path but does NOT make the named
# target a dependency, so the modules the driver dlopens must be ordered explicitly.
add_dependencies(
  arbc_dual_build_t
  arbc-ci-plugin-solid arbc-ci-plugin-tone arbc-ci-plugin-raster arbc-ci-plugin-nested
  arbc-ci-plugin-fade arbc-ci-plugin-crossfade arbc-plugin-imageseq)
catch_discover_tests(arbc_dual_build_t)

# model.per_object_revision: the cache key carries each object's OWN revision stamp, so an
# edit to one layer stops making every other layer's cached tiles unreachable. The headline
# 60-dab stroke counter, the composition-arrangement fold pinned at the pixel (its failure
# mode is a stale composite, not a slow frame), and doc 05's static-subtree caching promise
# re-enforced against the SHIPPED InteractiveRenderer rather than a stub that hand-writes its
# own contribution. Cross-component (runtime + compositor + cache + kind_nested/kind_solid +
# backend_cpu), so it links `arbc`.
add_executable(arbc_per_object_revision_keys_t per_object_revision_keys.t.cpp)
target_link_libraries(arbc_per_object_revision_keys_t
                      PRIVATE arbc Catch2::Catch2WithMain arbc_build_flags)
catch_discover_tests(arbc_per_object_revision_keys_t)
