// The single translation unit that compiles the imdec decode dependency (the plugin
// tier's private, stb-class codec). Isolated here so the decode symbols live only in
// `arbc-plugin-imdec` -- and, through the PRIVATE links, in the plugin impl archives and
// MODULEs that consume it -- and never in `libarbc` (doc 17 codec line).
#define IMDEC_IMPLEMENTATION
#include <imdec.h>
