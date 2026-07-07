// The single translation unit that compiles the imdec decode dependency (the
// plugin's private, stb-class codec). Isolated here so the decode symbols live
// only in `arbc-plugin-imageseq-impl` (hence the MODULE and the plugin test
// binaries) and never in `libarbc` (doc 17 codec line).
#define IMDEC_IMPLEMENTATION
#include "imdec.h"
