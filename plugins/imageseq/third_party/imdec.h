/* imdec.h -- single-header binary Netpbm (PPM P6 / PGM P5) image decoder.
 *
 * This is the reference plugin's private, stb-class decode dependency: a
 * header-only, permissively-licensed still-image decoder compiled ONLY into
 * `arbc-plugin-imageseq` (and the in-repo plugin test binaries), never into
 * `libarbc` or `arbc-testing` (doc 17 "codec line"; imageseq_plugin.md §1).
 * It stands in for stb_image and exposes the same shape (`imdec_*` mirrors the
 * `stbi_*` surface) so a real stb_image.h is a drop-in replacement -- the
 * codec-containment proof keys off the fact that a decode dependency's symbols
 * resolve here and never in `libarbc`, not off any particular decoder.
 *
 * Scope: 8-bit binary Netpbm (P6 RGB, P5 grayscale, maxval 255). Enough to
 * decode the checked-in fixture frames deterministically; a real codec-backed
 * kind is a separate, later plugin (doc 11:289).
 *
 * License: public domain (Unlicense) -- no doc-10 dependency decision, exactly
 * as doc 17's codec line pre-sanctions for the stb-class dep.
 *
 * Usage: define IMDEC_IMPLEMENTATION in exactly one translation unit before
 * including this header.
 */
#ifndef IMDEC_H_INCLUDED
#define IMDEC_H_INCLUDED

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Probe an image's dimensions and channel count without decoding its pixels.
 * Returns 1 on success (and fills *x, *y, *channels), 0 on any parse failure. */
int imdec_info_from_memory(const unsigned char* buffer, size_t len, int* x, int* y, int* channels);

/* Decode `buffer` into a freshly-allocated tightly-packed 8-bit RGBA image
 * (`(*x) * (*y) * 4` bytes, alpha forced to 255). Returns the buffer, or NULL
 * on any failure (unknown magic, non-255 maxval, truncated data). Free the
 * result with imdec_free. Never throws, never aborts -- failures are the NULL
 * value (boundary discipline, doc 03:177-180). */
unsigned char* imdec_load_from_memory(const unsigned char* buffer, size_t len, int* x, int* y);

/* Release a buffer returned by imdec_load_from_memory. */
void imdec_free(void* pixels);

#ifdef __cplusplus
}
#endif

#ifdef IMDEC_IMPLEMENTATION

#include <stdlib.h>

typedef struct {
  const unsigned char* p;
  const unsigned char* end;
} imdec__cursor;

/* Advance past ASCII whitespace and `#`-to-end-of-line comments (Netpbm
 * header grammar). */
static void imdec__skip_ws(imdec__cursor* c) {
  for (;;) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r')) {
      ++c->p;
    }
    if (c->p < c->end && *c->p == '#') {
      while (c->p < c->end && *c->p != '\n') {
        ++c->p;
      }
      continue;
    }
    break;
  }
}

/* Parse a non-negative ASCII integer. Returns 1 on success. */
static int imdec__read_uint(imdec__cursor* c, long* out) {
  imdec__skip_ws(c);
  if (c->p >= c->end || *c->p < '0' || *c->p > '9') {
    return 0;
  }
  long value = 0;
  while (c->p < c->end && *c->p >= '0' && *c->p <= '9') {
    value = value * 10 + (*c->p - '0');
    ++c->p;
  }
  *out = value;
  return 1;
}

/* Parse the header shared by P5/P6: magic, width, height, maxval, and the
 * single whitespace byte that must precede the binary raster. On success
 * returns 1 and leaves the cursor at the first pixel byte. */
static int imdec__parse_header(imdec__cursor* c, int* magic, long* w, long* h) {
  imdec__skip_ws(c);
  if (c->p + 2 > c->end || c->p[0] != 'P') {
    return 0;
  }
  if (c->p[1] == '5') {
    *magic = 5;
  } else if (c->p[1] == '6') {
    *magic = 6;
  } else {
    return 0;
  }
  c->p += 2;
  long maxval = 0;
  if (!imdec__read_uint(c, w) || !imdec__read_uint(c, h) || !imdec__read_uint(c, &maxval)) {
    return 0;
  }
  if (*w <= 0 || *h <= 0 || maxval != 255) {
    return 0; /* only 8-bit rasters are supported */
  }
  /* Exactly one whitespace byte separates the header from the binary data. */
  if (c->p >= c->end) {
    return 0;
  }
  ++c->p;
  return 1;
}

int imdec_info_from_memory(const unsigned char* buffer, size_t len, int* x, int* y, int* channels) {
  imdec__cursor c = {buffer, buffer + len};
  int magic = 0;
  long w = 0;
  long h = 0;
  if (!imdec__parse_header(&c, &magic, &w, &h)) {
    return 0;
  }
  if (x != NULL) {
    *x = (int)w;
  }
  if (y != NULL) {
    *y = (int)h;
  }
  if (channels != NULL) {
    *channels = (magic == 6) ? 3 : 1;
  }
  return 1;
}

unsigned char* imdec_load_from_memory(const unsigned char* buffer, size_t len, int* x, int* y) {
  imdec__cursor c = {buffer, buffer + len};
  int magic = 0;
  long w = 0;
  long h = 0;
  if (!imdec__parse_header(&c, &magic, &w, &h)) {
    return NULL;
  }
  const long pixels = w * h;
  const long src_channels = (magic == 6) ? 3 : 1;
  if ((long)(c.end - c.p) < pixels * src_channels) {
    return NULL; /* truncated raster */
  }
  unsigned char* out = (unsigned char*)malloc((size_t)pixels * 4);
  if (out == NULL) {
    return NULL;
  }
  const unsigned char* src = c.p;
  for (long i = 0; i < pixels; ++i) {
    unsigned char r, g, b;
    if (magic == 6) {
      r = src[i * 3 + 0];
      g = src[i * 3 + 1];
      b = src[i * 3 + 2];
    } else {
      r = g = b = src[i];
    }
    out[i * 4 + 0] = r;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = b;
    out[i * 4 + 3] = 255;
  }
  if (x != NULL) {
    *x = (int)w;
  }
  if (y != NULL) {
    *y = (int)h;
  }
  return out;
}

void imdec_free(void* pixels) { free(pixels); }

#endif /* IMDEC_IMPLEMENTATION */

#endif /* IMDEC_H_INCLUDED */
