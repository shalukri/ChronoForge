#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "chronoforge/algorithms/bit_stream.hpp"
#include "chronoforge/models/types.hpp"

namespace cf {

// A compressed, append-only block of (timestamp, double) samples for ONE series, using the
// scheme from Facebook's "Gorilla" paper (VLDB 2015):
//
//   timestamps : delta-of-delta  -> a regular scrape interval costs 1 BIT per sample
//   values     : XOR with the previous value; store only the "meaningful" middle bits,
//                reusing the previous leading/trailing-zero window when possible
//
// WHY this instead of general-purpose compression (zstd/LZ4)?
//   * Streaming: a sample is appended in O(1) with no re-compression of the block. LZ-style
//     codecs need a whole buffer to find matches; per-sample append would be O(block).
//   * Domain-aware: metrics are slowly varying and regularly spaced, so *arithmetic* structure
//     (equal deltas, near-equal float mantissas) is the redundancy -- not byte repetition.
//   * No decompress-to-query dictionary state; a block is self-contained and ~100-200 bytes.
//
// WHY NOT random access: delta-of-delta + XOR chains make sample i depend on all i-1 before it.
// We bound the cost by keeping blocks small (~120 samples): decode cost per query is O(120),
// and block-level min/max timestamps let the engine skip blocks without decoding.
//
// Robustness decisions beyond the paper:
//   * Timestamp deltas use *wrapping uint64 arithmetic* so extreme gaps (even INT64_MIN ->
//     INT64_MAX) round-trip exactly instead of invoking signed-overflow UB.
//   * Escape code carries a full 64-bit delta-of-delta (paper: 32) -> no silent truncation.
//   * Doubles are moved via bit_cast, so NaN payloads, -0.0 and denormals are lossless.
//   * The decoder is bounds-checked and validates every field of untrusted bit patterns.
class GorillaChunk {
 public:
  static constexpr std::uint8_t kNoWindow = 0xFF;

  enum class AppendResult { Ok, OutOfOrder, DuplicateTimestamp };

  // Timestamps must be strictly increasing within a chunk.
  AppendResult append(Timestamp ts, double value);

  // Release encoder slack. After sealing, the chunk is immutable by convention.
  void seal() { writer_.shrinkToFit(); }

  std::size_t count() const noexcept { return count_; }
  Timestamp minTime() const noexcept { return min_ts_; }
  Timestamp maxTime() const noexcept { return prev_ts_; }
  std::size_t encodedBits() const noexcept { return writer_.bitSize(); }
  std::size_t encodedBytes() const noexcept { return writer_.bytes().size(); }
  const std::vector<std::uint8_t>& bytes() const noexcept { return writer_.bytes(); }

  class Iterator {
   public:
    // Can decode any raw buffer (used by tests to feed truncated / bit-flipped data).
    Iterator(const std::uint8_t* data, std::size_t bit_len, std::size_t count) noexcept;
    bool next(Sample& out);                    // false on end OR corruption
    bool corrupted() const noexcept { return corrupted_; }

   private:
    bool readDod(std::int64_t& dod);
    bool readValue();
    bool fail() noexcept { corrupted_ = true; return false; }

    BitReader br_;
    std::size_t count_;
    std::size_t index_ = 0;
    Timestamp ts_ = 0;
    std::uint64_t delta_ = 0;
    std::uint64_t bits_ = 0;
    std::uint8_t lz_ = kNoWindow;
    std::uint8_t tz_ = 0;
    bool corrupted_ = false;
  };

  Iterator iterator() const noexcept { return Iterator(writer_.bytes().data(), writer_.bitSize(), count_); }

 private:
  void writeDod(std::int64_t dod);
  void writeValue(std::uint64_t bits);

  BitWriter writer_;
  std::size_t count_ = 0;
  Timestamp min_ts_ = 0;
  Timestamp prev_ts_ = 0;
  std::uint64_t prev_delta_ = 0;
  std::uint64_t prev_bits_ = 0;
  std::uint8_t prev_lz_ = kNoWindow;  // current XOR "meaningful bits" window
  std::uint8_t prev_tz_ = 0;
};

}  // namespace cf
