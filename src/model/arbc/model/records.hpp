#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/rational_time.hpp> // TimeMap (transitively TimeRange/Time) -- temporal placement
#include <arbc/base/transform.hpp>
#include <arbc/media/audio_format.hpp> // AudioFormat (per-composition working audio format, doc 12)
#include <arbc/media/surface_format.hpp> // SurfaceFormat (per-composition working space, doc 07)
#include <arbc/pool/slot_store.hpp>      // SlotIndex

#include <cstdint>
#include <type_traits>

namespace arbc {

// The immutable object records a DocState version maps `ObjectId` to (doc 14):
// composition, layer, and content records. Every record is standard-layout,
// fixed-size, and pointer-free (in-record references are index-only), so it can
// live in a mmapped, position-independent workspace-file arena (doc 15:192-214,
// 243-245). The records reference OTHER objects by `ObjectId` (the map key,
// resolved through the DocState), never by owned pointer or count -- which is
// exactly why they are trivially destructible: an object record owns no slot, so
// dropping one runs no cascade (the HAMT leaf that binds it owns its count).

// Discriminator for the tagged `ObjectRecord`. Several record shapes share one
// slab size class (one `RefStore<ObjectRecord>`), so a slot recycled from one
// kind to another reuses the same count column (doc 15:230-236,
// 15-memory-model#one-count-column-per-size-class).
enum class RecordKind : std::uint32_t {
  Composition = 0,
  Layer = 1,
  Content = 2,
  LayerOrderChunk = 3
};

// Sentinel meaning "no editable state captured". State handles are inert in this
// task; `model.editable_facet` gives them capture/restore semantics.
inline constexpr SlotIndex k_state_none = 0xFFFFFFFFu;

// Layer visibility bit (the low bit of the flag word). Room is left for the
// remaining placement flags the timeline tasks add.
inline constexpr std::uint32_t k_layer_visible = 1u;

// Layer audibility bit (doc 12:89-92): the audio twin of `k_layer_visible`. A
// layer whose audio does not contribute to the mix clears it (the audio `visible`
// cull). Default-set on every new layer, exactly as `k_layer_visible` is.
inline constexpr std::uint32_t k_layer_audible = 1u << 1;

// A slab reference to a content object's editable state (doc 15:237-239). A
// fixed-size, index-only handle -- SlotRef-shaped -- defined but INERT here: no
// capture/restore, never populated (default is `k_state_none`).
struct StateHandle {
  SlotIndex slot{k_state_none};

  bool has_state() const noexcept { return slot != k_state_none; }
  friend bool operator==(const StateHandle&, const StateHandle&) = default;
};

// Content record (doc 14): a kind id plus the (inert) editable-state handle.
// `model.content_binding` populates these from the runtime side-map.
struct ContentRecord {
  std::uint64_t kind{0};
  StateHandle state{};
};

// Layer placement in its parent composition (doc 14). The bound content is named
// by `ObjectId` (the DocState map key of a ContentRecord), not by owned edge, so
// the layer owns no count and is trivially destructible.
struct LayerRecord {
  ObjectId content{};
  Affine transform{};
  double opacity{1.0};
  // The audio placement gain (doc 12:89-92), the additive-mix twin of `opacity`:
  // 0..inf (unclamped -- boosting is legitimate), scaling this layer's audio
  // contribution. A visual-only layer carries the harmless default and never
  // reads it; the audio facet of a nesting kind (`org.arbc.nested`) is its first
  // consumer. Placed beside `opacity` (the two placement doubles) so the record
  // stays fixed-size and standard-layout.
  double gain{1.0};
  std::uint32_t flags{k_layer_visible | k_layer_audible};
  // Temporal placement beside the spatial `transform` (doc 11:59-71, doc
  // 01:33-36): the parent-time span the layer exists over, and the 1D affine
  // map from parent time to content-local time. The defaults make a layer with
  // no temporal placement a still -- always-present span, identity time map --
  // so a still is the degenerate case, not a special record (doc 11:61-64).
  // Trailing, default-initialized members: `add_layer`'s aggregate init stays
  // valid and every construction path gets the still-default for free.
  TimeRange span{TimeRange::all()};
  TimeMap time_map{};

  bool visible() const noexcept { return (flags & k_layer_visible) != 0; }
  bool audible() const noexcept { return (flags & k_layer_audible) != 0; }
};

// A composition holds its ordered layer list inline up to this fixed cap; beyond
// it the order spills to a HAMT-backed chain of `LayerOrderChunk` objects
// (`model.composition_membership`, doc 14 § The central decision). The fixed cap
// keeps the composition record a single slab size class.
inline constexpr std::size_t k_max_inline_layers = 8;

// A spill chunk for a composition whose layer order exceeds `k_max_inline_layers`
// (doc 14). Holds up to `k_max_inline_layers` ordered member ids plus the
// `ObjectId` of the next chunk in the chain (invalid == chain end). Chunks are
// ordinary objects in the DocState HAMT keyed by their own `ObjectId`; a
// composition names the chain head by `ObjectId` value (see `spill_root`), never
// an owning edge -- so, like every other record, a chunk owns no slot and is
// trivially destructible (records.hpp:12-19). Its size class stays within the
// `CompositionRecord` arm (9 `ObjectId`s + a word, vs 8 `ObjectId`s + two doubles).
struct LayerOrderChunk {
  std::uint32_t count{0};
  ObjectId members[k_max_inline_layers]{};
  ObjectId next{};
};

// Composition record (doc 14): canvas, working space, and the ordered layer list
// (bottom-to-top), each layer named by `ObjectId`. The order is inline in
// `layers[0, layer_count)` while `layer_count <= k_max_inline_layers` and
// `spill_root` is invalid; past the cap it lives in the `LayerOrderChunk` chain
// headed by `spill_root` (the inline array is then dead) and `layer_count`
// remains the authoritative total (`model.composition_membership`, doc 14).
//
// `working_space` is the `SurfaceFormat` the compositor blends this composition
// in (doc 07 rule 2) -- per-composition configuration mutated through a
// `Transaction` like any other record field (`color.working_space`). It defaults
// to the doc 07 walking-skeleton working format (`k_working_rgba32f`); the 16f
// designed default flips on when kernels make it storable. A media descriptor is
// level-1 vocabulary, so carrying it here is the `model -> media` edge doc 17
// scopes for exactly this record.
//
// `working_audio_format` is the `AudioFormat` (working sample rate + channel
// layout, float32) the mix engine pulls and converts this composition toward
// (doc 12:95-105) -- the audio twin of `working_space`, riding the same
// per-composition-configuration edge and mutated through a `Transaction`
// (`audio.audio_types`). It defaults to the doc 12 default (`k_working_audio`,
// 48 kHz stereo), which -- unlike the staged pixel default -- is fully
// functional from day one.
struct CompositionRecord {
  double canvas_w{0.0};
  double canvas_h{0.0};
  SurfaceFormat working_space{};
  AudioFormat working_audio_format{};
  std::uint32_t layer_count{0};
  ObjectId layers[k_max_inline_layers]{};
  ObjectId spill_root{};
};

// The tagged object record the DocState map binds each `ObjectId` to. One record
// type -> one slab size class -> one `RefStore<ObjectRecord>`; the active arm is
// selected by `kind`. Trivially destructible (owns no slot), standard-layout,
// and pointer-free -- so a HAMT leaf can hold a `SlotRef<ObjectRecord>` edge to
// it inside a mmapped node.
struct ObjectRecord {
  RecordKind kind{RecordKind::Layer};
  ObjectId id{};
  union {
    CompositionRecord composition;
    LayerRecord layer;
    ContentRecord content;
    LayerOrderChunk order_chunk;
  } as;

  // Value-initialize the union (defined state for asan/debug); every real record
  // is filled in place after `create`.
  ObjectRecord() : as{} {}
};

static_assert(std::is_standard_layout_v<StateHandle> && std::is_trivially_copyable_v<StateHandle>,
              "StateHandle must be an index-only slab handle");
static_assert(std::is_standard_layout_v<ContentRecord> &&
                  std::is_trivially_destructible_v<ContentRecord>,
              "ContentRecord must be a fixed-size, trivially destructible slab record");
static_assert(std::is_standard_layout_v<LayerRecord> &&
                  std::is_trivially_destructible_v<LayerRecord>,
              "LayerRecord must be a fixed-size, trivially destructible slab record");
static_assert(std::is_standard_layout_v<CompositionRecord> &&
                  std::is_trivially_destructible_v<CompositionRecord>,
              "CompositionRecord must be a fixed-size, trivially destructible slab record");
static_assert(std::is_standard_layout_v<LayerOrderChunk> &&
                  std::is_trivially_destructible_v<LayerOrderChunk>,
              "LayerOrderChunk must be a fixed-size, trivially destructible slab record");
static_assert(sizeof(LayerOrderChunk) <= sizeof(CompositionRecord),
              "the spill chunk arm must not grow the ObjectRecord union size class");
static_assert(std::is_standard_layout_v<ObjectRecord> &&
                  std::is_trivially_destructible_v<ObjectRecord> &&
                  std::is_trivially_copyable_v<ObjectRecord>,
              "ObjectRecord must be a fixed-size, trivially destructible/copyable slab record");

} // namespace arbc
