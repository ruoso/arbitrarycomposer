// The single translation unit that compiles the private miniaudio-class backend
// implementation. Kept in its own TU (like imageseq's imdec.cpp) so the backend
// symbols live only in `arbc-plugin-miniaudio-impl` (and the MODULE), never in
// `libarbc` / `arbc-testing` (doc 17 "the codec line", generalized to devices).
#define MAUDIO_IMPLEMENTATION
// MSVC deprecates getenv in favour of _dupenv_s; suppress the warning since
// getenv is both correct and portable for a read-only environment lookup.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#include "maudio.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
