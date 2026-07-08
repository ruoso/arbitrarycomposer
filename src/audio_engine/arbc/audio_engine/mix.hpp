#pragma once

#include <arbc/base/ids.hpp>         // ObjectId
#include <arbc/contract/content.hpp> // PullService, AudioRequest, AudioResult, Content, AudioFacet
#include <arbc/model/model.hpp>      // DocRoot

#include <functional>

// The composition mix engine for `arbc::audio-engine` (L4, doc 17:28,57): the
// audio analog of `compositor::render_frame` (`compositor.cpp:103-118`). Given a
// pinned `DocRoot`, a root composition id, a content resolver, a `PullService`,
// and an `AudioRequest` (window + working rate/layout + zeroed target +
// exactness + snapshot), it walks the composition's layers bottom-to-top and
// **additively** mixes each audible layer's contribution into the target -- the
// reusable core the export / device monitors drive and the Spatial policy leaf
// extends (doc 12:11-21,150-190).
//
// Levelization (doc 17:41,57): `arbc::audio-engine` reaches only `contract`
// (+ its transitive `model`/`media`/`base` closure) and `cache`. It is an L4
// peer of `compositor`, so it MUST NOT name a `compositor` type -- hence its own
// resolver typedef below rather than `compositor::ContentResolver`. The per-
// content pull machinery (block cache, worker dispatch) is the concrete
// `PullService` (compositor's `PullServiceImpl`, doc 17:56); this engine owns the
// *mix* and calls `pull_audio` for every layer, never `render_audio` inline
// (the doc-12 render-ahead asymmetry, doc 12:31-34).

namespace arbc {

// The mix engine's content resolver: `ObjectId -> Content*` over the pinned
// document's bindings. The audio-engine's own copy of the resolver seam -- it may
// not name compositor's `ContentResolver` (an L4 peer, doc 17:41) -- exactly as
// `kind-nested` carries its own `NestedResolver`.
using MixResolver = std::function<Content*(ObjectId)>;

// The per-layer contribution policy (doc 12:127-129,167-206): the mix policy is a
// *monitor* choice, with **Flat** the default (contribution = `gain x mixed`). The
// enum is the monitor-facing selector; the mechanism that actually carries the
// composed transform into the mix is the optional `AudioRequest::spatial` context
// (Decision D1) -- so on `Spatial` a monitor both names this enum AND seeds
// `request.spatial`, and the per-layer branch keys off `request.spatial` (never this
// enum, which the L3 nested walk cannot see). Passed by value, defaulting to `Flat`.
enum class MixPolicy { Flat, Spatial };

// Mix `composition`'s audible layers into `request.target`, bottom-to-top and
// additively (doc 12:11-21,202-208). A pure function over the pinned `doc`: it
// reads membership and records only from the frozen pin (no wall clock, no
// transport, no mutation), so two calls with equal (pin, composition, window,
// rate, layout) settle to **bit-identical** samples. Per layer it culls (content
// unresolved / no `audio()` facet / `!audible()` / `gain <= 0` / `span` non-
// overlap / reverse or zero rate / time-map overflow), computes the varispeed
// child rate from the layer's composed rational `time_map`, requests the layer's
// content audio at the composition's working layout and the composed rate through
// `pull_audio` (never `render_audio` directly), band-limit-reconstructs a below-
// rate child (`resample_audio`), remixes to the request layout, scales by `gain`,
// and sums -- folding `achieved_rate = min` / `exact = conjunction` over
// contributors. Returns the aggregate `AudioResult`. The walk mirrors
// `NestedContent`'s recursion prototype exactly, one level up (a duplicated cull
// loop, doc 17:41 Decision), so the top-level mix and nested's recursion are the
// same behavior at two levels.
AudioResult mix_composition(const DocRoot& doc, ObjectId composition, const MixResolver& resolve,
                            PullService& pull, const AudioRequest& request,
                            MixPolicy policy = MixPolicy::Flat);

} // namespace arbc
