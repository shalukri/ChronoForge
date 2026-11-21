#include "chronoforge/services/inverted_index.hpp"

#include <deque>
#include <numeric>

namespace cf {
namespace {
bool fail(std::string* err, std::string msg) {
  if (err) *err = std::move(msg);
  return false;
}
bool startsWith(const std::string& s, const std::string& p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
}  // namespace

SeriesId InvertedIndex::lookup(const LabelSet& ls) const {
  auto range = by_hash_.equal_range(ls.hash());
  for (auto it = range.first; it != range.second; ++it)
    if (series_[it->second] == ls) return it->second;
  return kInvalidSeries;
}

SeriesId InvertedIndex::insert(const LabelSet& ls) {
  if (series_.size() >= static_cast<std::size_t>(kInvalidSeries)) return kInvalidSeries;
  const auto id = static_cast<SeriesId>(series_.size());
  series_.push_back(ls);
  by_hash_.emplace(ls.hash(), id);
  // `id` is strictly greater than every id already in the index, so push_back keeps every
  // posting list sorted. This invariant is the reason the whole index needs no sorting step.
  for (const Label& l : ls.labels()) postings_[l.name][l.value].push_back(id);
  return id;
}

std::size_t InvertedIndex::distinctValues(const std::string& name) const {
  auto it = postings_.find(name);
  return it == postings_.end() ? 0 : it->second.size();
}

bool InvertedIndex::hasValue(const std::string& name, const std::string& value) const {
  auto it = postings_.find(name);
  return it != postings_.end() && it->second.find(value) != it->second.end();
}

bool InvertedIndex::select(const std::vector<Matcher>& matchers, PostingList& out, std::string* err) const {
  out.clear();
  if (matchers.empty()) return fail(err, "selector needs at least one matcher");
  for (const Matcher& m : matchers) {
    if (m.name.empty()) return fail(err, "matcher with empty label name");
    if (m.op != MatchOp::Exists && m.value.empty())
      return fail(err, "empty value in matcher on '" + m.name + "' (absent label == no label; use NotEqual/Exists)");
  }

  std::deque<PostingList> owned;               // deque: stable addresses for pointers below
  std::vector<const PostingList*> positives;   // intersected
  std::vector<const PostingList*> negatives;   // subtracted
  bool has_positive = false;

  for (const Matcher& m : matchers) {
    const auto name_it = postings_.find(m.name);
    switch (m.op) {
      case MatchOp::Equal: {
        has_positive = true;
        if (name_it == postings_.end()) return true;  // label never seen => empty result
        auto v = name_it->second.find(m.value);
        if (v == name_it->second.end()) return true;
        positives.push_back(&v->second);
        break;
      }
      case MatchOp::NotEqual: {
        if (name_it == postings_.end()) break;  // nothing to subtract
        auto v = name_it->second.find(m.value);
        if (v != name_it->second.end()) negatives.push_back(&v->second);
        break;
      }
      case MatchOp::Prefix:
      case MatchOp::Exists: {
        has_positive = true;
        if (name_it == postings_.end()) return true;
        const ValueMap& vm = name_it->second;
        std::vector<const PostingList*> parts;
        if (m.op == MatchOp::Exists) {
          for (const auto& kv : vm) parts.push_back(&kv.second);
        } else {
          for (auto it = vm.lower_bound(m.value); it != vm.end() && startsWith(it->first, m.value); ++it)
            parts.push_back(&it->second);
        }
        if (parts.empty()) return true;
        if (parts.size() == 1) {
          positives.push_back(parts[0]);  // no copy needed
        } else {
          owned.push_back(unionMany(parts));
          positives.push_back(&owned.back());
        }
        break;
      }
    }
  }

  PostingList result;
  if (has_positive) {
    result = intersectMany(positives);
  } else {
    // Only negative matchers: universe is every series. Ids are dense [0,n) so the universe is
    // implicit -- no "all series" list is stored (saves 4 B/series permanently).
    result.resize(series_.size());
    std::iota(result.begin(), result.end(), SeriesId{0});
  }
  // Negatives last: they can only shrink the set, so applying them to the already-reduced
  // result minimises work.
  for (const PostingList* neg : negatives) {
    if (result.empty()) break;
    result = difference(result, *neg);
  }
  out = std::move(result);
  return true;
}

std::size_t InvertedIndex::approxMemoryBytes() const {
  std::size_t bytes = series_.capacity() * sizeof(LabelSet) + by_hash_.size() * 40;
  for (const LabelSet& ls : series_)
    for (const Label& l : ls.labels()) bytes += l.name.capacity() + l.value.capacity() + 2 * sizeof(std::string);
  for (const auto& [name, vm] : postings_)
    for (const auto& [value, pl] : vm) bytes += pl.capacity() * sizeof(SeriesId) + value.capacity() + 64;
  return bytes;
}

}  // namespace cf
