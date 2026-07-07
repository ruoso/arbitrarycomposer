# arbc-plugin-imageseq -- the out-of-lib reference `Timed` kind (doc 11) and the
# permanent end-to-end test of the runtime plugin path (doc 17 "codec line").
#
# It carries its OWN private stb-class decode dependency (third_party/imdec.h),
# which must never enter `libarbc` / `arbc-testing` (imageseq_plugin.md §1). This
# is a hand-rolled MODULE target, NOT an `arbc_add_component` (those fold into
# `libarbc`, dragging the codec into core); `packaging.plugin_helper` (M9)
# migrates it onto the shipped `arbc_add_plugin()` helper later (Decision 6).

# The kind implementation as a STATIC library so BOTH the loadable MODULE and the
# in-repo test binaries construct the content directly, while the decode
# dependency stays private to this library (and the MODULE), never in `libarbc`.
add_library(
  arbc-plugin-imageseq-impl STATIC
  imageseq_content.cpp
  imdec.cpp)
target_include_directories(
  arbc-plugin-imageseq-impl
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"                # arbc/kind_imageseq/*.hpp
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")  # imdec.h -- private decode dep
target_compile_features(arbc-plugin-imageseq-impl PUBLIC cxx_std_20)
# Links the umbrella `arbc` (PUBLIC: Content/contract symbols + headers) plus its
# own PRIVATE decode dep (compiled into imdec.cpp). The decode dep is NOT added
# to any `arbc_*` component, to `arbc`, or to `arbc-testing` (constraint §1).
target_link_libraries(arbc-plugin-imageseq-impl PUBLIC arbc PRIVATE arbc_build_flags)

# The shipped loadable artifact: a MODULE carrying only the extern "C" entry
# point, linking the impl (+ its private decode dep) and the umbrella `arbc`.
add_library(arbc-plugin-imageseq MODULE imageseq_plugin.cpp)
target_link_libraries(arbc-plugin-imageseq PRIVATE arbc-plugin-imageseq-impl arbc arbc_build_flags)

# The checked-in tiny fixture sequence the tests decode, and the built plugin's
# path -- both handed to the `tests/` binaries via compile definitions (the test
# tree, not `plugins/`, is what `check_claims.py` scans, so the enforcing tests
# live under `tests/`; see tests/CMakeLists.txt).
set(ARBC_IMAGESEQ_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/t/fixtures"
    CACHE INTERNAL "imageseq checked-in fixture frame directory")
