#include "chronoforge/services/cardinality_guard.hpp"

#include <algorithm>

namespace cf {

void CardinalityGuard::recordAttempt(const LabelSet& ls) {
  for (const Label& l : ls.labels()) {
    auto it = attempted_.find(l.name);
    if (it == attempted_.end()) {
      if (attempted_.size() >= kMaxTrackedKeys) continue;  // bounded memory under hostile input
      it = attempted_.emplace(l.name, HyperLogLog<12>{}).first;
    }
    it->second.addString(l.value);
  }
}

CardinalityGuard::Verdict CardinalityGuard::admit(const LabelSet& ls, const InvertedIndex& index) const {
  if (index.seriesCount() >= limits_.max_series) return Verdict::SeriesLimit;
  for (const Label& l : ls.labels()) {
    // Only a NEW value can raise a key's cardinality; reusing a known value is always fine.
    if (!index.hasValue(l.name, l.value) && index.distinctValues(l.name) >= limits_.max_distinct_values_per_label)
      return Verdict::LabelValueLimit;
  }
  return Verdict::Admit;
}

std::vector<CardinalityGuard::KeyReport> CardinalityGuard::report(const InvertedIndex& index) const {
  std::vector<KeyReport> out;
  out.reserve(attempted_.size());
  for (const auto& [key, hll] : attempted_) out.push_back({key, hll.estimate(), index.distinctValues(key)});
  std::sort(out.begin(), out.end(), [](const KeyReport& a, const KeyReport& b) { return a.attempted_estimate > b.attempted_estimate; });
  return out;
}

}  // namespace cf
