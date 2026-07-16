#pragma once

#include <arbc/arbc_api.h>

// The tree's content hash (serialize.raster_tile_store Decision 3; doc 00 decision
// record; doc 08 Principle 8, § Dependency note).
//
// Doc 08's content-addressed tile store is "keyed by content hash" and the tree had
// nothing that could serve: `hash_mix.hpp`'s `mix64` is a splitmix64 finalizer whose
// own comment disclaims any cryptographic claim, and there was no sha/blake/xxhash
// anywhere under `src/`. Doc 10's dependency table is a closed list of two, and its
// zstd row closes with "it is worth exactly one small, well-vetted dependency and no
// more" -- so the hash is WRITTEN rather than BOUGHT.
//
// Hand-rolling a primitive is normally a smell, and it is worth being exact about why
// it is safe HERE: SHA-256 is a fixed public spec (FIPS 180-4) of about 150 lines,
// there is no key and no secret so there is no side-channel surface, and correctness
// is COMPLETELY pinned by the published NIST vectors (`t/sha256.t.cpp`). The hazard
// the usual advice hedges against -- a subtly wrong construction that still looks
// right -- cannot survive an input whose reference output is published.
//
// Not constant-time, and it does not need to be: it hashes the user's own pixels.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace arbc {

// A full SHA-256 digest: 32 bytes, big-endian per FIPS 180-4 §6.2.2.
struct Sha256Digest {
  std::array<std::uint8_t, 32> bytes{};

  friend bool operator==(const Sha256Digest&, const Sha256Digest&) = default;
};

// SHA-256 over `data`. One-shot, allocation-free, and stateless -- safe from any
// thread, which is what lets the tile-encode pipeline eventually fan out across the
// pool workers (`serialize.tile_store_parallel_save`).
ARBC_API Sha256Digest sha256(std::span<const std::byte> data);

// Lowercase hex of the whole digest (64 chars).
ARBC_API std::string to_hex(const Sha256Digest& digest);

// Lowercase hex of the digest's leading `n` bytes -- the blob-name form (n == 16 is
// SHA-256/128: a 2^64 birthday bound, set at the generous end because a collision's
// failure mode is silent pixel corruption). `n` is clamped to the digest width.
ARBC_API std::string to_hex(const Sha256Digest& digest, std::size_t n);

} // namespace arbc
