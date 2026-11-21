#pragma once
// Core value types shared by every layer. Kept dependency-free on purpose so that
// algorithm headers can include it without dragging in services.
#include <cstdint>
#include <limits>

namespace cf {

// 32-bit series ids: postings lists are the dominant index cost (4 B per posting per label).
// A 64-bit id would double index memory for a ceiling (4.29 B series) no single node reaches.
using SeriesId = std::uint32_t;

// Milliseconds since Unix epoch. Signed so that deltas/differences never need casts at call sites.
using Timestamp = std::int64_t;

inline constexpr SeriesId kInvalidSeries = std::numeric_limits<SeriesId>::max();

struct Sample {
  Timestamp ts;
  double value;
};

}  // namespace cf
