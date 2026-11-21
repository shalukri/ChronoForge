#include "chronoforge/algorithms/gorilla_codec.hpp"

#include <algorithm>
#include <bit>

namespace cf {
namespace {

// Delta-of-delta bucket widths. Control prefixes: 0 | 10 | 110 | 1110 | 1111.
// Chosen so that the overwhelmingly common cases (dod == 0 and |dod| of a few ms of scrape
// jitter) fit in 1 / 9 bits; the 64-bit escape guarantees losslessness for anything else.
constexpr unsigned kDodBits[3] = {7, 9, 12};

inline std::uint64_t toBits(double d) noexcept { return std::bit_cast<std::uint64_t>(d); }

// Interpret the low n bits of v as a two's-complement-like value with range [-(2^(n-1)-1), 2^(n-1)].
// The asymmetric range lets us spend zero extra codes on "-2^(n-1)" which dod==0 already covers.
inline std::int64_t signExtend(std::uint64_t v, unsigned n) noexcept {
  const std::uint64_t half = 1ULL << (n - 1);
  return v > half ? static_cast<std::int64_t>(v) - static_cast<std::int64_t>(1ULL << n)
                  : static_cast<std::int64_t>(v);
}

}  // namespace

// ---------------------------------------------------------------- encoder

void GorillaChunk::writeDod(std::int64_t dod) {
  if (dod == 0) {
    writer_.writeBit(false);
  } else if (dod >= -63 && dod <= 64) {
    writer_.writeBits(0b10, 2);
    writer_.writeBits(static_cast<std::uint64_t>(dod) & 0x7F, 7);
  } else if (dod >= -255 && dod <= 256) {
    writer_.writeBits(0b110, 3);
    writer_.writeBits(static_cast<std::uint64_t>(dod) & 0x1FF, 9);
  } else if (dod >= -2047 && dod <= 2048) {
    writer_.writeBits(0b1110, 4);
    writer_.writeBits(static_cast<std::uint64_t>(dod) & 0xFFF, 12);
  } else {
    writer_.writeBits(0b1111, 4);
    writer_.writeBits(static_cast<std::uint64_t>(dod), 64);
  }
}

void GorillaChunk::writeValue(std::uint64_t bits) {
  const std::uint64_t x = bits ^ prev_bits_;
  if (x == 0) {  // identical value: 1 bit. Counters that stall / constant gauges live here.
    writer_.writeBit(false);
  } else {
    writer_.writeBit(true);
    // Leading zeros are stored in 5 bits, so clamp to 31. Clamping only *widens* the stored
    // window (extra zero bits), it never loses information.
    const unsigned lz = std::min<unsigned>(std::countl_zero(x), 31);
    const unsigned tz = std::countr_zero(x);
    if (prev_lz_ != kNoWindow && lz >= prev_lz_ && tz >= prev_tz_) {
      // '10': meaningful bits fit inside the previous window -> skip re-sending the window.
      writer_.writeBit(false);
      writer_.writeBits(x >> prev_tz_, 64u - prev_lz_ - prev_tz_);
    } else {
      // '11': open a new window. len==64 is encoded as 0 (6 bits can't hold 64; len==0 is
      // impossible because x != 0), which the decoder maps back.
      const unsigned len = 64u - lz - tz;
      writer_.writeBit(true);
      writer_.writeBits(lz, 5);
      writer_.writeBits(len & 63u, 6);
      writer_.writeBits(x >> tz, len);
      prev_lz_ = static_cast<std::uint8_t>(lz);
      prev_tz_ = static_cast<std::uint8_t>(tz);
    }
  }
  prev_bits_ = bits;
}

GorillaChunk::AppendResult GorillaChunk::append(Timestamp ts, double value) {
  const std::uint64_t bits = toBits(value);
  if (count_ == 0) {
    // Header: raw 64-bit timestamp + raw 64-bit value (16 B, amortised over the whole chunk).
    writer_.writeBits(static_cast<std::uint64_t>(ts), 64);
    writer_.writeBits(bits, 64);
    min_ts_ = prev_ts_ = ts;
    prev_bits_ = bits;
    count_ = 1;
    return AppendResult::Ok;
  }
  if (ts < prev_ts_) return AppendResult::OutOfOrder;
  if (ts == prev_ts_) return AppendResult::DuplicateTimestamp;

  // Wrapping arithmetic on purpose: ts strictly increases, so delta is in [1, 2^64-1] and fits
  // uint64 exactly. dod = delta - prev_delta may exceed int64 as a mathematical integer, but its
  // value modulo 2^64 is all the decoder needs (it re-adds modulo 2^64).
  const std::uint64_t delta = static_cast<std::uint64_t>(ts) - static_cast<std::uint64_t>(prev_ts_);
  writeDod(static_cast<std::int64_t>(delta - prev_delta_));
  writeValue(bits);
  prev_delta_ = delta;
  prev_ts_ = ts;
  ++count_;
  return AppendResult::Ok;
}

// ---------------------------------------------------------------- decoder

GorillaChunk::Iterator::Iterator(const std::uint8_t* data, std::size_t bit_len, std::size_t count) noexcept
    : br_(data, bit_len), count_(count) {}

bool GorillaChunk::Iterator::readDod(std::int64_t& dod) {
  bool bit;
  if (!br_.readBit(bit)) return false;
  if (!bit) { dod = 0; return true; }
  std::uint64_t raw;
  for (unsigned i = 0; i < 3; ++i) {
    if (!br_.readBit(bit)) return false;
    if (!bit) {
      if (!br_.readBits(kDodBits[i], raw)) return false;
      dod = signExtend(raw, kDodBits[i]);
      return true;
    }
  }
  if (!br_.readBits(64, raw)) return false;
  dod = static_cast<std::int64_t>(raw);
  return true;
}

bool GorillaChunk::Iterator::readValue() {
  bool bit;
  if (!br_.readBit(bit)) return false;
  if (!bit) return true;  // value unchanged
  if (!br_.readBit(bit)) return false;
  std::uint64_t raw;
  if (!bit) {
    if (lz_ == kNoWindow) return false;  // "reuse window" before any window exists => corrupt
    const unsigned len = 64u - lz_ - tz_;
    if (!br_.readBits(len, raw)) return false;
    bits_ ^= raw << tz_;
  } else {
    std::uint64_t lz, len;
    if (!br_.readBits(5, lz) || !br_.readBits(6, len)) return false;
    if (len == 0) len = 64;
    if (lz + len > 64) return false;  // window would extend past bit 0 => corrupt
    const unsigned tz = static_cast<unsigned>(64 - lz - len);
    if (!br_.readBits(static_cast<unsigned>(len), raw)) return false;
    bits_ ^= raw << tz;
    lz_ = static_cast<std::uint8_t>(lz);
    tz_ = static_cast<std::uint8_t>(tz);
  }
  return true;
}

bool GorillaChunk::Iterator::next(Sample& out) {
  if (corrupted_ || index_ >= count_) return false;
  if (index_ == 0) {
    std::uint64_t t, v;
    if (!br_.readBits(64, t) || !br_.readBits(64, v)) return fail();
    ts_ = static_cast<Timestamp>(t);
    bits_ = v;
  } else {
    std::int64_t dod;
    if (!readDod(dod)) return fail();
    delta_ += static_cast<std::uint64_t>(dod);  // modulo 2^64, mirrors the encoder
    ts_ = static_cast<Timestamp>(static_cast<std::uint64_t>(ts_) + delta_);
    if (!readValue()) return fail();
  }
  out = Sample{ts_, std::bit_cast<double>(bits_)};
  ++index_;
  return true;
}

}  // namespace cf
