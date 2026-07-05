#include <arbc/base/expected.hpp>
#include <arbc/surface/capabilities.hpp>
#include <arbc/surface/surface_error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

namespace {

TEST_CASE("ImportHandleTypes is an empty set by default") {
  const arbc::ImportHandleTypes handles;
  REQUIRE(handles.empty());
  REQUIRE_FALSE(handles.test(arbc::ImportHandle::CpuMemory));
  REQUIRE_FALSE(handles.test(arbc::ImportHandle::GlTexture));
  REQUIRE_FALSE(handles.test(arbc::ImportHandle::VulkanImage));
  REQUIRE_FALSE(handles.test(arbc::ImportHandle::DmaBuf));
}

TEST_CASE("ImportHandleTypes tests, unions, and compares bits") {
  const arbc::ImportHandleTypes cpu = arbc::ImportHandle::CpuMemory;
  REQUIRE_FALSE(cpu.empty());
  REQUIRE(cpu.test(arbc::ImportHandle::CpuMemory));
  REQUIRE_FALSE(cpu.test(arbc::ImportHandle::DmaBuf));

  // Union carries both members; a single-bit set is a subset of the union.
  const arbc::ImportHandleTypes both = cpu | arbc::ImportHandleTypes(arbc::ImportHandle::DmaBuf);
  REQUIRE(both.test(arbc::ImportHandle::CpuMemory));
  REQUIRE(both.test(arbc::ImportHandle::DmaBuf));
  REQUIRE_FALSE(both.test(arbc::ImportHandle::GlTexture));

  arbc::ImportHandleTypes accum;
  accum |= arbc::ImportHandle::CpuMemory;
  accum |= arbc::ImportHandle::DmaBuf;
  REQUIRE(accum == both);
  REQUIRE_FALSE(accum == cpu);
}

TEST_CASE("BackendCaps has value semantics and member-wise equality") {
  const arbc::BackendCaps a{
      .cpu_access = true,
      .import_handles = {},
      .sync_primitives = false,
  };
  const arbc::BackendCaps copy = a; // NOLINT(performance-unnecessary-copy-initialization)
  REQUIRE(copy == a);

  // Differing on any single axis breaks equality.
  arbc::BackendCaps cpu_off = a;
  cpu_off.cpu_access = false;
  REQUIRE_FALSE(cpu_off == a);

  arbc::BackendCaps with_import = a;
  with_import.import_handles = arbc::ImportHandle::CpuMemory;
  REQUIRE_FALSE(with_import == a);

  arbc::BackendCaps with_sync = a;
  with_sync.sync_primitives = true;
  REQUIRE_FALSE(with_sync == a);

  // Default-constructed caps advertise nothing (honest zero).
  const arbc::BackendCaps none;
  REQUIRE_FALSE(none.cpu_access);
  REQUIRE(none.import_handles.empty());
  REQUIRE_FALSE(none.sync_primitives);
}

TEST_CASE("SurfaceError round-trips through arbc::expected") {
  using Result = arbc::expected<std::unique_ptr<int>, arbc::SurfaceError>;

  const Result ok = std::make_unique<int>(7);
  REQUIRE(ok.has_value());
  REQUIRE(*ok.value() == 7);

  const Result err = arbc::unexpected(arbc::SurfaceError::UnsupportedFormat);
  REQUIRE_FALSE(err.has_value());
  REQUIRE(err.error() == arbc::SurfaceError::UnsupportedFormat);
}

} // namespace
