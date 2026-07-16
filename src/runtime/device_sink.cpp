#include <arbc/runtime/device_sink.hpp>

namespace arbc {

// The out-of-line anchor (key function) for the polymorphic DeviceSink base: with
// its destructor defined here, DeviceSink's vtable and typeinfo are emitted only in
// libarbc and exported through the class-level ARBC_API, so a device-backend plugin
// resolves them from the single shared libarbc across the plugin-load boundary
// rather than carrying a private weak copy (packaging.shared_library_build_msvc;
// mirrors arbc::Content::~Content in contract/content.cpp).
DeviceSink::~DeviceSink() = default;

} // namespace arbc
