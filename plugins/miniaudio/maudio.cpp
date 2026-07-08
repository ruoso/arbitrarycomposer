// The single translation unit that compiles the private miniaudio-class backend
// implementation. Kept in its own TU (like imageseq's imdec.cpp) so the backend
// symbols live only in `arbc-plugin-miniaudio-impl` (and the MODULE), never in
// `libarbc` / `arbc-testing` (doc 17 "the codec line", generalized to devices).
#define MAUDIO_IMPLEMENTATION
#include "maudio.h"
