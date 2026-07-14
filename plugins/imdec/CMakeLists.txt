# arbc-plugin-imdec -- the ONE vendored stb-class decode dependency, shared by every
# out-of-lib kind that parses a foreign image format (doc 17 "The codec line";
# kinds/image.md Decision 6).
#
# It used to live inside `plugins/imageseq/`, which was fine while imageseq was its only
# consumer. `org.arbc.image` is the second, and doc 17:228-229 says the two kinds share
# "the one vendored stb-class decoder" -- so the decoder moves BESIDE its consumers rather
# than one plugin reaching into the other's directory layout (a levelization smell the
# containment test would not catch).
#
# CONTAINMENT is the whole point: the decode symbols must resolve ONLY in the plugin tier.
# That is achieved by every consumer linking this target PRIVATE, which keeps both the
# archive's object code and its include directory off the consumer's own INTERFACE -- so
# nothing propagates further, and in particular nothing can reach `libarbc`, any `arbc_*`
# component, or `arbc-testing` (none of which link a plugin at all).
# `tests/image_containment.t.cpp` and `tests/imageseq_containment.t.cpp` are the enforcing
# byte scans.
#
# Hand-rolled, deliberately NOT `arbc_add_component` -- those fold their objects into
# `libarbc`, which is exactly the leak the codec line forbids.
add_library(arbc-plugin-imdec STATIC imdec.cpp)

# PUBLIC so a consumer reaches `<imdec.h>` through the TARGET rather than by hard-coding a
# path into this directory. Because every consumer links this target PRIVATE, the include
# dir stops at that consumer and travels no further -- the "third_party stays private to
# the plugin tier" requirement, expressed through CMake's usage requirements instead of a
# path.
target_include_directories(arbc-plugin-imdec PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_features(arbc-plugin-imdec PUBLIC cxx_std_20)
target_link_libraries(arbc-plugin-imdec PRIVATE arbc_build_flags)

# Position-independent: it is linked into MODULE (shared) plugin artifacts.
set_target_properties(arbc-plugin-imdec PROPERTIES POSITION_INDEPENDENT_CODE ON)
