# host-interactive — the interactive arbc embedding

The shape of a windowed arbc host with the window removed: a `HostViewport`
bound to a `Document`, an `InteractiveRenderer` frame loop rendering into one
persistent caller-owned surface, and pan/zoom applied as camera-transform
edits. arbc is deliberately not a GUI framework (doc 00) — the library
produces composed pixels into a surface the host provides; windowing, input
handling, and widgets belong to the host. So this example ships none of them:
the driver is a deterministic scripted **gesture tape**, which is also what
lets it run headlessly in CI on every lane.

What it demonstrates, in order:

1. **Kind bootstrap + document assembly** — as in `../host-offline/`.
2. **The host's single wiring step** (doc 01) — backend, surface pool, tile
   cache, one persistent target surface, `InteractiveRenderer`, and a
   `HostViewport` bound to the document (`DocumentBinding{}` is the right
   shape for a programmatically-built document; the constructor installs the
   document's damage sink, so edits reach the frame loop with no further
   plumbing).
3. **Pan/zoom as camera edits** (doc 01, doc 04) — a gesture is a device-space
   affine composed onto the camera's left via `HostViewport::set_camera`;
   because it composes on the device side it stays valid whatever anchor the
   viewport has rebased the camera to.
4. **The frame loop** — `HostViewport::step()` after each gesture, repeated
   while a follow-up frame is owed; the target surface persists across frames
   (doc 02) and holds the latest composed pixels at all times. The first step
   composites the bound scene even though every commit predates the binding
   (the bootstrap frame), and each camera edit repaints on the next step —
   the host never forges damage.
5. **Artifact** — the final frame is converted to straight-alpha sRGB8 through
   the library's `PixelTraits` encode and written as a PNG via
   `../common/png_writer.hpp`, so CI validates the loop's output, not just its
   exit code.

## The swap point for a real toolkit

Everything except the tape is what a production host writes. To embed under a
real windowing toolkit, replace the `tape` array and its `for` loop in
`main.cpp` with your toolkit's event loop:

- mouse-drag / touch-pan handler → `view.set_camera(compose(pan(dx, dy), view.camera()))`
- wheel / pinch handler → `view.set_camera(compose(zoom_about(factor, x, y), view.camera()))`
- a camera edit is damage (doc 02): the next `step()` repaints the full
  viewport at the new camera, with nothing else required of the host — and it
  repaints without invalidating, so panning back over already-rendered content
  re-plans from the tile cache
- frame tick (vsync callback, idle hook, or timer) → `view.step()`, re-arming
  the tick while `StepOutcome::schedule_follow_up` is set, then presenting the
  target surface (blit or texture upload — the pixels are CPU-readable
  premultiplied linear float)

Also restore a real per-frame budget (`config.budget`, typically the frame
interval — the 16ms default) instead of this example's effectively-unbounded
budget, and drop the fixed playhead source if your content is timed.

## Build and run

Against an installed arbc prefix:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/arbc/prefix
cmake --build build
./build/host_interactive out.png
```

The output path is optional (defaults to `out.png` in the working directory).
The program runs the scripted pan/zoom tape over a 512×512 scene, writes the
final frame, and exits non-zero on any failure.

CI configures, builds, and runs this project against a real staged install on
every lane (the `install.consumer` CTest) and validates the emitted PNG
byte-exactly — the scene and tape are deliberately hand-computable, so if you
change either, update `tests/consumer/host_example_artifacts.cpp` in the same
commit.
