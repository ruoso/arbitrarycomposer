#pragma once

#include <arbc/model/model.hpp>
#include <arbc/runtime/document.hpp>

// The attorney-client accessor granting `arbc::runtime`'s host-facing viewport objects the
// one thing a `Document` does not publish: the versioned `Model&` underneath
// (runtime.host_viewport_document_binding Decision 2). PRIVATE to the component -- it is not
// a public header, so no host can reach it, and `Document`'s public shape and member set stay
// exactly as they were. It mirrors `DocumentSerializeAccess` (`document_serialize.cpp`), the
// pattern `document.hpp` already states and justifies.
//
// Two collaborators need it, for the same reason: both take a `Model&` in their member-init
// list, so a `Document&` overload can only DELEGATE to the `Model&` one -- which is what keeps
// a single code path through a frame (`HostViewport::step`) and a single damage-slot install
// (`DamageRouter`), rather than a second, host-facing, untested mode.

namespace arbc {

struct HostViewportDocumentAccess {
  static Model& model(Document& doc) { return doc.d_model; }
};

} // namespace arbc
