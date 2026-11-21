#pragma once
#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "chronoforge/algorithms/postings.hpp"
#include "chronoforge/models/label_set.hpp"
#include "chronoforge/models/matcher.hpp"

namespace cf {

// Series registry + label inverted index.
//
//   registry  : LabelSet  -> SeriesId   (dedup on write path)
//   postings  : label name -> label value -> sorted PostingList<SeriesId>   (query path)
//
// A selector like {__name__="cpu", region="us", host!="h7"} becomes:
//   (postings[__name__][cpu] ∩ postings[region][us]) \ postings[host][h7]
// i.e. set algebra over sorted integer lists -- no scan over series, cost depends on the
// SELECTIVITY of the selector, not on total cardinality.
//
// NOT thread-safe by itself: StorageEngine guards it with a shared_mutex (many concurrent
// readers; writers only on series creation, which is rare relative to sample appends).
class InvertedIndex {
 public:
  // O(1) expected. kInvalidSeries when unknown.
  SeriesId lookup(const LabelSet& ls) const;

  // Precondition: lookup(ls) == kInvalidSeries. Returns kInvalidSeries if the id space is full.
  SeriesId insert(const LabelSet& ls);

  const LabelSet& labelsOf(SeriesId id) const { return series_[id]; }
  std::size_t seriesCount() const noexcept { return series_.size(); }

  // EXACT distinct-value count for a label key -- free, it is the size of the value map.
  std::size_t distinctValues(const std::string& name) const;
  bool hasValue(const std::string& name, const std::string& value) const;

  // Evaluates a conjunction of matchers. `out` is sorted ascending. Returns false + *err on
  // an invalid selector (empty list, empty name, empty value for Equal/NotEqual/Prefix).
  bool select(const std::vector<Matcher>& matchers, PostingList& out, std::string* err) const;

  std::size_t approxMemoryBytes() const;

 private:
  // WHY std::map (ordered) for values and std::unordered_map for names:
  //  * Label NAMES are looked up by exact key only -> hash map, O(1).
  //  * Label VALUES also need Prefix matchers, which are a contiguous range in sorted order:
  //    lower_bound(prefix) then walk while the prefix holds = O(log V + K). A hash map would
  //    force an O(V) scan of every value for every prefix query.
  using ValueMap = std::map<std::string, PostingList, std::less<>>;

  std::vector<LabelSet> series_;  // id -> labels; ids are dense so a vector beats any map
  // multimap, not map: two *different* label sets may share a 64-bit hash. We keep both and
  // disambiguate by full comparison, so a collision costs a compare, never correctness.
  std::unordered_multimap<std::uint64_t, SeriesId> by_hash_;
  std::unordered_map<std::string, ValueMap> postings_;
};

}  // namespace cf
