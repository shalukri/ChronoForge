#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace cf {

// splitmix64 finalizer: a bijection on 64-bit ints with full avalanche (every input bit flips
// ~50% of output bits). HyperLogLog consumes the TOP bits for the register index and the LOW
// bits for the rank, so *every* bit must be well mixed or the estimator is silently biased.
constexpr std::uint64_t mix64(std::uint64_t x) noexcept {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

// FNV-1a over bytes + splitmix finisher.
// TRADE-OFF: FNV-1a is byte-serial (~1 B/cycle) whereas xxh3/wyhash process 16-32 B/cycle.
// Label names/values are short (<= 256 B, typically ~10 B) and hashed only on series *creation*
// or by cached LabelSet::hash(), so simplicity + zero dependencies beats raw throughput here.
// The mix64 finisher repairs FNV's weak high-bit diffusion. Length is folded in so that
// "" / "\0" style prefixes cannot collide trivially.
constexpr std::uint64_t hashBytes(std::string_view s, std::uint64_t seed = 0) noexcept {
  std::uint64_t h = 0xcbf29ce484222325ULL ^ seed;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return mix64(h ^ static_cast<std::uint64_t>(s.size()));
}

}  // namespace cf
