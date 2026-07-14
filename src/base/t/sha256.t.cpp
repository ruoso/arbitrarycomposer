// The in-tree SHA-256 against the published NIST FIPS 180-4 test vectors
// (serialize.raster_tile_store Decision 3). The vectors are the whole defence of the
// hand-roll: SHA-256 is a fixed public spec with no key and no secret, so a passing
// implementation of it is a CORRECT one -- the hazard the "never hand-roll crypto"
// advice hedges against is a subtly wrong construction that still looks right, and
// that cannot survive an input whose reference output is published.

#include <arbc/base/sha256.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace arbc;

namespace {

std::span<const std::byte> as_bytes(std::string_view s) {
  return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}

std::string hex_of(std::string_view s) { return to_hex(sha256(as_bytes(s))); }

} // namespace

TEST_CASE("sha256 matches the NIST FIPS 180-4 vectors") {
  // The empty message (NIST CAVS SHA256ShortMsg, Len = 0).
  CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // FIPS 180-4 Appendix B.1: the one-block message "abc".
  CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // FIPS 180-4 Appendix B.2: the 448-bit two-block message. Its length lands exactly
  // where the pad must spill into a SECOND block -- the off-by-one the tail code can
  // realistically get wrong.
  CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  // FIPS 180-4 Appendix B.3: 1 000 000 repetitions of 'a' -- 15625 whole blocks, so
  // the multi-block loop and the 64-bit length field are both exercised at scale.
  const std::string million(1000000, 'a');
  CHECK(hex_of(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 pad boundaries around the block size") {
  // 55 bytes: the largest message whose pad byte + 64-bit length still fit one block.
  CHECK(hex_of(std::string(55, 'a')) ==
        "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  // 56 bytes: the pad now spills to a second block.
  CHECK(hex_of(std::string(56, 'a')) ==
        "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  // 64 bytes: an exact block, so the tail is pad-only.
  CHECK(hex_of(std::string(64, 'a')) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}

TEST_CASE("sha256 hex truncation is the blob-name form") {
  const Sha256Digest d = sha256(as_bytes("abc"));
  CHECK(to_hex(d).size() == 64);
  // SHA-256/128: the leading 16 bytes, 32 hex chars -- the tile-blob name.
  CHECK(to_hex(d, 16) == "ba7816bf8f01cfea414140de5dae2223");
  CHECK(to_hex(d, 16).size() == 32);
  // Clamped, never out of bounds.
  CHECK(to_hex(d, 999) == to_hex(d));
  CHECK(to_hex(d, 0).empty());
}

TEST_CASE("sha256 is a pure function of the bytes") {
  const std::vector<std::byte> a{std::byte{0x00}, std::byte{0x01}, std::byte{0x02}};
  const std::vector<std::byte> b{std::byte{0x00}, std::byte{0x01}, std::byte{0x03}};
  CHECK(sha256(a) == sha256(a));
  CHECK_FALSE(sha256(a) == sha256(b));
}
