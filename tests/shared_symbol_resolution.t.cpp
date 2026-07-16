// The single-instance symbol-resolution proof (packaging.shared_library_build,
// doc 17:173-182; Decision D3). Built ONLY under BUILD_SHARED_LIBS (wired behind
// an if(BUILD_SHARED_LIBS) in tests/CMakeLists.txt), so the assertions below are
// never vacuous in the static lanes -- there a plugin carries a PRIVATE static
// copy of every core object it references and the property this test pins does
// not hold.
//
// enforces: 17-internal-components#plugin-resolves-core-symbols-from-host-image
//
// The difference between "a plugin resolves core symbols FROM THE HOST IMAGE" and
// "a plugin carries a private static copy" is a LINKAGE property the existing
// dual_build render/facet/service assertions cannot see -- they pass with two
// copies as happily as with one. Under the shared build a plugin's core references
// are UNDEFINED dynamic imports satisfied at load by the single libarbc.so; under
// the static build they are local definitions. A dynamic-symbol-table scan of the
// built artifacts (paths passed as compile definitions, the containment-test shape
// of miniaudio_containment.t.cpp) sees it directly, is deterministic, and needs no
// subprocess.
//
// ELF-only, which is exactly the scope of the gcc BUILD_SHARED_LIBS lane
// (Decision D4); the MSVC shared build is packaging.shared_library_build_msvc.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <cxxabi.h>
#include <elf.h>

namespace {

// One dynamic-symbol-table entry, reduced to what the proof needs.
struct DynSym {
  std::string mangled;   // raw .dynsym name (the linker's identity)
  std::string demangled; // Itanium-demangled, or == mangled if not a C++ symbol
  bool defined = false;  // st_shndx != SHN_UNDEF
  bool global = false;   // STB_GLOBAL binding (a strong ref/def)
  bool weak = false;     // STB_WEAK binding
  bool default_vis = false;
};

std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string demangle(const std::string& name) {
  int status = 0;
  char* out = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
  if (status == 0 && out != nullptr) {
    std::string result(out);
    std::free(out);
    return result;
  }
  std::free(out);
  return name;
}

// Parse the .dynsym of an ELF64 shared object / module into DynSym rows. The
// artifacts under test are all ELFCLASS64 on the gcc-shared lane; a non-ELF64
// input fails the REQUIREs rather than reading garbage.
std::vector<DynSym> read_dynsyms(const char* path) {
  const std::string blob = read_file(path);
  REQUIRE(blob.size() >= sizeof(Elf64_Ehdr));
  const char* base = blob.data();

  Elf64_Ehdr ehdr;
  std::memcpy(&ehdr, base, sizeof(ehdr));
  REQUIRE(std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) == 0);
  REQUIRE(ehdr.e_ident[EI_CLASS] == ELFCLASS64);
  REQUIRE(ehdr.e_shentsize == sizeof(Elf64_Shdr));
  REQUIRE(ehdr.e_shoff + static_cast<std::uint64_t>(ehdr.e_shnum) * ehdr.e_shentsize <= blob.size());

  auto section = [&](std::size_t i) {
    Elf64_Shdr shdr;
    std::memcpy(&shdr, base + ehdr.e_shoff + i * sizeof(Elf64_Shdr), sizeof(shdr));
    return shdr;
  };

  std::vector<DynSym> syms;
  for (std::size_t i = 0; i < ehdr.e_shnum; ++i) {
    const Elf64_Shdr sh = section(i);
    if (sh.sh_type != SHT_DYNSYM) {
      continue;
    }
    REQUIRE(sh.sh_entsize == sizeof(Elf64_Sym));
    const Elf64_Shdr strtab = section(sh.sh_link);
    const char* strs = base + strtab.sh_offset;
    const std::size_t count = sh.sh_entsize ? sh.sh_size / sh.sh_entsize : 0;
    for (std::size_t s = 1; s < count; ++s) { // entry 0 is the null symbol
      Elf64_Sym sym;
      std::memcpy(&sym, base + sh.sh_offset + s * sizeof(Elf64_Sym), sizeof(sym));
      if (sym.st_name == 0) {
        continue;
      }
      DynSym row;
      row.mangled = std::string(strs + sym.st_name);
      row.demangled = demangle(row.mangled);
      row.defined = sym.st_shndx != SHN_UNDEF;
      const unsigned char bind = ELF64_ST_BIND(sym.st_info);
      row.global = bind == STB_GLOBAL;
      row.weak = bind == STB_WEAK;
      row.default_vis = ELF64_ST_VISIBILITY(sym.st_other) == STV_DEFAULT;
      syms.push_back(std::move(row));
    }
  }
  return syms;
}

// Does the artifact EXPORT this symbol -- defined, global/weak, default-visibility?
bool exports(const std::vector<DynSym>& syms, const std::string& mangled) {
  for (const auto& s : syms) {
    if (s.mangled == mangled && s.defined && (s.global || s.weak) && s.default_vis) {
      return true;
    }
  }
  return false;
}

bool exports_demangled_containing(const std::vector<DynSym>& syms, const std::string& needle) {
  for (const auto& s : syms) {
    if (s.defined && (s.global || s.weak) && s.default_vis &&
        s.demangled.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

const char* const k_plugins[] = {
    ARBC_CI_PLUGIN_SOLID_FILE,  ARBC_CI_PLUGIN_TONE_FILE,   ARBC_CI_PLUGIN_RASTER_FILE,
    ARBC_CI_PLUGIN_NESTED_FILE, ARBC_CI_PLUGIN_FADE_FILE,   ARBC_CI_PLUGIN_CROSSFADE_FILE,
    ARBC_IMAGESEQ_PLUGIN_FILE,  ARBC_MINIAUDIO_PLUGIN_FILE,
};

} // namespace

TEST_CASE("libarbc.so exports its deliberate ARBC_API public surface") {
  const std::vector<DynSym> core = read_dynsyms(ARBC_LIBARBC_FILE);

  // The registration entry point every content plugin calls, and a polymorphic
  // contract base every kind derives from: both must be exported so a plugin
  // resolves them from the host image (the annotation actually took effect).
  REQUIRE(exports_demangled_containing(core, "arbc::Registry::add"));
  REQUIRE(exports_demangled_containing(core, "typeinfo for arbc::Content"));
}

TEST_CASE("each plugin resolves its core symbols from the single libarbc.so") {
  const std::vector<DynSym> core = read_dynsyms(ARBC_LIBARBC_FILE);

  for (const char* plugin : k_plugins) {
    CAPTURE(plugin);
    const std::vector<DynSym> syms = read_dynsyms(plugin);

    // The core references a plugin carries as UNDEFINED strong dynamic imports:
    // arbc::-namespaced (functions) or "... for arbc::..." (vtable/typeinfo/VTT/
    // guard variable). Under a static libarbc these would instead be LOCAL
    // definitions -- the observable that distinguishes single-instance resolution
    // from a private copy.
    std::vector<std::string> core_imports;
    for (const auto& s : syms) {
      if (!s.defined && s.global && s.demangled.find("arbc::") != std::string::npos) {
        core_imports.push_back(s.mangled);
      }
    }

    // (1) It genuinely imports core symbols -- not a private static copy.
    REQUIRE_FALSE(core_imports.empty());

    // (2) Every such import is exported by the single libarbc.so, so it resolves
    // there at load. This is exactly the condition under which dlopen succeeds, so
    // it also proves (with the re-run dual_build/imageseq/miniaudio loads) that the
    // eight modules load against the shared libarbc -- and it catches any core
    // symbol a plugin references that was left un-ARBC_API-annotated.
    for (const std::string& imported : core_imports) {
      CAPTURE(imported);
      REQUIRE(exports(core, imported));
    }
  }
}
