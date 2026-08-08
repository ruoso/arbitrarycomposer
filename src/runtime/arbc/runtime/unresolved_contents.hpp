#pragma once

// Which contents in a RENDERED VERSION name an external reference that is not there
// (issue #35) -- the count an offline render reports, so an export with a hole in it
// becomes a value the caller can act on instead of a silently transparent region.
//
// The interactive path already has this conversation: `Document::pending_external_loads()`
// says how many fetches are outstanding, `settle_external_loads` installs the arrivals, and
// `HostViewport::StepOutcome::external_loads_ready` tells the host a settle is owed. An
// OFFLINE render has none of it engaged -- and must not, because `render_offline`'s
// byte-exact-reference role depends on doing exactly what it is told and never settling
// underneath the caller (doc 02:73-85). What was missing is the REPORT: the caller could not
// learn that the frame it just exported has a hole.
//
// WHY NOT THE DOCUMENT'S PENDING COUNT. `Document::pending_external_loads()` answers about
// the document NOW, and an offline frame renders a PIN. The two disagree in both directions.
// A composition arrival installs a new `CompositionRecord` on a NEW revision, so a pin taken
// before the settle still renders the placeholder while the document's count has already
// dropped to zero -- an under-report, which is the dangerous direction. And a fetch
// outstanding for a content nobody in this camera's frame names is not this frame's hole --
// an over-report. So the question is asked of the pinned version and the contents it
// actually reaches, and never of the pending maps (which are writer-thread-only state a
// render thread must not read at all).
//
// WHAT COUNTS AS UNRESOLVED. Exactly the two ways a content can NAME an external target and
// not have it, which are the two `Content` discovery virtuals that name one
// (`content.hpp`):
//
//   * `external_composition_ref()` is non-empty -- the content names another project file --
//     and `composition_ref()` either is invalid (the load answered UNAVAILABLE) or names no
//     `CompositionRecord` in this version (the load is still PENDING). Both render the doc-05
//     placeholder (doc 05:50-52).
//   * `external_asset_ref()` is non-empty -- the content names an encoded asset -- and its
//     `bounds()` are EMPTY, which for an asset-backed kind is precisely the "no bytes"
//     state: `org.arbc.image` mints pending and unavailable in the same empty-extent shape
//     and deliberately fabricates no extent for bytes that have not arrived
//     (`image_content.cpp:463-472`, doc 08:135-144). An asset-backed content that HAS its
//     bytes has a real extent, and an evicted one keeps the extent it had, so residency
//     never registers here.
//
// PENDING AND UNAVAILABLE ARE COUNTED ALIKE, on purpose. They differ by whether the source
// answered, which is the loader's distinction to care about; from the exporter's side both
// are one thing -- a region that composited nothing because its bytes are not here -- and a
// count that reported only the pending half would still let a missing file export silently.
//
// A layer naming an `ObjectId` that resolves to no `Content` is NOT counted: that is a
// dangling model record, not an external reference, and folding the two would make the
// number mean two things.

#include <arbc/arbc_api.h>
#include <arbc/base/ids.hpp> // ObjectId

#include <cstddef>
#include <functional>

namespace arbc {

class Content;
class DocRoot;

// Count the unresolved contents reachable from `anchor` in `state`, per the rule above.
//
// The walk is ANCHORED and reaches exactly what the frame draws: `anchor`'s member layers,
// each layer content's operator `inputs()` (an unresolved image under a fade is still a hole),
// and each nesting content's child composition, transitively. An INSTALLED external child IS
// descended -- its records live in this document's model and its members composite in this
// frame, so a chained reference that resolved one level and not the next is still counted.
// A content reached twice is counted once (it is one region), and a document that references
// itself terminates on the same visited guard.
//
// An invalid `anchor` (a document with no composition) reaches nothing and counts zero.
//
// `resolve` maps a layer record's `ObjectId` to its live `Content*` -- the driver's document
// resolver. Reads only the pinned version and the contents' const metadata virtuals, the same
// reads the frame walk itself makes, so it is safe wherever the render is.
ARBC_API std::size_t count_unresolved_contents(const DocRoot& state,
                                               const std::function<Content*(ObjectId)>& resolve,
                                               ObjectId anchor);

} // namespace arbc
