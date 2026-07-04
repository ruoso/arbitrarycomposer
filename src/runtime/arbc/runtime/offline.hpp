#pragma once

#include <arbc/compositor/compositor.hpp>
#include <arbc/runtime/document.hpp>

#include <memory>

namespace arbc {

// One exact frame of the document's current version (doc 02, offline
// discipline). The interactive renderer — frame loop, deadlines,
// progressive refinement — is a separate driver over the same compositor.
std::unique_ptr<Surface> render_offline(const Document& document, const Viewport& viewport,
                                        Backend& backend);

} // namespace arbc
