#pragma once

#include <arbc/arbc_api.h>
#include <arbc/compositor/compositor.hpp>
#include <arbc/runtime/document.hpp>
#include <arbc/runtime/unresolved_contents.hpp> // count_unresolved_contents

#include <cstddef>
#include <memory>

namespace arbc {

// What an offline frame could not resolve (issue #35). Caller-owned and passed by optional
// trailing pointer, the established seam-growth discipline for a report a caller may not want
// (`CompositorCounters`, `GraphDiagnostics`); null is byte-for-byte the pre-issue behaviour.
struct OfflineRenderReport {
  // How many contents in the rendered frame composited NOTHING because an external reference
  // behind them is not there -- a pending fetch a deferring `AssetSource` has not answered, or
  // one it answered with absence. Zero for a document with no external references, and zero
  // for one whose source answers inline (a `FilesystemAssetSource` never leaves it non-zero),
  // which is why the whole condition is latent for a filesystem-backed host and reachable the
  // moment the source genuinely defers.
  //
  // Non-zero means THIS EXPORT HAS A HOLE: some region of the returned surface is transparent
  // where the picture should be. It is a value to act on -- warn, retry after a settle, or
  // refuse -- and it is deliberately a count and not an error, because a frame with a hole is
  // still exactly the frame the caller asked for.
  //
  // Counted against the RENDERED VERSION and the contents the anchored frame walk actually
  // reaches, never against the document's live pending maps; `unresolved_contents.hpp` states
  // the rule and why the document's own `pending_external_loads()` is the wrong question here.
  std::size_t unresolved_contents{0};
};

// One exact frame of the document's current version (doc 02, offline
// discipline). The interactive renderer — frame loop, deadlines,
// progressive refinement — is a separate driver over the same compositor.
//
// The target is allocated in the composition's configured working space (doc 07
// rule 2), read from the pinned version. A backend that cannot store that
// working space returns a `SurfaceError` (errors as values, doc 10) rather than
// aborting -- capability honesty for a document configured for an unstorable
// format.
//
// THIS PATH NEVER SETTLES (issue #35). An external reference whose bytes have not arrived
// stays unresolved for the whole render and composites the doc-05 placeholder; nothing here
// drives `settle_external_loads`, and that is deliberate, because the byte-exact-reference
// role depends on rendering exactly the version it was handed and never publishing a revision
// underneath the caller. A caller that wants resolved content settles FIRST, on the writer
// thread, and pins after. A caller that cannot guarantee quiescence passes `report` and reads
// `OfflineRenderReport::unresolved_contents`, which turns the silent hole into a number.
ARBC_API expected<std::unique_ptr<Surface>, SurfaceError>
render_offline(const Document& document, const Viewport& viewport, Backend& backend,
               OfflineRenderReport* report = nullptr);

// The same frame, rendered against a version the CALLER pinned (issue #27).
//
// The overload above pins per call, so a host exporting a batch -- N cameras, one PNG
// each -- issues N calls and each pins whatever version is current when it runs. An
// edit landing mid-batch makes item 3 reflect a document state item 1 did not, and
// nothing in the API let the host ask for the *same* version twice. Its two workarounds
// were both untenable: blocking the writer for the whole batch freezes the UI for the
// minutes a dozen 4K frames take, and snapshotting the document means a full
// serialize-and-reload per export.
//
// A `DocStatePtr` is the answer the library was already holding: pinning is what the
// versioned model is for, a pin is cheap, and it RETAINS its version -- so there is no
// "that revision is gone" error case to report, the way a revision NUMBER would have
// had. The host pins once, renders N frames against it, and drops the pin; the writer
// is never blocked and no second document exists.
//
// `state` must be a pin of `document` (they share the content binding). A null pin
// renders nothing and returns `SurfaceError::UnsupportedFormat`, the same
// errors-as-values posture as an unstorable working space.
//
// `report` is the issue-#35 unresolved-content count, answered against THIS pin -- which is
// the whole reason it is not `Document::pending_external_loads()`. A composition arrival
// installs on a NEW revision, so between the pin and the render the document's count can
// reach zero while this pin still draws the placeholder; the report reads the version that is
// actually being rendered and therefore cannot under-report that way. Null (the default)
// computes nothing and costs nothing. A null pin leaves the report zeroed: no frame was
// walked, so no content was found unresolved.
ARBC_API expected<std::unique_ptr<Surface>, SurfaceError>
render_offline(const Document& document, const DocStatePtr& state, const Viewport& viewport,
               Backend& backend, OfflineRenderReport* report = nullptr);

} // namespace arbc
