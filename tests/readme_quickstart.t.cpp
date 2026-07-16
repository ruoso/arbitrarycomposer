// tests/readme_quickstart.t.cpp -- README quickstart <-> shipped example
// sync (packaging.release_01).
//
// Doc 16:88-90 (test tier 10): every code sample in docs compiles and runs
// in CI -- "a README example that doesn't build is a bug." The README
// quickstart's code blocks are lifted byte-identically from
// examples/host-offline/, a standalone foreign project the install.consumer
// CTest configures, builds, and RUNS against a staged install prefix on
// every lane (16-sdlc-and-quality#shipped-examples-compile-and-run-in-ci).
// Byte-identity, checked here, transfers that proof to the README: the
// quickstart cannot drift from a building, running program.
//
// Mechanics (release_01 Decision D3): an HTML marker comment in README.md
// names each quickstart block (`<!-- readme-quickstart: <id> -->`, the
// fenced code block immediately after it), and the block must equal, byte
// for byte, the region between the comment anchors `[readme-quickstart:<id>]`
// and `[/readme-quickstart:<id>]` in the example source. Paths arrive as
// compile definitions, per the fixture-dir convention
// (tests/fuzz_corpus_replay.t.cpp). Self-contained byte work -- links no
// arbc component.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef ARBC_README_FILE
#error "ARBC_README_FILE must be defined (the repository's README.md)"
#endif
#ifndef ARBC_HOST_OFFLINE_SRC_DIR
#error "ARBC_HOST_OFFLINE_SRC_DIR must be defined (examples/host-offline)"
#endif

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::ifstream in{path, std::ios::binary};
  REQUIRE(in.is_open());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

std::string trimmed(const std::string& line) {
  const std::size_t first = line.find_first_not_of(" \t");
  const std::size_t last = line.find_last_not_of(" \t");
  return first == std::string::npos ? std::string{} : line.substr(first, last - first + 1);
}

// Index of the first line at or after `from` whose whitespace-trimmed text
// equals `needle`; lines.size() when absent.
std::size_t find_trimmed(const std::vector<std::string>& lines, const std::string& needle,
                         std::size_t from) {
  std::size_t i = from;
  while (i < lines.size() && trimmed(lines[i]) != needle) {
    ++i;
  }
  return i;
}

// The README's fenced code block introduced by `<!-- readme-quickstart: <id> -->`,
// joined with '\n' (one trailing '\n'). The marker must occur exactly once
// and sit directly above the opening fence.
std::string readme_block(const std::vector<std::string>& lines, const std::string& id) {
  const std::string marker = "<!-- readme-quickstart: " + id + " -->";
  const std::size_t open = find_trimmed(lines, marker, 0);
  INFO("README marker: " << marker);
  REQUIRE(open < lines.size());
  REQUIRE(find_trimmed(lines, marker, open + 1) == lines.size());
  REQUIRE(open + 1 < lines.size());
  REQUIRE(lines[open + 1].rfind("```", 0) == 0);
  std::string block;
  std::size_t i = open + 2;
  while (i < lines.size() && lines[i] != "```") {
    block += lines[i];
    block += '\n';
    ++i;
  }
  REQUIRE(i < lines.size());
  return block;
}

// The example-source region between `<comment> [readme-quickstart:<id>]` and
// `<comment> [/readme-quickstart:<id>]` anchor lines (exclusive), joined with
// '\n' (one trailing '\n'). The opening anchor must occur exactly once.
std::string anchored_region(const std::filesystem::path& path, const std::string& comment,
                            const std::string& id) {
  const std::vector<std::string> lines = read_lines(path);
  const std::string open_anchor = comment + " [readme-quickstart:" + id + "]";
  const std::string close_anchor = comment + " [/readme-quickstart:" + id + "]";
  const std::size_t open = find_trimmed(lines, open_anchor, 0);
  INFO("anchor " << open_anchor << " in " << path);
  REQUIRE(open < lines.size());
  REQUIRE(find_trimmed(lines, open_anchor, open + 1) == lines.size());
  std::string region;
  std::size_t i = open + 1;
  while (i < lines.size() && trimmed(lines[i]) != close_anchor) {
    region += lines[i];
    region += '\n';
    ++i;
  }
  REQUIRE(i < lines.size());
  return region;
}

} // namespace

// enforces: 16-sdlc-and-quality#readme-quickstart-is-the-shipped-example
TEST_CASE("the README consume recipe is the shipped example's CMakeLists, byte for byte") {
  const std::vector<std::string> readme = read_lines(ARBC_README_FILE);
  const std::filesystem::path example{ARBC_HOST_OFFLINE_SRC_DIR};
  CHECK(readme_block(readme, "consume") ==
        anchored_region(example / "CMakeLists.txt", "#", "consume"));
}

TEST_CASE("the README embedding snippet is the shipped example's main(), byte for byte") {
  const std::vector<std::string> readme = read_lines(ARBC_README_FILE);
  const std::filesystem::path example{ARBC_HOST_OFFLINE_SRC_DIR};
  CHECK(readme_block(readme, "embed") == anchored_region(example / "main.cpp", "//", "embed"));
}
