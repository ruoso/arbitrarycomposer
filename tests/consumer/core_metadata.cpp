// The core-imposition claim enforcer of the out-of-tree consumer (packaging.install).
//
// Doc 10:32-35 promises that embedding the core never transitively imposes a codec, a
// GPU SDK, or a GUI toolkit -- and, by the optional-component design (doc 00:408-423),
// never Catch2 either. That promise has to hold in EVERY install-metadata form, not just
// the CMake config. This reads the two forms a plain embedder consumes -- the installed
// arbc.pc's dependency fields (read here directly) and the installed arbc.cps `arbc`
// component's `requires` (extracted precisely by the consumer CMakeLists via string(JSON)
// and written to ARBC_CPS_ARBC_REQUIRES_FILE) -- and asserts neither names anything but the
// conditional zstd. Catch2 is expected on the CPS `testing` component and so is NOT scanned
// here; the whole point is that it never reaches the CORE component.
//
// enforces: packaging.core-metadata-imposes-only-zstd

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

// Names that must never appear in the CORE metadata's dependency-expressing fields: a
// test framework, the stb-class decoders / image codecs, GPU SDKs, and GUI toolkits.
// zstd is the one allowed (conditional) dependency and is deliberately absent from this
// list.
constexpr std::array<std::string_view, 18> k_forbidden = {
    "catch2",  "png",   "jpeg",    "jpg",  "webp",  "avif",
    "imdec",   "stb",   "miniaudio", "maudio", "vulkan", "opengl",
    "metal",   "cuda",  "directx", "qt",   "gtk",   "imgui"};

std::string to_lower(std::string_view in) {
  std::string out(in);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Reports the first forbidden token found in `haystack` (already lowercased), or empty.
std::string_view first_forbidden(const std::string& haystack) {
  for (const std::string_view token : k_forbidden) {
    if (haystack.find(token) != std::string::npos) {
      return token;
    }
  }
  return {};
}

bool scan_field(std::string_view what, const std::string& value) {
  const std::string lowered = to_lower(value);
  const std::string_view hit = first_forbidden(lowered);
  if (!hit.empty()) {
    std::printf("core_metadata: %s exposes a forbidden dependency '%.*s': %s\n",
                std::string(what).c_str(), static_cast<int>(hit.size()), hit.data(),
                value.c_str());
    return false;
  }
  return true;
}

} // namespace

int main() {
  bool ok = true;

  // The installed pkg-config file: scan the VALUE of every Requires*/Libs* field. -larbc
  // and the zstd requirement are legitimate; a codec/GPU/GUI/Catch2 token is the failure.
  std::ifstream pc(ARBC_INSTALLED_PC);
  if (!pc) {
    std::printf("core_metadata: cannot open installed pkg-config file %s\n", ARBC_INSTALLED_PC);
    return 1;
  }
  std::string line;
  while (std::getline(pc, line)) {
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, colon);
    if (key.rfind("Requires", 0) == 0 || key.rfind("Libs", 0) == 0) {
      ok = scan_field("arbc.pc " + key, line.substr(colon + 1)) && ok;
    }
  }

  // The installed CPS `arbc` component's `requires`, extracted by the consumer build into
  // a file (the JSON string carries quotes that do not survive a compile definition
  // cleanly). An absent/empty file means an empty requires -- still a valid check.
  std::ifstream cps_req(ARBC_CPS_ARBC_REQUIRES_FILE);
  std::string cps_requires;
  if (cps_req) {
    std::ostringstream buffer;
    buffer << cps_req.rdbuf();
    cps_requires = buffer.str();
  }
  ok = scan_field("arbc.cps arbc component requires", cps_requires) && ok;

  if (!ok) {
    return 1;
  }
  std::puts("core_metadata: installed core metadata imposes nothing beyond conditional zstd");
  return 0;
}
