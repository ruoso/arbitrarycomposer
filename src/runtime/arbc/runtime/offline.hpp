#pragma once

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
expected<std::unique_ptr<Surface>, SurfaceError>
render_offline(const Document& document, const Viewport& viewport, Backend& backend);

} // namespace arbc
