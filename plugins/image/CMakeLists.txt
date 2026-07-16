# arbc-plugin-image -- `org.arbc.image`, the out-of-lib STILL image kind (doc 03:236) and
# the half of the pixel-persistence split that stores NOTHING in the document (doc 08
# Principle 3).
#
# It carries a PRIVATE stb-class decode dependency -- the shared `arbc-plugin-imdec`,
# vendored once and linked by both image kinds (kinds/image.md Decision 6) -- which must
# never enter `libarbc`, any `arbc_*` component, or `arbc-testing` (doc 17 "the codec
# line"; `tests/image_containment.t.cpp` is the enforcing byte scan).
#
# Only the DECODER lives here. The SERIALIZE codec for this kind lives in `runtime`
# (`src/runtime/codec_image.cpp`): it parses our own JSON and a URI string and never an
# encoded image byte, so by doc 17's own "what is being parsed" test it does not cross the
# line. That is why this plugin performs no file I/O of its own -- the core resolves the URI
# and fetches the bytes through the one `LoadContext` / `AssetSource` seam, and hands them
# here through the opaque `ContentConfig` (Decision 5).
#
# An `arbc_add_plugin()` MODULE + impl-STATIC pair, mirroring `plugins/imageseq`,
# deliberately NOT `arbc_add_component` (those fold objects into `libarbc`, dragging the
# decoder into core); the MODULE rides the shipped helper (packaging.plugin_helper).

# The kind implementation as a STATIC library so BOTH the loadable MODULE and the in-repo
# test binaries construct the content directly (no `dlopen` needed to drive the kind), while
# the decode dependency stays private to this library and the MODULE.
add_library(arbc-plugin-image-impl STATIC image_content.cpp)
target_include_directories(arbc-plugin-image-impl
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}") # arbc/kind_image/*.hpp
target_compile_features(arbc-plugin-image-impl PUBLIC cxx_std_20)
# The umbrella `arbc` PUBLIC (Content/contract symbols + the `media` resampling kernels this
# kind reuses -- L2, plugin-legal) and the decode dep PRIVATE. PRIVATE is what keeps both
# `<imdec.h>` and the decode objects off this archive's INTERFACE (constraint §1). This kind
# reaches NO `backend-cpu` kernel (L3, forbidden to a plugin) and allocates no surface
# through a `Backend`: it owns its own storage, as imageseq's `FrameSurface` does.
target_link_libraries(arbc-plugin-image-impl PUBLIC arbc PRIVATE arbc-plugin-imdec
                                                                 arbc_build_flags)

# The shipped loadable artifact: the helper's MODULE carrying only the extern "C" entry
# point. The helper links `arbc::arbc` PRIVATE; the impl (+ its private decode dep) and
# the in-tree warning flags ride the LINK_LIBRARIES seam.
arbc_add_plugin(arbc-plugin-image SOURCES image_plugin.cpp
                LINK_LIBRARIES arbc-plugin-image-impl arbc_build_flags)

# The checked-in fixture the tests decode. It is deliberately MATERIALLY LARGER than the
# compositor's 256-px tile (`tile_planning.hpp:145`), so the provided-surface claim -- that
# a pull hands back the requested region and not the whole frame (Decision 1) -- is asserted
# over an image that genuinely spans several tiles. The enforcing tests live under `tests/`
# (that tree, not `plugins/`, is what `check_claims.py` scans); the path reaches them as a
# compile definition set in `tests/CMakeLists.txt`.
