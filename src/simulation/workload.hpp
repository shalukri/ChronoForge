#pragma once
// Deterministic synthetic telemetry. Counter-based RNG => identical numbers on every run and
// platform, so benchmark results in the README are reproducible, not anecdotal.
#include <cmath>
#include <cstdint>

#include "chronoforge/algorithms/hashing.hpp"
#include "chronoforge/models/types.hpp"

namespace sim {

class Rng {
 public:
  explicit Rng(std::uint64_t seed) : ctr_(seed * 0x9e3779b97f4a7c15ULL) {}
  std::uint64_t next() { return cf::mix64(ctr_++); }
  double uniform() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  // Irwin-Hall approximation of N(0,1): sum of 12 U(0,1) minus 6. Cheap and good enough here.
  double normal() { double s = 0; for (int i = 0; i < 12; ++i) s += uniform(); return s - 6.0; }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
 private:
  std::uint64_t ctr_;
};

// The four value "shapes" that cover real metric families -- and bracket compressibility
// from best case to adversarial worst case.
enum class Shape { Constant, Counter, Gauge, Noise };

inline const char* name(Shape s) {
  switch (s) {
    case Shape::Constant: return "constant (up{} / config gauge)";
    case Shape::Counter:  return "counter (requests_total)";
    case Shape::Gauge:    return "quantized gauge (cpu %, 0.01 res)";
    case Shape::Noise:    return "random doubles (adversarial)";
  }
  return "?";
}

class ValueSource {
 public:
  ValueSource(Shape shape, std::uint64_t seed) : shape_(shape), rng_(seed) {
    v_ = shape == Shape::Gauge ? 40.0 + 20.0 * rng_.uniform() : shape == Shape::Counter ? 1000.0 : 1.0;
  }
  double next() {
    switch (shape_) {
      case Shape::Constant: return v_;
      case Shape::Counter:  v_ += static_cast<double>(rng_.below(40)); return v_;
      case Shape::Gauge: {
        v_ += rng_.normal() * 0.4;
        v_ = std::fmin(100.0, std::fmax(0.0, v_));
        return std::round(v_ * 100.0) / 100.0;
      }
      case Shape::Noise:    return rng_.uniform() * 1e6;
    }
    return 0;
  }
 private:
  Shape shape_;
  Rng rng_;
  double v_;
};

// Scrape-time model: fixed interval with 5% of samples jittered by up to +-5 ms. Real scrapers
// are never perfectly periodic; ignoring jitter would flatter the delta-of-delta encoding.
class Clock {
 public:
  Clock(cf::Timestamp start_ms, cf::Timestamp interval_ms, std::uint64_t seed)
      : start_(start_ms), interval_(interval_ms), rng_(seed) {}
  cf::Timestamp at(std::uint64_t step) {
    cf::Timestamp t = start_ + static_cast<cf::Timestamp>(step) * interval_;
    if (rng_.below(100) < 5) t += static_cast<cf::Timestamp>(rng_.below(11)) - 5;
    return t;
  }
 private:
  cf::Timestamp start_, interval_;
  Rng rng_;
};

}  // namespace sim
