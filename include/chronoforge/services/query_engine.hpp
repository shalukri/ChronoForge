#pragma once
#include <string>
#include <vector>

#include "chronoforge/services/storage_engine.hpp"

namespace cf {

struct SeriesResult {
  SeriesId id;
  LabelSet labels;
  std::vector<Sample> samples;
};

enum class AggFn { Min, Max, Sum, Avg, Count, Last };
const char* toString(AggFn f);

struct Bucket {
  Timestamp start;       // inclusive bucket start (aligned to `from`)
  double value;
  std::uint32_t samples; // how many raw samples fed this bucket
};

// Read-side facade: label selection (index) -> time-range scan (chunks) -> optional downsampling.
class QueryEngine {
 public:
  struct Options {
    std::size_t max_series = 100'000;  // protects the process from a selector matching "everything"
  };

  // Two constructors instead of `Options opt = {}`: a nested struct with default member
  // initializers cannot be used as a default argument inside its own enclosing class (GCC/Clang
  // evaluate default arguments before the enclosing class is complete).
  explicit QueryEngine(const StorageEngine& engine) : QueryEngine(engine, Options{}) {}
  QueryEngine(const StorageEngine& engine, Options opt) : engine_(engine), opt_(opt) {}

  // Series that match and have >= 1 sample in [from, to]. Inclusive range.
  bool query(const std::vector<Matcher>& matchers, Timestamp from, Timestamp to,
             std::vector<SeriesResult>& out, std::string* err) const;

  // Single-pass, O(N) step-window aggregation over a time-sorted sample vector.
  // Buckets are [from + k*step, from + (k+1)*step). Empty buckets are omitted (sparse output);
  // NaN samples (staleness markers) are ignored so they never poison a sum/min/max.
  static bool downsample(const std::vector<Sample>& in, Timestamp from, Timestamp to, Timestamp step,
                         AggFn fn, std::vector<Bucket>& out, std::string* err);

 private:
  const StorageEngine& engine_;
  Options opt_;
};

}  // namespace cf
