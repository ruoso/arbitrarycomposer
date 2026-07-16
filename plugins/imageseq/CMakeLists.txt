# arbc-plugin-imageseq -- the out-of-lib reference `Timed` kind (doc 11) and the
# permanent end-to-end test of the runtime plugin path (doc 17 "codec line").
#
# It carries a private stb-class decode dependency, which must never enter `libarbc` /
# `arbc-testing` (imageseq_plugin.md §1). That decoder used to be vendored HERE
# (`third_party/imdec.h` + `imdec.cpp`); `org.arbc.image` is its second consumer, so it
# now lives beside both as the shared `arbc-plugin-imdec` target (kinds/image.md
# Decision 6, doc 17:228-229 "the one vendored stb-class decoder"). Linking it PRIVATE
# preserves the containment exactly as compiling it in did. The MODULE is defined via the
# shipped `arbc_add_plugin()` helper (packaging.plugin_helper), NOT `arbc_add_component`
# (those fold into `libarbc`, dragging the codec into core).

# The kind implementation as a STATIC library so BOTH the loadable MODULE and the
# in-repo test binaries construct the content directly, while the decode
# dependency stays private to this library (and the MODULE), never in `libarbc`.
add_library(
  arbc-plugin-imageseq-impl STATIC
  imageseq_content.cpp)
target_include_directories(arbc-plugin-imageseq-impl
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}") # arbc/kind_imageseq/*.hpp
target_compile_features(arbc-plugin-imageseq-impl PUBLIC cxx_std_20)
# Links the umbrella `arbc` (PUBLIC: Content/contract symbols + headers) plus the shared
# decode dep PRIVATE. PRIVATE is what keeps `<imdec.h>` and the decode objects off this
# archive's INTERFACE: the decode dep is NOT added to any `arbc_*` component, to `arbc`,
# or to `arbc-testing` (constraint §1).
target_link_libraries(arbc-plugin-imageseq-impl PUBLIC arbc PRIVATE arbc-plugin-imdec
                                                                    arbc_build_flags)

# The shipped loadable artifact: the helper's MODULE carrying only the extern "C"
# entry point. The helper links `arbc::arbc` PRIVATE; the impl (+ its private decode
# dep) and the in-tree warning flags ride the LINK_LIBRARIES seam.
arbc_add_plugin(arbc-plugin-imageseq SOURCES imageseq_plugin.cpp
                LINK_LIBRARIES arbc-plugin-imageseq-impl arbc_build_flags)

# The checked-in tiny fixture sequence the tests decode, and the built plugin's
# path -- both handed to the `tests/` binaries via compile definitions (the test
# tree, not `plugins/`, is what `check_claims.py` scans, so the enforcing tests
# live under `tests/`; see tests/CMakeLists.txt).
set(ARBC_IMAGESEQ_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/t/fixtures"
    CACHE INTERNAL "imageseq checked-in fixture frame directory")
