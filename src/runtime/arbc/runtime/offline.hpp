#pragma once

#include <arbc/arbc_api.h>
#include <arbc/compositor/compositor.hpp>
#include <arbc/runtime/document.hpp>

#include <memory>

namespace arbc {

// One exact frame of the document's current version (doc 02, offline
// discipline). The interactive renderer — frame loop, deadlines,
// progressive refinement — is a separate driver over the same compositor.
//
// The target is allocated in the composition's configured working space (doc 07
// rule 2), read from the pinned version. A backend that cannot store that
// working space returns a `SurfaceError` (errors as values, doc 10) rather than
// aborting -- capability honesty for a document configured for an unstorable
// format.
ARBC_API expected<std::unique_ptr<Surface>, SurfaceError>
render_offline(const Document& document, const Viewport& viewport, Backend& backend);

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
ARBC_API expected<std::unique_ptr<Surface>, SurfaceError> render_offline(const Document& document,
                                                                         const DocStatePtr& state,
                                                                         const Viewport& viewport,
                                                                         Backend& backend);

} // namespace arbc
