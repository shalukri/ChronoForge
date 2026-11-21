#include "chronoforge/services/query_engine.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cf {

const char* toString(AggFn f) {
  switch (f) {
    case AggFn::Min: return "min";
    case AggFn::Max: return "max";
    case AggFn::Sum: return "sum";
    case AggFn::Avg: return "avg";
    case AggFn::Count: return "count";
    case AggFn::Last: return "last";
  }
  return "?";
}

bool QueryEngine::query(const std::vector<Matcher>& matchers, Timestamp from, Timestamp to,
                        std::vector<SeriesResult>& out, std::string* err) const {
  out.clear();
  if (from > to) {
    if (err) *err = "invalid range: from > to";
    return false;
  }
  std::vector<SeriesRef> refs;
  if (!engine_.selectSeries(matchers, refs, err, opt_.max_series)) return false;
  out.reserve(refs.size());
  for (SeriesRef& r : refs) {
    SeriesResult sr{r.id, std::move(r.labels), {}};
    if (!engine_.readSeries(r.id, from, to, sr.samples)) {
      if (err) *err = "chunk decode failed for series " + std::to_string(r.id);
      return false;
    }
    if (!sr.samples.empty()) out.push_back(std::move(sr));
  }
  return true;
}

bool QueryEngine::downsample(const std::vector<Sample>& in, Timestamp from, Timestamp to, Timestamp step,
                             AggFn fn, std::vector<Bucket>& out, std::string* err) {
  out.clear();
  if (step <= 0) { if (err) *err = "step must be positive"; return false; }
  if (from > to) { if (err) *err = "invalid range: from > to"; return false; }

  struct Acc {
    Timestamp idx = -1;
    double mn = 0, mx = 0, sum = 0, last = 0;
    std::uint32_t n = 0;
  } a;

  auto flush = [&]() {
    if (a.n == 0) return;
    double v = 0;
    switch (fn) {
      case AggFn::Min: v = a.mn; break;
      case AggFn::Max: v = a.mx; break;
      case AggFn::Sum: v = a.sum; break;
      case AggFn::Avg: v = a.sum / a.n; break;
      case AggFn::Count: v = a.n; break;
      case AggFn::Last: v = a.last; break;
    }
    out.push_back({from + a.idx * step, v, a.n});
  };

  for (const Sample& s : in) {
    if (s.ts < from || s.ts > to || std::isnan(s.value)) continue;
    // Samples are time-sorted, so bucket indices are non-decreasing: a single open accumulator
    // suffices -- O(1) memory instead of a hash map of buckets.
    const Timestamp idx = (s.ts - from) / step;
    if (a.n == 0 || idx != a.idx) {
      flush();
      a = Acc{};
      a.idx = idx;
      a.mn = std::numeric_limits<double>::infinity();
      a.mx = -std::numeric_limits<double>::infinity();
    }
    a.mn = std::min(a.mn, s.value);
    a.mx = std::max(a.mx, s.value);
    a.sum += s.value;
    a.last = s.value;
    ++a.n;
  }
  flush();
  return true;
}

}  // namespace cf
