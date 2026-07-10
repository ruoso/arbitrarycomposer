#pragma once
// MSVC-only fallback for 128-bit integer arithmetic used by rational_time.
//
// Provides `rational_u128` and `rational_i128` as structs on compilers that
// lack `__int128` (primarily MSVC/cl.exe on Windows x64). Only the operations
// required by rational_time.hpp/.cpp are implemented.
//
// On GCC and Clang, rational_time.hpp selects the native `__int128` types.

#include <cstdint>
#include <utility>

namespace arbc {

// High 64 bits of the product of two unsigned 64-bit integers.
// Uses the 32-bit split trick to avoid MSVC-specific intrinsics.
inline std::uint64_t arbc_mul64_high(std::uint64_t a, std::uint64_t b) noexcept {
  const std::uint64_t a0 = a & 0xFFFF'FFFFu, a1 = a >> 32u;
  const std::uint64_t b0 = b & 0xFFFF'FFFFu, b1 = b >> 32u;
  const std::uint64_t p1 = a0 * b0;
  const std::uint64_t p2 = a0 * b1;
  const std::uint64_t p3 = a1 * b0;
  const std::uint64_t p4 = a1 * b1;
  const std::uint64_t mid = (p1 >> 32u) + (p2 & 0xFFFF'FFFFu) + (p3 & 0xFFFF'FFFFu);
  return p4 + (p2 >> 32u) + (p3 >> 32u) + (mid >> 32u);
}

// ── Unsigned 128-bit integer ─────────────────────────────────────────────────

struct rational_u128 {
  std::uint64_t lo = 0;
  std::uint64_t hi = 0;

  constexpr rational_u128() noexcept = default;
  // Non-explicit so `b != 0` works via implicit int→uint64 conversion.
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr rational_u128(std::uint64_t v) noexcept : lo(v), hi(0) {}
  // For `static_cast<u128>(INT64_MAX)` — always non-negative so hi stays 0
  explicit constexpr rational_u128(std::int64_t v) noexcept
      : lo(static_cast<std::uint64_t>(v)), hi(0) {}

  // ── Comparison ────────────────────────────────────────────────────────────
  constexpr bool operator==(rational_u128 o) const noexcept {
    return lo == o.lo && hi == o.hi;
  }
  constexpr bool operator!=(rational_u128 o) const noexcept {
    return !(*this == o);
  }
  constexpr bool operator<(rational_u128 o) const noexcept {
    return hi < o.hi || (hi == o.hi && lo < o.lo);
  }
  constexpr bool operator<=(rational_u128 o) const noexcept {
    return !(o < *this);
  }
  constexpr bool operator>(rational_u128 o) const noexcept { return o < *this; }

  // ── Arithmetic ────────────────────────────────────────────────────────────
  constexpr rational_u128 operator+(rational_u128 o) const noexcept {
    const std::uint64_t r_lo = lo + o.lo;
    const std::uint64_t carry = (r_lo < lo) ? std::uint64_t{1} : std::uint64_t{0};
    rational_u128 r;
    r.lo = r_lo;
    r.hi = hi + o.hi + carry;
    return r;
  }
  constexpr rational_u128 operator-(rational_u128 o) const noexcept {
    const std::uint64_t r_lo = lo - o.lo;
    const std::uint64_t borrow = (r_lo > lo) ? std::uint64_t{1} : std::uint64_t{0};
    rational_u128 r;
    r.lo = r_lo;
    r.hi = hi - o.hi - borrow;
    return r;
  }
  constexpr rational_u128 operator-() const noexcept {
    // Two's complement negation: ~x + 1
    rational_u128 r;
    r.lo = ~lo + std::uint64_t{1};
    r.hi = ~hi + (lo == 0u ? std::uint64_t{1} : std::uint64_t{0});
    return r;
  }
  constexpr rational_u128& operator/=(rational_u128 b) noexcept {
    *this = divmod(*this, b).first;
    return *this;
  }

  constexpr rational_u128 operator<<(unsigned shift) const noexcept {
    rational_u128 r;
    if (shift == 0u) return *this;
    if (shift < 64u) {
      r.lo = lo << shift;
      r.hi = (hi << shift) | (lo >> (64u - shift));
    } else if (shift < 128u) {
      r.lo = 0u;
      r.hi = lo << (shift - 64u);
    }
    return r;
  }
  constexpr rational_u128 operator>>(unsigned shift) const noexcept {
    rational_u128 r;
    if (shift == 0u) return *this;
    if (shift < 64u) {
      r.lo = (lo >> shift) | (hi << (64u - shift));
      r.hi = hi >> shift;
    } else if (shift < 128u) {
      r.lo = hi >> (shift - 64u);
      r.hi = 0u;
    }
    return r;
  }

  // Binary long division: returns {quotient, remainder}.
  static constexpr std::pair<rational_u128, rational_u128> divmod(
      rational_u128 a, rational_u128 b) noexcept {
    if (b.hi == 0u && b.lo == 0u) {
      return {}; // undefined — avoid crash
    }
    if (a < b) {
      return {rational_u128{}, a};
    }
    rational_u128 q{}, r{};
    for (int i = 127; i >= 0; --i) {
      r = r << 1u;
      // Insert bit i of `a` into r's LSB
      const auto ui = static_cast<unsigned>(i);
      const std::uint64_t src_word = (ui < 64u) ? a.lo : a.hi;
      const std::uint64_t bit = src_word >> (ui % 64u) & std::uint64_t{1};
      r.lo |= bit;
      if (!(r < b)) {
        r = r - b;
        if (ui < 64u) {
          q.lo |= std::uint64_t{1} << ui;
        } else {
          q.hi |= std::uint64_t{1} << (ui - 64u);
        }
      }
    }
    return {q, r};
  }

  constexpr rational_u128 operator/(rational_u128 b) const noexcept {
    return divmod(*this, b).first;
  }
  constexpr rational_u128 operator%(rational_u128 b) const noexcept {
    return divmod(*this, b).second;
  }

  // Cast to int64 (caller ensures value fits; used after GCD reduction)
  explicit constexpr operator std::int64_t() const noexcept {
    return static_cast<std::int64_t>(lo);
  }
};

// ── Signed 128-bit integer ───────────────────────────────────────────────────

struct rational_i128 {
  std::uint64_t lo = 0;
  std::int64_t hi = 0; // sign-extended high word

  constexpr rational_i128() noexcept = default;

  // Implicit conversion from int64 (models `const i128 x = int64_value`)
  // NOLINTNEXTLINE(google-explicit-constructor)
  constexpr rational_i128(std::int64_t v) noexcept
      : lo(static_cast<std::uint64_t>(v)),
        hi(v < 0 ? std::int64_t{-1} : std::int64_t{0}) {}

  // Bit-reinterpret from u128 (used internally after unsigned arithmetic)
  explicit constexpr rational_i128(rational_u128 v) noexcept
      : lo(v.lo), hi(static_cast<std::int64_t>(v.hi)) {}

  bool is_negative() const noexcept { return hi < 0; }

  // Explicit cast to u128 (bit reinterpretation; used by magnitude_i128)
  explicit operator rational_u128() const noexcept {
    rational_u128 r;
    r.lo = lo;
    r.hi = static_cast<std::uint64_t>(hi);
    return r;
  }

  // Absolute value as u128 (two's complement negation for negative inputs)
  rational_u128 magnitude() const noexcept {
    rational_u128 bits;
    bits.lo = lo;
    bits.hi = static_cast<std::uint64_t>(hi);
    return is_negative() ? -bits : bits;
  }

  // ── Comparison ────────────────────────────────────────────────────────────
  constexpr bool operator==(rational_i128 o) const noexcept {
    return lo == o.lo && hi == o.hi;
  }
  constexpr bool operator!=(rational_i128 o) const noexcept {
    return !(*this == o);
  }
  constexpr bool operator!=(int v) const noexcept {
    return *this != rational_i128(static_cast<std::int64_t>(v));
  }
  constexpr bool operator<(rational_i128 o) const noexcept {
    // Signed: compare hi first (signed), then lo (unsigned)
    return hi < o.hi || (hi == o.hi && lo < o.lo);
  }
  constexpr bool operator<(int v) const noexcept {
    return *this < rational_i128(static_cast<std::int64_t>(v));
  }
  constexpr bool operator>(rational_i128 o) const noexcept { return o < *this; }
  constexpr bool operator>=(rational_i128 o) const noexcept {
    return !(*this < o);
  }

  // ── Arithmetic ────────────────────────────────────────────────────────────
  constexpr rational_i128 operator-() const noexcept {
    const std::uint64_t r_lo = ~lo + std::uint64_t{1};
    const std::int64_t r_hi = static_cast<std::int64_t>(
        ~static_cast<std::uint64_t>(hi) + (lo == 0u ? std::uint64_t{1} : std::uint64_t{0}));
    rational_i128 r;
    r.lo = r_lo;
    r.hi = r_hi;
    return r;
  }
  constexpr rational_i128 operator+(rational_i128 o) const noexcept {
    const std::uint64_t r_lo = lo + o.lo;
    const std::uint64_t carry = (r_lo < lo) ? std::uint64_t{1} : std::uint64_t{0};
    rational_i128 r;
    r.lo = r_lo;
    r.hi = static_cast<std::int64_t>(
        static_cast<std::uint64_t>(hi) + static_cast<std::uint64_t>(o.hi) + carry);
    return r;
  }
  constexpr rational_i128 operator-(rational_i128 o) const noexcept {
    return *this + (-o);
  }
  constexpr rational_i128& operator+=(rational_i128 o) noexcept {
    *this = *this + o;
    return *this;
  }
  constexpr rational_i128& operator-=(std::int64_t v) noexcept {
    *this = *this - rational_i128(v);
    return *this;
  }

  // Signed multiply — both operands always originate from int64 casts in
  // practice; the 128-bit result is the low 128 bits of the signed product.
  rational_i128 operator*(rational_i128 o) const noexcept {
    const bool neg = is_negative() != o.is_negative();
    const rational_u128 aa = magnitude();
    const rational_u128 bb = o.magnitude();
    // 128-bit unsigned product (keep only low 128 bits):
    //   aa * bb = aa.hi * bb.hi * 2^128   (discarded)
    //           + (aa.hi * bb.lo + aa.lo * bb.hi) * 2^64
    //           + aa.lo * bb.lo
    const std::uint64_t r_lo = aa.lo * bb.lo;
    const std::uint64_t r_hi = arbc_mul64_high(aa.lo, bb.lo) +
                               aa.hi * bb.lo + aa.lo * bb.hi;
    rational_u128 mag;
    mag.lo = r_lo;
    mag.hi = r_hi;
    if (neg) mag = -mag;
    return rational_i128(mag);
  }

  // Truncated division (toward zero) and modulo, via unsigned divmod + sign.
  rational_i128 operator/(rational_i128 o) const noexcept {
    const bool neg = is_negative() != o.is_negative();
    const auto [q, rem] = rational_u128::divmod(magnitude(), o.magnitude());
    (void)rem;
    const rational_i128 r(q);
    return neg ? -r : r;
  }
  rational_i128 operator%(rational_i128 o) const noexcept {
    const bool neg_num = is_negative();
    const auto [quot, rem] = rational_u128::divmod(magnitude(), o.magnitude());
    (void)quot;
    const rational_i128 r(rem);
    return neg_num ? -r : r;
  }

  // Bitwise AND with a small mask (used for even/odd check: `q & 1`)
  constexpr std::int64_t operator&(std::int64_t mask) const noexcept {
    return static_cast<std::int64_t>(lo) & mask;
  }

  // Cast to int64 (caller ensures value fits after final bounds check)
  explicit constexpr operator std::int64_t() const noexcept {
    return static_cast<std::int64_t>(lo);
  }
};

// Free operator* so `2 * r` (int literal on the left) compiles.
inline rational_i128 operator*(std::int64_t lhs, rational_i128 rhs) noexcept {
  return rational_i128(lhs) * rhs;
}

// ── Portable overflow-checked arithmetic ─────────────────────────────────────
//
// Used by rational_time.cpp in place of `__builtin_mul_overflow` /
// `__builtin_add_overflow`, which are GCC/Clang extensions unavailable on MSVC.

// Returns true iff a * b overflows signed 128-bit; writes the truncated product
// to `result` only when there is no overflow.
inline bool arbc_mul_overflow_i128(rational_i128 a, rational_i128 b,
                                   rational_i128& result) noexcept {
  const bool neg = a.is_negative() != b.is_negative();
  const rational_u128 aa = a.magnitude();
  const rational_u128 bb = b.magnitude();

  // Compute the full 256-bit unsigned magnitude.
  // aa = (a1 << 64) | a0,  bb = (b1 << 64) | b0
  const std::uint64_t a0 = aa.lo, a1 = aa.hi;
  const std::uint64_t b0 = bb.lo, b1 = bb.hi;

  // 128-bit low result (same arithmetic as operator*)
  const std::uint64_t r0 = a0 * b0;
  const std::uint64_t r0_h = arbc_mul64_high(a0, b0);

  // Accumulate the four partial products for bits 64..127 and carry into 128+
  const std::uint64_t cross_lo = a1 * b0;
  const std::uint64_t cross_hi_0 = a0 * b1;
  // Sum: r0_h + cross_lo + cross_hi_0 (with carry tracking)
  std::uint64_t carry = 0;
  std::uint64_t mid = r0_h;
  if (mid + cross_lo < mid) ++carry;
  mid += cross_lo;
  if (mid + cross_hi_0 < mid) ++carry;
  mid += cross_hi_0;

  // Bits 128+ (if non-zero, the product overflows i128)
  const std::uint64_t hi_overflow = a1 * b1 +
                                    arbc_mul64_high(a1, b0) +
                                    arbc_mul64_high(a0, b1) + carry;

  if (hi_overflow != 0u) return true; // product >= 2^128 -> overflow

  // Check whether the 128-bit magnitude fits in a signed 128-bit value:
  //   positive:  mag <= INT128_MAX  = 2^127 - 1  (high bit of mag must be 0)
  //   negative:  mag <= 2^127       (INT128_MIN = -2^127 is representable)
  const bool high_bit_set = (mid >> 63u) != 0u;
  if (!neg && high_bit_set) return true;
  if (neg && high_bit_set) {
    // -2^127 is the only negative value with high_bit set that's representable
    if (mid != (std::uint64_t{1} << 63u) || r0 != 0u) return true;
  }

  rational_u128 mag;
  mag.lo = r0;
  mag.hi = mid;
  if (neg) mag = -mag;
  result = rational_i128(mag);
  return false;
}

// Returns true iff a + b overflows signed 128-bit; always writes the (possibly
// wrapped) sum to `result`.
inline bool arbc_add_overflow_i128(rational_i128 a, rational_i128 b,
                                   rational_i128& result) noexcept {
  result = a + b;
  // Signed overflow: both operands same sign, result opposite sign.
  const bool a_neg = a.is_negative();
  const bool b_neg = b.is_negative();
  const bool r_neg = result.is_negative();
  return (a_neg == b_neg) && (r_neg != a_neg);
}

} // namespace arbc
