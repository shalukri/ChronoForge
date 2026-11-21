#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "chronoforge/algorithms/hyperloglog.hpp"
#include "chronoforge/models/label_set.hpp"
#include "chronoforge/services/inverted_index.hpp"

namespace cf {

// Defence against "cardinality explosions": one buggy client putting a unique id (user_id,
// trace_id, URL with query string) in a label creates millions of series and takes the whole
// metrics backend down. This is the #1 real-world operational failure of Prometheus-style TSDBs.
//
// Two complementary mechanisms, deliberately using different data structures:
//   * ADMISSION (hard decision) uses EXACT counts from the index: a new series is rejected if
//     it would introduce a NEW value to a label key that already holds >= limit values.
//     Existing series keep working -- load shedding degrades gracefully, it never blackholes.
//   * OBSERVABILITY (soft signal) uses a HyperLogLog per label key counting every ATTEMPTED
//     value, including rejected ones that are (by design) not stored anywhere. Operators then
//     see "user_id: 200,000 attempted vs 50,000 admitted" in 4 KiB per key.
//
// Thread-safety: externally synchronised (StorageEngine holds the index write-lock when
// calling recordAttempt/admit; report() runs under the read-lock).
class CardinalityGuard {
 public:
  struct Limits {
    std::size_t max_series;
    std::size_t max_distinct_values_per_label;
  };
  enum class Verdict { Admit, SeriesLimit, LabelValueLimit };

  struct KeyReport {
    std::string key;
    std::uint64_t attempted_estimate;  // HLL: every value ever offered, admitted or not
    std::size_t admitted_exact;        // exact: values actually present in the index
  };

  explicit CardinalityGuard(Limits limits) : limits_(limits) {}

  void recordAttempt(const LabelSet& ls);
  Verdict admit(const LabelSet& ls, const InvertedIndex& index) const;
  std::vector<KeyReport> report(const InvertedIndex& index) const;

 private:
  // An attacker controls label NAMES too. Without this cap, sending unique label names would
  // grow this map (4 KiB each) without bound -- the guard itself would be the memory leak.
  static constexpr std::size_t kMaxTrackedKeys = 256;

  Limits limits_;
  std::unordered_map<std::string, HyperLogLog<12>> attempted_;
};

}  // namespace cf
