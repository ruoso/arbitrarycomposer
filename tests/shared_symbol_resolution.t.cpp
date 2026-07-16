// The single-instance symbol-resolution proof (packaging.shared_library_build,
// doc 17:173-182; packaging.shared_library_build_msvc, doc 17:188-192). Built ONLY
// under BUILD_SHARED_LIBS (wired behind an if(BUILD_SHARED_LIBS) in
// tests/CMakeLists.txt), so the assertions below are never vacuous in the static
// lanes -- there a plugin carries a PRIVATE static copy of every core object it
// references and the property this test pins does not hold.
//
// enforces: 17-internal-components#plugin-resolves-core-symbols-from-host-image
//
// The difference between "a plugin resolves core symbols FROM THE HOST IMAGE" and
// "a plugin carries a private static copy" is a LINKAGE property the existing
// dual_build render/facet/service assertions cannot see -- they pass with two
// copies as happily as with one. Under the shared build a plugin's core references
// are imports satisfied at load by the single libarbc; under the static build they
// are local definitions. A symbol-table scan of the built artifacts (paths passed
// as compile definitions, the containment-test shape of miniaudio_containment.t.cpp)
// sees it directly, is deterministic, and needs no subprocess.
//
// The proof is platform-forked in ONE file (Decision D2), mirroring
// plugin_host.cpp's single-#if-defined(_WIN32) seam: the TEST_CASE structure, the
// core-symbol witnesses, and the "each plugin imports core symbols from the single
// libarbc, which exports them" assertions are shared; only the reader differs.
//   - ELF (gcc/clang shared lane): walks the .dynsym -- exported symbols are
//     defined/global/default-visibility entries; a plugin's core imports are its
//     UNDEFINED global arbc:: entries, resolved by the loader from libarbc.so.
//   - PE/COFF (MSVC shared lane): reads arbc.dll's EXPORT directory and each plugin
//     .dll's IMPORT directory -- a plugin's core imports are the by-name entries
//     under the "arbc.dll" import descriptor, the Windows observable that names the
//     single host image the symbol resolves from (Constraint 3). This is where
//     ARBC_API's __declspec(dllexport)/(dllimport) asymmetry (unlike ELF's
//     symmetric visibility("default")) is proven correct across the LoadLibrary
//     boundary.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// libarbc's exported public surface, and a plugin's core imports resolved from it,
// reduced to what the proof needs. Both readers populate these; the TEST_CASE
// bodies below are platform-agnostic.
struct Exports {
  std::vector<std::string> mangled;   // linker-identity export names
  std::vector<std::string> demangled; // human-readable (Itanium- or MSVC-undecorated)
};
struct Imports {
  std::vector<std::string> mangled;
  std::vector<std::string> demangled;
};

bool any_contains(const std::vector<std::string>& names, const std::string& needle) {
  for (const auto& n : names) {
    if (n.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool contains_exact(const std::vector<std::string>& names, const std::string& name) {
  for (const auto& n : names) {
    if (n == name) {
      return true;
    }
  }
  return false;
}

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

// The MSVC-decorated name rendered to the same human-readable form the ELF branch
// gets from abi::__cxa_demangle, so the core-symbol witnesses match the same
// substrings on both platforms.
std::string undecorate(const std::string& name) {
  char buf[4096];
  const DWORD n =
      UnDecorateSymbolName(name.c_str(), buf, static_cast<DWORD>(sizeof(buf)), UNDNAME_COMPLETE);
  if (n > 0) {
    return std::string(buf, buf + n);
  }
  return name;
}

// A parsed PE image plus the two data directories the proof reads. The artifacts
// are all x64 PE32+ on the msvc-shared lane; a non-PE32+ input fails the REQUIREs
// rather than reading garbage (the mirror of the ELF branch's ELFCLASS64 guard).
struct PeImage {
  std::string blob;
  std::vector<IMAGE_SECTION_HEADER> sections;
  IMAGE_DATA_DIRECTORY export_dir{};
  IMAGE_DATA_DIRECTORY import_dir{};

  const char* base() const { return blob.data(); }

  // Map a relative virtual address to a file offset via the section headers;
  // blob.size() is the "unmapped" sentinel every read below bounds-checks.
  std::size_t rva_to_offset(std::uint32_t rva) const {
    for (const auto& s : sections) {
      const std::uint32_t va = s.VirtualAddress;
      std::uint32_t size = s.SizeOfRawData;
      if (s.Misc.VirtualSize > size) {
        size = s.Misc.VirtualSize;
      }
      if (rva >= va && rva < va + size) {
        return static_cast<std::size_t>(rva - va) + s.PointerToRawData;
      }
    }
    return blob.size();
  }
};

PeImage read_pe(const char* path) {
  PeImage img;
  img.blob = read_file(path);
  const std::string& blob = img.blob;
  const char* base = blob.data();
  REQUIRE(blob.size() >= sizeof(IMAGE_DOS_HEADER));

  IMAGE_DOS_HEADER dos;
  std::memcpy(&dos, base, sizeof(dos));
  REQUIRE(dos.e_magic == IMAGE_DOS_SIGNATURE);

  const std::size_t nt_off = static_cast<std::size_t>(dos.e_lfanew);
  REQUIRE(nt_off + sizeof(IMAGE_NT_HEADERS64) <= blob.size());
  IMAGE_NT_HEADERS64 nt;
  std::memcpy(&nt, base + nt_off, sizeof(nt));
  REQUIRE(nt.Signature == IMAGE_NT_SIGNATURE);
  REQUIRE(nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);

  img.export_dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  img.import_dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

  const std::size_t sec_off =
      nt_off + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
  const std::size_t sec_count = nt.FileHeader.NumberOfSections;
  REQUIRE(sec_off + sec_count * sizeof(IMAGE_SECTION_HEADER) <= blob.size());
  img.sections.resize(sec_count);
  for (std::size_t i = 0; i < sec_count; ++i) {
    std::memcpy(&img.sections[i], base + sec_off + i * sizeof(IMAGE_SECTION_HEADER),
                sizeof(IMAGE_SECTION_HEADER));
  }
  return img;
}

// A NUL-terminated string living at a file offset, bounded by the blob.
std::string read_cstr(const std::string& blob, std::size_t off) {
  REQUIRE(off < blob.size());
  const std::size_t end = blob.find('\0', off);
  REQUIRE(end != std::string::npos);
  return blob.substr(off, end - off);
}

std::uint32_t read_u32(const std::string& blob, std::size_t off) {
  REQUIRE(off + sizeof(std::uint32_t) <= blob.size());
  std::uint32_t value = 0;
  std::memcpy(&value, blob.data() + off, sizeof(value));
  return value;
}

Exports read_host_exports(const char* path) {
  const PeImage img = read_pe(path);
  Exports out;
  REQUIRE(img.export_dir.VirtualAddress != 0); // a DLL with a public surface has one
  const std::size_t dir_off = img.rva_to_offset(img.export_dir.VirtualAddress);
  REQUIRE(dir_off + sizeof(IMAGE_EXPORT_DIRECTORY) <= img.blob.size());

  IMAGE_EXPORT_DIRECTORY ed;
  std::memcpy(&ed, img.base() + dir_off, sizeof(ed));
  const std::size_t names_off = img.rva_to_offset(ed.AddressOfNames);
  for (std::size_t i = 0; i < ed.NumberOfNames; ++i) {
    const std::uint32_t name_rva = read_u32(img.blob, names_off + i * sizeof(std::uint32_t));
    std::string mangled = read_cstr(img.blob, img.rva_to_offset(name_rva));
    out.demangled.push_back(undecorate(mangled));
    out.mangled.push_back(std::move(mangled));
  }
  return out;
}

Imports read_core_imports(const char* path) {
  const PeImage img = read_pe(path);
  Imports out;
  if (img.import_dir.VirtualAddress == 0) {
    return out; // no import directory at all -- a fully static image (caught by REQUIRE_FALSE)
  }

  std::size_t desc_off = img.rva_to_offset(img.import_dir.VirtualAddress);
  for (;; desc_off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    REQUIRE(desc_off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= img.blob.size());
    IMAGE_IMPORT_DESCRIPTOR desc;
    std::memcpy(&desc, img.base() + desc_off, sizeof(desc));
    if (desc.Name == 0 && desc.FirstThunk == 0) {
      break; // the all-zero terminator entry
    }

    std::string dll = read_cstr(img.blob, img.rva_to_offset(desc.Name));
    for (char& c : dll) {
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (dll != "arbc.dll") {
      continue; // imports from the CRT / kernel32 / etc. are not the single-instance question
    }

    // Prefer the import-name table (OriginalFirstThunk); it survives binding.
    const std::uint32_t ilt_rva =
        desc.OriginalFirstThunk != 0 ? desc.OriginalFirstThunk : desc.FirstThunk;
    std::size_t thunk_off = img.rva_to_offset(ilt_rva);
    for (;; thunk_off += sizeof(IMAGE_THUNK_DATA64)) {
      REQUIRE(thunk_off + sizeof(IMAGE_THUNK_DATA64) <= img.blob.size());
      IMAGE_THUNK_DATA64 thunk;
      std::memcpy(&thunk, img.base() + thunk_off, sizeof(thunk));
      if (thunk.u1.AddressOfData == 0) {
        break; // the null thunk terminating this DLL's imports
      }
      if ((thunk.u1.Ordinal & IMAGE_ORDINAL_FLAG64) != 0) {
        continue; // imported by ordinal; libarbc exports by name, so nothing to match here
      }
      // AddressOfData -> IMAGE_IMPORT_BY_NAME { WORD Hint; CHAR Name[]; }.
      const std::size_t ibn_off =
          img.rva_to_offset(static_cast<std::uint32_t>(thunk.u1.AddressOfData));
      std::string mangled = read_cstr(img.blob, ibn_off + sizeof(WORD));
      out.demangled.push_back(undecorate(mangled));
      out.mangled.push_back(std::move(mangled));
    }
  }
  return out;
}

bool exports_content_rtti(const Exports& host) {
  // Class-level __declspec(dllexport) on the polymorphic arbc::Content exports its
  // vftable (??_7Content@arbc@@6B@); a member-wise export would emit its methods but
  // omit the vftable, so dynamic_cast / virtual dispatch across the DLL boundary
  // would break at load/call time, not link time. Matching the vftable export is the
  // MSVC witness that Content's vtable/RTTI crosses the boundary -- the analog of the
  // ELF "typeinfo for arbc::Content" export (the Itanium spelling has no PE
  // named-export analog, Acceptance criteria).
  const std::string vftable_prefix = "??_7Content@arbc@@";
  for (const auto& m : host.mangled) {
    if (m.rfind(vftable_prefix, 0) == 0) {
      return true;
    }
  }
  return false;
}

#else

#include <cxxabi.h>
#include <elf.h>

// One dynamic-symbol-table entry, reduced to what the proof needs.
struct DynSym {
  std::string mangled;   // raw .dynsym name (the linker's identity)
  std::string demangled; // Itanium-demangled, or == mangled if not a C++ symbol
  bool defined = false;  // st_shndx != SHN_UNDEF
  bool global = false;   // STB_GLOBAL binding (a strong ref/def)
  bool weak = false;     // STB_WEAK binding
  bool default_vis = false;
};

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
// artifacts under test are all ELFCLASS64 on the gcc/clang shared lane; a non-ELF64
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
  REQUIRE(ehdr.e_shoff + static_cast<std::uint64_t>(ehdr.e_shnum) * ehdr.e_shentsize <=
          blob.size());

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

Exports read_host_exports(const char* path) {
  const std::vector<DynSym> syms = read_dynsyms(path);
  Exports out;
  for (const auto& s : syms) {
    // Defined, global/weak, default-visibility: the .dynsym shape of an export.
    if (s.defined && (s.global || s.weak) && s.default_vis) {
      out.mangled.push_back(s.mangled);
      out.demangled.push_back(s.demangled);
    }
  }
  return out;
}

Imports read_core_imports(const char* path) {
  const std::vector<DynSym> syms = read_dynsyms(path);
  Imports out;
  for (const auto& s : syms) {
    // The core references a plugin carries as UNDEFINED strong dynamic imports:
    // arbc::-namespaced (functions) or "... for arbc::..." (vtable/typeinfo/VTT/
    // guard variable). Under a static libarbc these would instead be LOCAL
    // definitions -- the observable that distinguishes single-instance resolution
    // from a private copy.
    if (!s.defined && s.global && s.demangled.find("arbc::") != std::string::npos) {
      out.mangled.push_back(s.mangled);
      out.demangled.push_back(s.demangled);
    }
  }
  return out;
}

bool exports_content_rtti(const Exports& host) {
  return any_contains(host.demangled, "typeinfo for arbc::Content");
}

#endif

const char* const k_plugins[] = {
    ARBC_CI_PLUGIN_SOLID_FILE,  ARBC_CI_PLUGIN_TONE_FILE,   ARBC_CI_PLUGIN_RASTER_FILE,
    ARBC_CI_PLUGIN_NESTED_FILE, ARBC_CI_PLUGIN_FADE_FILE,   ARBC_CI_PLUGIN_CROSSFADE_FILE,
    ARBC_IMAGESEQ_PLUGIN_FILE,  ARBC_MINIAUDIO_PLUGIN_FILE,
};

} // namespace

TEST_CASE("libarbc exports its deliberate ARBC_API public surface") {
  const Exports host = read_host_exports(ARBC_LIBARBC_FILE);

  // The registration entry point every content plugin calls, and a polymorphic
  // contract base every kind derives from: both must be exported so a plugin
  // resolves them from the host image (the annotation actually took effect). The
  // Content witness is the vtable/RTTI-carrying class-level export -- see
  // exports_content_rtti for how each toolchain spells it.
  REQUIRE(any_contains(host.demangled, "arbc::Registry::add"));
  REQUIRE(exports_content_rtti(host));
}

TEST_CASE("each plugin resolves its core symbols from the single libarbc") {
  const Exports host = read_host_exports(ARBC_LIBARBC_FILE);

  for (const char* plugin : k_plugins) {
    CAPTURE(plugin);
    const Imports imports = read_core_imports(plugin);

    // (1) It genuinely imports core symbols from libarbc -- not a private static copy.
    REQUIRE_FALSE(imports.mangled.empty());

    // (2) Every such import is exported by the single libarbc, so it resolves there at
    // load. This is exactly the condition under which dlopen / LoadLibrary succeeds, so
    // it also proves (with the re-run dual_build/imageseq/miniaudio loads) that the
    // eight modules load against the shared libarbc -- and it catches any core symbol a
    // plugin references that was left un-ARBC_API-annotated.
    for (const std::string& imported : imports.mangled) {
      CAPTURE(imported);
      REQUIRE(contains_exact(host.mangled, imported));
    }
  }
}
