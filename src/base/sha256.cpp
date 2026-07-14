#include <arbc/base/sha256.hpp>

#include <algorithm>
#include <cstring>

namespace arbc {
namespace {

// FIPS 180-4 §4.2.2: the first 32 bits of the fractional parts of the cube roots of
// the first 64 primes.
constexpr std::uint32_t k_round[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n)); // n is in [1, 31] at every call site
}

// FIPS 180-4 §5.3.3: the first 32 bits of the fractional parts of the square roots of
// the first 8 primes.
struct State {
  std::uint32_t h[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
};

// One 64-byte block (FIPS 180-4 §6.2.2).
void compress(State& s, const std::uint8_t* block) {
  std::uint32_t w[64];
  for (int t = 0; t < 16; ++t) {
    w[t] = (static_cast<std::uint32_t>(block[t * 4]) << 24) |
           (static_cast<std::uint32_t>(block[t * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[t * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[t * 4 + 3]);
  }
  for (int t = 16; t < 64; ++t) {
    const std::uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
    const std::uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
    w[t] = w[t - 16] + s0 + w[t - 7] + s1;
  }

  std::uint32_t a = s.h[0];
  std::uint32_t b = s.h[1];
  std::uint32_t c = s.h[2];
  std::uint32_t d = s.h[3];
  std::uint32_t e = s.h[4];
  std::uint32_t f = s.h[5];
  std::uint32_t g = s.h[6];
  std::uint32_t h = s.h[7];

  for (int t = 0; t < 64; ++t) {
    const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t ch = (e & f) ^ (~e & g);
    const std::uint32_t t1 = h + S1 + ch + k_round[t] + w[t];
    const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  s.h[0] += a;
  s.h[1] += b;
  s.h[2] += c;
  s.h[3] += d;
  s.h[4] += e;
  s.h[5] += f;
  s.h[6] += g;
  s.h[7] += h;
}

constexpr char k_hex[] = "0123456789abcdef";

} // namespace

Sha256Digest sha256(std::span<const std::byte> data) {
  State s;

  const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
  const std::size_t n = data.size();

  std::size_t at = 0;
  for (; at + 64 <= n; at += 64) {
    compress(s, p + at);
  }

  // The tail: the remaining <64 bytes, the 0x80 pad byte, zero fill, and the 64-bit
  // big-endian BIT length. Two blocks when the remainder leaves no room for the
  // length field (FIPS 180-4 §5.1.1).
  std::uint8_t tail[128] = {};
  const std::size_t rest = n - at;
  if (rest != 0) {
    std::memcpy(tail, p + at, rest);
  }
  tail[rest] = 0x80U;
  const std::size_t blocks = (rest + 1 + 8 <= 64) ? 1 : 2;
  const std::uint64_t bits = static_cast<std::uint64_t>(n) * 8U;
  const std::size_t len_at = blocks * 64 - 8;
  for (int i = 0; i < 8; ++i) {
    tail[len_at + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((bits >> (56 - i * 8)) & 0xFFU);
  }
  for (std::size_t b = 0; b < blocks; ++b) {
    compress(s, tail + b * 64);
  }

  Sha256Digest out;
  for (int i = 0; i < 8; ++i) {
    const std::size_t o = static_cast<std::size_t>(i) * 4;
    out.bytes[o] = static_cast<std::uint8_t>((s.h[i] >> 24) & 0xFFU);
    out.bytes[o + 1] = static_cast<std::uint8_t>((s.h[i] >> 16) & 0xFFU);
    out.bytes[o + 2] = static_cast<std::uint8_t>((s.h[i] >> 8) & 0xFFU);
    out.bytes[o + 3] = static_cast<std::uint8_t>(s.h[i] & 0xFFU);
  }
  return out;
}

std::string to_hex(const Sha256Digest& digest, std::size_t n) {
  const std::size_t take = std::min(n, digest.bytes.size());
  std::string out;
  out.reserve(take * 2);
  for (std::size_t i = 0; i < take; ++i) {
    const std::uint8_t b = digest.bytes[i];
    out.push_back(k_hex[(b >> 4) & 0x0FU]);
    out.push_back(k_hex[b & 0x0FU]);
  }
  return out;
}

std::string to_hex(const Sha256Digest& digest) { return to_hex(digest, digest.bytes.size()); }

} // namespace arbc
