/* maudio.h -- single-header, cross-platform audio-backend stand-in.
 *
 * This is the reference device plugin's private, miniaudio-class backend
 * dependency: a header-only, permissively-licensed audio device backend
 * compiled ONLY into `arbc-plugin-miniaudio` (and the in-repo plugin test
 * binaries), never into `libarbc` or `arbc-testing` (doc 17 "the codec line",
 * generalized to device backends; device_monitor.md D4). It stands in for a real
 * single-header backend (miniaudio's `ma_*` surface) and exposes the same shape
 * (`maudio_*` mirrors `ma_device_*`) so a real miniaudio.h is a drop-in
 * replacement -- the containment proof keys off the fact that a backend
 * dependency's symbols resolve here and never in `libarbc`, not off any
 * particular backend.
 *
 * Scope: a NULL playback device. `maudio_device_count()` reports the number of
 * real playback devices the host exposes -- zero in this stand-in build (a
 * headless host), which is why the hardware-gated smoke test skips in CI. Setting
 * the ARBC_AUDIO_DEVICE environment variable to a non-"0" value makes the
 * stand-in advertise one loopback device, so a developer can force-run the
 * end-to-end open/start/callback/stop path against no real hardware. A real
 * codec-backed, OS-audio backend is a later plugin (doc 12:246-259; device
 * discovery through the plugin loader is runtime.plugin_loading, M8).
 *
 * License: public domain (Unlicense) -- no doc-10 dependency decision, exactly as
 * doc 17's codec line pre-sanctions for the stb-class / miniaudio-class dep.
 *
 * Usage: define MAUDIO_IMPLEMENTATION in exactly one translation unit before
 * including this header.
 */
#ifndef MAUDIO_H_INCLUDED
#define MAUDIO_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* The device fill callback: write `frames` interleaved Float32 frames
 * (`channels * frames` floats) into `out`. `user` is the opaque pointer passed
 * in the config. Invoked on the backend's device thread. */
typedef void (*maudio_data_callback)(void* user, float* out, unsigned int frames);

typedef struct {
  unsigned int sample_rate;
  unsigned int channels;
  maudio_data_callback callback;
  void* user;
} maudio_config;

typedef struct maudio_device maudio_device;

/* Number of playback devices the host exposes (0 on a headless / stand-in host).
 * The hardware-gated smoke test opens a stream only when this is > 0. */
int maudio_device_count(void);

/* Open a playback device for `config`, or NULL if no device is present or the
 * config is degenerate. Never throws / aborts -- failure is the NULL value. */
maudio_device* maudio_device_open(const maudio_config* config);

/* Begin invoking the config's callback on the device thread. */
void maudio_device_start(maudio_device* device);

/* Stop invoking the callback; after it returns the callback is not invoked. */
void maudio_device_stop(maudio_device* device);

/* Release a device returned by maudio_device_open. */
void maudio_device_close(maudio_device* device);

#ifdef __cplusplus
}
#endif

#ifdef MAUDIO_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

struct maudio_device {
  maudio_config config;
  int running;
};

int maudio_device_count(void) {
  /* This stand-in binds no real OS audio API, so it advertises a device only
   * when the developer opts in via the environment (a loopback null device);
   * otherwise it reports a headless host and the smoke test skips. */
  const char* env = getenv("ARBC_AUDIO_DEVICE");
  if (env != NULL && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0')) {
    return 1;
  }
  return 0;
}

maudio_device* maudio_device_open(const maudio_config* config) {
  if (config == NULL || config->callback == NULL || config->sample_rate == 0 ||
      config->channels == 0) {
    return NULL;
  }
  if (maudio_device_count() <= 0) {
    return NULL;
  }
  maudio_device* device = (maudio_device*)malloc(sizeof(maudio_device));
  if (device == NULL) {
    return NULL;
  }
  device->config = *config;
  device->running = 0;
  return device;
}

void maudio_device_start(maudio_device* device) {
  if (device == NULL || device->running) {
    return;
  }
  device->running = 1;
  /* The NULL loopback device drives a few buffers synchronously to prove the
   * callback path end-to-end, then idles (a real backend would run its own RT
   * thread until stop). Output is discarded (no real hardware). */
  {
    const unsigned int frames = 256;
    float* buffer = (float*)malloc((size_t)frames * device->config.channels * sizeof(float));
    if (buffer != NULL) {
      int i;
      for (i = 0; i < 4 && device->running; ++i) {
        memset(buffer, 0, (size_t)frames * device->config.channels * sizeof(float));
        device->config.callback(device->config.user, buffer, frames);
      }
      free(buffer);
    }
  }
}

void maudio_device_stop(maudio_device* device) {
  if (device == NULL) {
    return;
  }
  device->running = 0;
}

void maudio_device_close(maudio_device* device) { free(device); }

#endif /* MAUDIO_IMPLEMENTATION */

#endif /* MAUDIO_H_INCLUDED */
