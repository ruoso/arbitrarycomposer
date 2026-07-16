# host-offline — the minimal arbc embedding

One exact frame of a small document, rendered offline and written to a PNG:
doc 01's "the offline renderer is just a viewport with no deadline," made
executable. This is the batch/export embedder's shape — no frame loop, no
deadline, no window.

What it demonstrates, in order:

1. **Kind bootstrap** — `arbc::register_builtin_kinds(Registry&)`, the host's
   one call that presents every built-in kind through the same `Registry`
   surface loaded plugins register into.
2. **Document assembly** — `add_composition`, `add_content`, `add_layer`,
   `attach_layer`; one content built through the registry's factory (the
   config-driven path) and one constructed directly (the programmatic path).
3. **One exact frame** — `arbc::render_offline(document, viewport, backend)`
   on the reference `CpuBackend`; failures come back as values, never throws.
4. **Working space → image** — the frame is premultiplied linear-light float;
   `arbc::PixelTraits<PixelFormat::Rgba8Srgb>::encode` performs the library's
   own unpremultiply + linear→sRGB conversion to straight-alpha sRGB8.
5. **PNG output** — through `../common/png_writer.hpp`, a ~150-line
   dependency-free writer (stored-deflate blocks), kept example-local so this
   project's entire dependency surface stays `arbc::arbc` (doc 10: embedding
   the core imposes no codecs, GPU SDKs, or GUI toolkits).

## Build and run

Against an installed arbc prefix:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/arbc/prefix
cmake --build build
./build/host_offline out.png
```

The output path is optional (defaults to `out.png` in the working directory).
The program prints the compiled-against and linked arbc versions, renders a
32×32 scene, and exits non-zero on any failure.

CI configures, builds, and runs this project against a real staged install on
every lane (the `install.consumer` CTest) and validates the emitted PNG
byte-exactly — the scene is deliberately hand-computable, so if you change it,
update `tests/consumer/host_example_artifacts.cpp` in the same commit.
