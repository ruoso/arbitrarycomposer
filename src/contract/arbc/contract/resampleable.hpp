#pragma once

// The RESAMPLE facet (issue #31): a content that can change its own WORKING GRID -- the pixel
// lattice its render samples and its edits land on -- on a host's request.
//
// Why it is a facet and not a method on one kind. A host that surfaces a per-cell detail floor
// ("this cell is magnified past its native grid") wants the obvious next affordance: "resample
// to crisp", growing the grid to match the camera's pixel density so subsequent painting has
// the resolution to land on. Without a generic verb, the host has to name `RasterContent`, own
// the upsampling itself, and keep a per-kind allowlist -- which is exactly what the `Registry`
// seam exists to avoid, and it puts levelization in the host, where `arbc::media`'s resampler
// is the KIND's floor rather than the host's business.
//
// THE DISCOVERY HALF MATTERS AS MUCH AS THE VERB (the `Registry::insert_schema` pattern, issue
// #21). A painted `org.arbc.raster` owns its pixels and returns a facet; a REFERENCED
// `org.arbc.image` is limited by its source file and returns `nullptr`, and a host reads that
// as "source-limited", greys the action, and gives an honest reason -- without knowing which
// kinds are resizable.
//
// It is a WRITER-THREAD, TRANSACTIONAL verb, deliberately shaped like `RasterContent::paint`:
// the kind builds the new version, assigns it with `Transaction::set_content_state` and adds
// its own damage, so one call is one journal entry, undoable, with the content's `ObjectId`
// preserved. A host that already drives paint through a command needs no new protocol.

#include <arbc/arbc_api.h>
#include <arbc/base/ids.hpp>    // ObjectId
#include <arbc/model/model.hpp> // Model::Transaction (the publish seam, doc 14)

#include <optional>

namespace arbc {

// The optional resample facet, discovered through `Content::resampleable()`. The shape mirrors
// `Editable` and `AudioFacet`: pure virtuals, a virtual destructor, non-copyable, a protected
// default ctor -- the L3 interface only (doc 17:53), no state and no filter machinery (that is
// the kind's own, over `arbc::media`).
class ARBC_API Resampleable {
public:
  // A working grid in the content's OWN local pixel units.
  struct Grid {
    int width{0};
    int height{0};

    friend bool operator==(const Grid&, const Grid&) = default;
  };

  Resampleable(const Resampleable&) = delete;
  Resampleable& operator=(const Resampleable&) = delete;
  virtual ~Resampleable() = default;

  // The grid this content currently holds -- what a host compares against the density its
  // camera is asking for, to decide whether "resample to crisp" would gain anything.
  virtual Grid working_grid() const = 0;

  // Resample onto `target`, publishing the result as a new content state on `txn` and adding
  // the damage it implies. `self` is this content's object id, exactly as in
  // `RasterContent::paint`. Returns the grid ACTUALLY achieved (a kind may clamp to what it can
  // afford), or `nullopt` if it declined and published nothing -- an empty or negative target,
  // or a build that could not allocate.
  //
  // THE CONTENT'S LOCAL EXTENT FOLLOWS THE GRID. A content's local space is its own pixels, so
  // a content resampled from 512x512 to 1024x1024 reports twice the `bounds()` afterwards; it
  // has no knowledge of the layer transform that places it, and so cannot preserve its world
  // footprint on its own. A host that wants the cell to stay put scales the layer transform by
  // the ratio of the returned grid to the one `working_grid()` reported before the call -- in
  // the SAME transaction, so the change stays one undoable step.
  virtual std::optional<Grid> resample(Model::Transaction& txn, ObjectId self, Grid target) = 0;

protected:
  Resampleable() = default;
};

} // namespace arbc
