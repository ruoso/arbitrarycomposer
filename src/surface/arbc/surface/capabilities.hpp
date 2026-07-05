#pragma once

namespace arbc {

// External-handle kinds a backend can import (doc 09 "Content-provided
// surfaces"): CPU memory, a GL texture, a Vulkan image, or a DMA-BUF. The
// universe is closed and core-owned (doc 07), so a backend's import support
// is a subset of these bits, never an open registry.
enum class ImportHandle : unsigned {
  CpuMemory = 1u << 0,
  GlTexture = 1u << 1,
  VulkanImage = 1u << 2,
  DmaBuf = 1u << 3,
};

// A set of ImportHandle bits: the "which handle types?" capability axis
// (doc 09). A small value type rather than a raw integer so the set-vs-bool
// distinction from the other BackendCaps axes stays legible; the empty set is
// the honest report of a backend with no import machinery yet.
class ImportHandleTypes {
public:
  constexpr ImportHandleTypes() = default;
  constexpr ImportHandleTypes(ImportHandle handle) // NOLINT(google-explicit-constructor)
      : d_bits(static_cast<unsigned>(handle)) {}

  constexpr bool empty() const { return d_bits == 0; }
  constexpr bool test(ImportHandle handle) const {
    return (d_bits & static_cast<unsigned>(handle)) != 0;
  }

  constexpr ImportHandleTypes operator|(ImportHandleTypes other) const {
    return ImportHandleTypes(d_bits | other.d_bits);
  }
  constexpr ImportHandleTypes& operator|=(ImportHandleTypes other) {
    d_bits |= other.d_bits;
    return *this;
  }

  friend constexpr bool operator==(const ImportHandleTypes&, const ImportHandleTypes&) = default;

private:
  constexpr explicit ImportHandleTypes(unsigned bits) : d_bits(bits) {}

  unsigned d_bits = 0;
};

// The backend capability descriptor (doc 09 "Backend contract"): the
// heterogeneous "capability flags" a backend advertises through
// `Backend::capabilities()`. One value struct, not loose args (mirroring
// SurfaceFormat's tag triple): a bool for typed CPU access, a set for import
// handle types, a bool for sync primitives. A backend advertises only what it
// currently implements (capability honesty, doc 07/09).
struct BackendCaps {
  bool cpu_access = false;
  ImportHandleTypes import_handles;
  bool sync_primitives = false;

  friend constexpr bool operator==(const BackendCaps&, const BackendCaps&) = default;
};

} // namespace arbc
