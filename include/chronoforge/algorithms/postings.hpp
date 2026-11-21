#pragma once
// Sorted-set algebra over posting lists (sorted, de-duplicated vectors of SeriesId).
//
// WHY sorted std::vector<uint32_t> and not std::set / unordered_set / bitmaps?
//   * Contiguous memory: 4 B/posting vs ~32-48 B/node for std::set. At 10^7 series x ~8 labels
//     that is the difference between ~300 MB and ~3 GB of index.
//   * Series ids are assigned monotonically, so a new posting is ALWAYS the largest element:
//     insertion is an O(1) amortised push_back that preserves sortedness. No re-sorting, ever.
//   * Sorted order enables merge-style set algebra that streams through memory sequentially.
//   * (Roaring bitmaps win for very dense lists; sparse high-cardinality lists -- the case this
//     engine is built for -- are exactly where plain sorted arrays are smaller and faster.)
#include <algorithm>
#include <cstddef>
#include <queue>
#include <vector>

#include "chronoforge/models/types.hpp"

namespace cf {

using PostingList = std::vector<SeriesId>;

// Below this size ratio a plain two-pointer merge beats galloping (galloping pays ~2*log2(gap)
// compares per element; merge pays gap). Empirically the crossover sits around 8-16.
inline constexpr std::size_t kGallopRatio = 8;

// First index i >= lo with v[i] >= target (or v.size()). Exponential probe + binary search:
// O(log d) where d is the distance advanced, NOT O(log n). That distinction is what makes
// intersecting a tiny list against a huge one cost O(m log(n/m)) instead of O(n).
inline std::size_t gallopLowerBound(const PostingList& v, std::size_t lo, SeriesId target) {
  const std::size_t n = v.size();
  if (lo >= n || v[lo] >= target) return lo;
  // Invariant: v[lo] < target.
  std::size_t bound = 1;
  while (lo + bound < n && v[lo + bound] < target) bound <<= 1;
  // v[lo + bound/2] < target, and (lo + bound >= n  or  v[lo + bound] >= target).
  const std::size_t left = lo + (bound >> 1) + 1;
  const std::size_t right = std::min(lo + bound, n);
  return static_cast<std::size_t>(std::lower_bound(v.begin() + left, v.begin() + right, target) - v.begin());
}

// A ∩ B. Picks merge vs galloping from the size ratio.
inline PostingList intersect(const PostingList& x, const PostingList& y) {
  const PostingList& a = x.size() <= y.size() ? x : y;  // iterate the smaller side
  const PostingList& b = x.size() <= y.size() ? y : x;
  PostingList out;
  if (a.empty()) return out;
  out.reserve(a.size());
  if (b.size() / a.size() < kGallopRatio) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      if (a[i] < b[j]) ++i;
      else if (b[j] < a[i]) ++j;
      else { out.push_back(a[i]); ++i; ++j; }
    }
  } else {
    std::size_t j = 0;
    for (SeriesId id : a) {
      j = gallopLowerBound(b, j, id);
      if (j == b.size()) break;
      if (b[j] == id) { out.push_back(id); ++j; }
    }
  }
  return out;
}

// ⋂ of many lists. Ordering by ascending size means the running result is bounded by the
// SMALLEST list from the first step on, and an empty intermediate short-circuits everything.
inline PostingList intersectMany(std::vector<const PostingList*> lists) {
  if (lists.empty()) return {};
  std::sort(lists.begin(), lists.end(), [](const PostingList* a, const PostingList* b) { return a->size() < b->size(); });
  PostingList acc = *lists[0];
  for (std::size_t i = 1; i < lists.size() && !acc.empty(); ++i) acc = intersect(acc, *lists[i]);
  return acc;
}

// A \ B, galloping over B (cheap when B is much larger than A, degenerates to merge otherwise).
inline PostingList difference(const PostingList& a, const PostingList& b) {
  PostingList out;
  out.reserve(a.size());
  std::size_t j = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    j = gallopLowerBound(b, j, a[i]);
    if (j == b.size()) { out.insert(out.end(), a.begin() + static_cast<std::ptrdiff_t>(i), a.end()); break; }
    if (b[j] != a[i]) out.push_back(a[i]);
  }
  return out;
}

// ⋃ of k lists via a min-heap k-way merge: O(N log k) with output produced already sorted.
// The alternative (concatenate + sort + unique) is O(N log N) and allocates 2N.
inline PostingList unionMany(const std::vector<const PostingList*>& lists) {
  if (lists.empty()) return {};
  if (lists.size() == 1) return *lists[0];
  struct Cursor { SeriesId value; std::size_t list; std::size_t pos; };
  auto cmp = [](const Cursor& a, const Cursor& b) { return a.value > b.value; };
  std::priority_queue<Cursor, std::vector<Cursor>, decltype(cmp)> heap(cmp);
  std::size_t total = 0;
  for (std::size_t i = 0; i < lists.size(); ++i) {
    if (lists[i]->empty()) continue;
    heap.push({(*lists[i])[0], i, 0});
    total += lists[i]->size();
  }
  PostingList out;
  out.reserve(total);  // exact when lists are disjoint (distinct values of ONE label are)
  while (!heap.empty()) {
    Cursor c = heap.top();
    heap.pop();
    if (out.empty() || out.back() != c.value) out.push_back(c.value);
    if (++c.pos < lists[c.list]->size()) {
      c.value = (*lists[c.list])[c.pos];
      heap.push(c);
    }
  }
  return out;
}

}  // namespace cf
