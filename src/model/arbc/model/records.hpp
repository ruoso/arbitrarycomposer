#pragma once

#include <arbc/base/ids.hpp>
#include <arbc/base/transform.hpp>
#include <arbc/pool/slot_store.hpp> // SlotIndex

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
enum class RecordKind : std::uint32_t { Composition = 0, Layer = 1, Content = 2 };

// Sentinel meaning "no editable state captured". State handles are inert in this
// task; `model.editable_facet` gives them capture/restore semantics.
inline constexpr SlotIndex k_state_none = 0xFFFFFFFFu;

// Layer visibility bit (the low bit of the flag word). Room is left for the
// remaining placement flags (span/time-map presence, etc.) the timeline tasks add.
inline constexpr std::uint32_t k_layer_visible = 1u;

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
  std::uint32_t flags{k_layer_visible};

  bool visible() const noexcept { return (flags & k_layer_visible) != 0; }
};

// A composition holds a bounded, inline layer order for this foundation task.
// Unbounded / chunked layer order is deferred to `model.transactions` (which
// drives commit shape); the fixed inline cap keeps the record a single slab
// size class here.
inline constexpr std::size_t k_max_inline_layers = 8;

// Composition record (doc 14): canvas, working space, and the ordered layer
// list (bottom-to-top), each layer named by `ObjectId`.
struct CompositionRecord {
  double canvas_w{0.0};
  double canvas_h{0.0};
  std::uint32_t working_space{0};
  std::uint32_t layer_count{0};
  ObjectId layers[k_max_inline_layers]{};
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
static_assert(std::is_standard_layout_v<ObjectRecord> &&
                  std::is_trivially_destructible_v<ObjectRecord> &&
                  std::is_trivially_copyable_v<ObjectRecord>,
              "ObjectRecord must be a fixed-size, trivially destructible/copyable slab record");

} // namespace arbc
