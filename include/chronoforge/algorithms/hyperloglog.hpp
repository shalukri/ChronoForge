#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <bit>
#include <string_view>

#include "chronoforge/algorithms/hashing.hpp"

namespace cf {

// HyperLogLog distinct-count sketch (Flajolet et al. 2007).
//
// Memory: 2^P one-byte registers. P=12 -> 4 KiB, standard error 1.04/sqrt(2^P) = 1.63%.
// An exact set of 10^7 distinct label values would need hundreds of MB; this needs 4 KiB.
//
// WHY it exists in a TSDB: when a client explodes a label (user_id, request_id) the cardinality
// guard REJECTS new series -- so rejected values are never stored anywhere and the exact
// "how bad is it?" number is unrecoverable from the index. HLL counts them in O(1) memory.
//
// Mergeability (register-wise max) is the second reason: per-shard / per-node sketches combine
// into a global distinct count with no raw data movement.
//
// Known limitation: between ~2.5m and ~5m the raw estimator has a small positive bias. HLL++
// fixes it with empirical bias tables; we skip that (a few %) because the guard's decisions use
// exact counts and this sketch is only for reporting.
template <unsigned P = 12>
class HyperLogLog {
  static_assert(P >= 4 && P <= 18, "precision out of supported range");
  static constexpr std::size_t M = std::size_t{1} << P;

 public:
  // `hash` must be well-mixed 64-bit (see mix64/hashBytes). Returns true if a register grew.
  bool add(std::uint64_t hash) noexcept {
    const std::size_t idx = static_cast<std::size_t>(hash >> (64 - P));
    // Shift off the index bits; OR a sentinel 1 into bit (P-1) so countl_zero is bounded by
    // 64-P even if the remaining hash bits are all zero (rank = 64-P+1 is the max).
    const std::uint64_t w = (hash << P) | (std::uint64_t{1} << (P - 1));
    const auto rank = static_cast<std::uint8_t>(std::countl_zero(w) + 1);
    if (rank > reg_[idx]) { reg_[idx] = rank; return true; }
    return false;
  }

  bool addString(std::string_view s) noexcept { return add(hashBytes(s)); }

  std::uint64_t estimate() const noexcept {
    double inv_sum = 0.0;
    std::size_t zeros = 0;
    for (std::uint8_t r : reg_) {
      inv_sum += std::ldexp(1.0, -static_cast<int>(r));
      zeros += (r == 0);
    }
    const double m = static_cast<double>(M);
    const double alpha = (M == 16) ? 0.673 : (M == 32) ? 0.697 : (M == 64) ? 0.709
                                                                         : 0.7213 / (1.0 + 1.079 / m);
    double e = alpha * m * m / inv_sum;
    // Small-range correction: while many registers are still empty, LINEAR COUNTING
    // (m * ln(m / zeros)) is far more accurate than the harmonic-mean estimator.
    if (e <= 2.5 * m && zeros > 0) e = m * std::log(m / static_cast<double>(zeros));
    // No large-range correction: with a 64-bit hash, collisions are irrelevant below ~10^18.
    return static_cast<std::uint64_t>(std::llround(e));
  }

  void merge(const HyperLogLog& other) noexcept {
    for (std::size_t i = 0; i < M; ++i)
      if (other.reg_[i] > reg_[i]) reg_[i] = other.reg_[i];
  }

  static constexpr std::size_t memoryBytes() noexcept { return M; }
  static double standardError() noexcept { return 1.04 / std::sqrt(static_cast<double>(M)); }

 private:
  std::array<std::uint8_t, M> reg_{};
};

}  // namespace cf
