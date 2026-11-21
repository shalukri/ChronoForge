#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cf {

// MSB-first bit packer. Gorilla's control codes are 1-4 bits and its payloads are 5-64 bits,
// so byte-granular APIs would waste the entire point of the codec.
class BitWriter {
 public:
  void writeBit(bool bit) { writeBits(bit ? 1u : 0u, 1); }

  // Writes the low `nbits` of `value` (nbits in [0,64]), most-significant bit first.
  void writeBits(std::uint64_t value, unsigned nbits) {
    assert(nbits <= 64);
    while (nbits > 0) {
      if (used_ == 8) {  // current byte is full (or buffer empty): open a new one
        buf_.push_back(0);
        used_ = 0;
      }
      const unsigned room = 8 - used_;
      const unsigned take = std::min(room, nbits);
      const unsigned shift = nbits - take;  // bits still to emit after this slice
      const auto slice = static_cast<std::uint8_t>((value >> shift) & ((1u << take) - 1u));
      buf_.back() |= static_cast<std::uint8_t>(slice << (room - take));
      used_ += take;
      nbits -= take;
    }
    // PERF NOTE: a 64-bit accumulator that flushes whole words would be ~3x faster; the
    // byte-slice loop is kept because it is branch-simple, endian-agnostic and obviously correct.
  }

  std::size_t bitSize() const noexcept { return buf_.empty() ? 0 : (buf_.size() - 1) * 8 + used_; }
  const std::vector<std::uint8_t>& bytes() const noexcept { return buf_; }
  void shrinkToFit() { buf_.shrink_to_fit(); }  // sealed chunks must not carry vector growth slack

 private:
  std::vector<std::uint8_t> buf_;
  unsigned used_ = 8;  // bits consumed in buf_.back(); 8 == "full", forces a push on next write
};

// Bounds-checked reader. Every read returns false on overrun instead of reading out-of-bounds,
// because chunk bytes may eventually come from disk/network where truncation is a fact of life.
class BitReader {
 public:
  BitReader(const std::uint8_t* data, std::size_t bit_len) noexcept : data_(data), bit_len_(bit_len) {}

  bool readBits(unsigned nbits, std::uint64_t& out) noexcept {
    if (nbits > 64 || pos_ + nbits > bit_len_) return false;
    std::uint64_t v = 0;
    while (nbits > 0) {
      const unsigned off = pos_ & 7u;
      const unsigned avail = 8 - off;
      const unsigned take = std::min(avail, nbits);
      const auto byte = data_[pos_ >> 3];
      const auto slice = static_cast<std::uint8_t>((byte >> (avail - take)) & ((1u << take) - 1u));
      v = (v << take) | slice;
      pos_ += take;
      nbits -= take;
    }
    out = v;
    return true;
  }

  bool readBit(bool& out) noexcept {
    std::uint64_t b;
    if (!readBits(1, b)) return false;
    out = (b != 0);
    return true;
  }

  std::size_t position() const noexcept { return pos_; }

 private:
  const std::uint8_t* data_;
  std::size_t bit_len_;
  std::size_t pos_ = 0;
};

}  // namespace cf
