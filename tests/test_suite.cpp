// Dependency-free test suite. Every structure is checked against a trivially-correct oracle
// (std::set_intersection, std::lower_bound, brute-force label matching, exact distinct counts).
#include <algorithm>
#include <atomic>
#include <bit>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iostream>
#include <random>
#include <set>
#include <thread>

#include "chronoforge/algorithms/bit_stream.hpp"
#include "chronoforge/algorithms/gorilla_codec.hpp"
#include "chronoforge/algorithms/hyperloglog.hpp"
#include "chronoforge/algorithms/postings.hpp"
#include "chronoforge/services/inverted_index.hpp"
#include "chronoforge/services/query_engine.hpp"
#include "chronoforge/services/storage_engine.hpp"

using namespace cf;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                   \
  do {                                                                                \
    ++g_checks;                                                                       \
    if (!(cond)) { ++g_failed; std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

static void run(const char* name, const std::function<void()>& fn) {
  const int before = g_failed;
  fn();
  std::printf("[%s] %s\n", g_failed == before ? " ok " : "FAIL", name);
}

static LabelSet mk(std::vector<Label> l) { return *LabelSet::create(std::move(l)); }
static std::uint64_t B(double d) { return std::bit_cast<std::uint64_t>(d); }

// ---------------------------------------------------------------- bit stream
static void testBitStream() {
  std::mt19937_64 rng(1);
  std::vector<std::pair<std::uint64_t, unsigned>> items;
  BitWriter w;
  for (int i = 0; i < 20000; ++i) {
    unsigned n = rng() % 65;
    std::uint64_t v = n == 64 ? rng() : (rng() & ((n == 0) ? 0 : ((1ULL << n) - 1)));
    items.push_back({v, n});
    w.writeBits(v, n);
  }
  BitReader r(w.bytes().data(), w.bitSize());
  for (auto& [v, n] : items) {
    std::uint64_t got = ~0ULL;
    CHECK(r.readBits(n, got));
    CHECK(got == v);
  }
  std::uint64_t x;
  CHECK(!r.readBits(1, x));  // exactly exhausted
  BitReader empty(nullptr, 0);
  CHECK(!empty.readBits(1, x));
  CHECK(empty.readBits(0, x) && x == 0);
}

// ---------------------------------------------------------------- gorilla
static std::vector<Sample> roundTrip(const std::vector<Sample>& in) {
  GorillaChunk c;
  for (auto& s : in) CHECK(c.append(s.ts, s.value) == GorillaChunk::AppendResult::Ok);
  std::vector<Sample> out;
  auto it = c.iterator();
  Sample s;
  while (it.next(s)) out.push_back(s);
  CHECK(!it.corrupted());
  return out;
}
static bool same(const std::vector<Sample>& a, const std::vector<Sample>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].ts != b[i].ts || B(a[i].value) != B(b[i].value)) return false;
  return true;
}

static void testGorillaSpecialValues() {
  const double nan_payload = std::bit_cast<double>(0x7FF8DEADBEEF0001ULL);
  const double specials[] = {0.0, -0.0, 1.0, -1.0, NAN, nan_payload, INFINITY, -INFINITY, DBL_MIN, DBL_MAX,
                             DBL_TRUE_MIN, 0.1, 0.1, 0.1, 1e-300, 1e300, 3.141592653589793};
  std::vector<Sample> in;
  Timestamp t = 1000;
  for (double v : specials) in.push_back({t += 15000, v});
  CHECK(same(in, roundTrip(in)));
}

static void testGorillaTimestampExtremes() {
  // Gaps that overflow signed 64-bit arithmetic if implemented naively.
  CHECK(same({{INT64_MIN, 1.0}, {0, 2.0}, {INT64_MAX, 3.0}}, roundTrip({{INT64_MIN, 1.0}, {0, 2.0}, {INT64_MAX, 3.0}})));
  std::vector<Sample> in = {{INT64_MIN, 1}, {INT64_MIN + 1, 2}, {INT64_MAX - 1, 3}, {INT64_MAX, 4}};
  CHECK(same(in, roundTrip(in)));
  // Every delta-of-delta bucket boundary, both signs.
  std::vector<Sample> b;
  Timestamp t = 0, delta = 1000;
  b.push_back({t, 0});
  for (long long dod : {0LL, 1LL, -1LL, 64LL, -63LL, 65LL, -64LL, 256LL, -255LL, 257LL, -256LL, 2048LL, -2047LL, 2049LL, -2048LL, 100000LL, -90000LL, 0LL}) {
    delta += dod;
    t += delta;
    b.push_back({t, double(b.size())});
  }
  CHECK(same(b, roundTrip(b)));
}

static void testGorillaFuzz() {
  std::mt19937_64 rng(42);
  for (int trial = 0; trial < 300; ++trial) {
    std::vector<Sample> in;
    Timestamp t = (Timestamp)(rng() % 1'000'000'000'000ULL);
    double v = double(rng() % 1000);
    const int n = 1 + rng() % 400;
    for (int i = 0; i < n; ++i) {
      switch (rng() % 4) {
        case 0: t += 15000; break;
        case 1: t += 15000 + (Timestamp)(rng() % 21) - 10; break;
        case 2: t += 1 + (Timestamp)(rng() % 100000); break;
        default: t += 1 + (Timestamp)(rng() % (1ULL << 40)); break;
      }
      switch (rng() % 5) {
        case 0: break;                                   // unchanged
        case 1: v += double(rng() % 10); break;          // counter-ish
        case 2: v = double(rng() % 100000) / 100.0; break;
        case 3: v = std::bit_cast<double>(rng()); break; // arbitrary bit patterns incl. NaN/denormals
        default: v = -v; break;
      }
      in.push_back({t, v});
    }
    CHECK(same(in, roundTrip(in)));
  }
}

static void testGorillaOrderingAndCompression() {
  GorillaChunk c;
  CHECK(c.append(100, 1) == GorillaChunk::AppendResult::Ok);
  CHECK(c.append(100, 2) == GorillaChunk::AppendResult::DuplicateTimestamp);
  CHECK(c.append(99, 2) == GorillaChunk::AppendResult::OutOfOrder);
  CHECK(c.count() == 1);  // rejected appends must not mutate the chunk
  CHECK(c.minTime() == 100 && c.maxTime() == 100);

  // A perfectly regular constant series must collapse to ~2 bits/sample after the header.
  GorillaChunk k;
  for (int i = 0; i < 1000; ++i) k.append(1'000'000 + i * 15000LL, 7.0);
  CHECK(k.encodedBits() <= 128 + 68 + 999 * 2 + 8);
  GorillaChunk empty;
  Sample s;
  auto it = empty.iterator();
  CHECK(!it.next(s) && !it.corrupted());
}

static void testGorillaCorruption() {
  GorillaChunk c;
  std::mt19937_64 rng(7);
  for (int i = 0; i < 100; ++i) c.append(1000 + i * 1000LL + (Timestamp)(rng() % 7), double(rng() % 1000) / 8);
  // Truncation at every possible bit length must terminate cleanly: no crash, no over-read.
  for (size_t bits = 0; bits < c.encodedBits(); bits += 3) {
    GorillaChunk::Iterator it(c.bytes().data(), bits, c.count());
    Sample s;
    size_t n = 0;
    while (it.next(s)) ++n;
    CHECK(n < c.count());
    CHECK(it.corrupted());
  }
  // Random bit flips: decoder may produce garbage but must terminate and stay in bounds
  // (the sanitizer build is what actually proves the "in bounds" part).
  for (int trial = 0; trial < 2000; ++trial) {
    auto bytes = c.bytes();
    for (int f = 0; f < 1 + trial % 4; ++f) bytes[rng() % bytes.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
    GorillaChunk::Iterator it(bytes.data(), c.encodedBits(), c.count());
    Sample s;
    size_t n = 0;
    while (it.next(s)) ++n;
    CHECK(n <= c.count());
  }
}

// ---------------------------------------------------------------- postings
static PostingList randomList(std::mt19937_64& rng, size_t n, SeriesId universe) {
  std::set<SeriesId> s;
  while (s.size() < n) s.insert(static_cast<SeriesId>(rng() % universe));
  return PostingList(s.begin(), s.end());
}

static void testPostings() {
  std::mt19937_64 rng(99);
  for (int trial = 0; trial < 400; ++trial) {
    const SeriesId U = 1 + rng() % 5000;
    const size_t na = rng() % std::min<SeriesId>(U, 80), nb = rng() % U;
    PostingList a = randomList(rng, na, U), b = randomList(rng, nb, U);
    PostingList want_i, want_d, want_u;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(want_i));
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(want_d));
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(want_u));
    CHECK(intersect(a, b) == want_i);
    CHECK(intersect(b, a) == want_i);
    CHECK(difference(a, b) == want_d);
    CHECK(unionMany({&a, &b}) == want_u);
    // gallop vs std::lower_bound for every lo / random target
    for (int k = 0; k < 20 && !b.empty(); ++k) {
      size_t lo = rng() % (b.size() + 1);
      SeriesId target = static_cast<SeriesId>(rng() % (U + 2));
      CHECK(gallopLowerBound(b, lo, target) == static_cast<size_t>(std::lower_bound(b.begin() + lo, b.end(), target) - b.begin()));
    }
  }
  // k-way
  PostingList l1 = {1, 3, 5, 7}, l2 = {3, 4, 5, 6}, l3 = {5, 9}, e;
  CHECK(intersectMany({&l1, &l2, &l3}) == PostingList{5});
  CHECK(intersectMany({&l1, &e}).empty());
  CHECK(intersectMany({}).empty());
  CHECK((unionMany({&l1, &l2, &l3, &e}) == PostingList{1, 3, 4, 5, 6, 7, 9}));
  CHECK(unionMany({}).empty());
  CHECK(difference(e, l1).empty());
  CHECK(difference(l1, e) == l1);
}

// ---------------------------------------------------------------- HyperLogLog
static void testHll() {
  auto estimate = [](size_t n) {
    HyperLogLog<12> h;
    for (size_t i = 0; i < n; ++i) h.addString("value-" + std::to_string(i));
    return double(h.estimate());
  };
  CHECK(HyperLogLog<12>{}.estimate() == 0);
  for (size_t n : {1u, 2u, 10u, 100u, 1000u, 10000u, 100000u, 500000u}) {
    const double e = estimate(n);
    const double tol = n < 20 ? 1.0 : 0.05 * n;  // ~3 sigma at p=12
    CHECK(std::fabs(e - double(n)) <= tol);
  }
  HyperLogLog<12> dup;  // duplicates must not inflate the estimate
  for (int i = 0; i < 200000; ++i) dup.addString("same");
  CHECK(dup.estimate() == 1);
  HyperLogLog<12> a, b, u;  // merge == sketch of the union, and overlap is not double-counted
  for (int i = 0; i < 30000; ++i) { a.addString("k" + std::to_string(i)); u.addString("k" + std::to_string(i)); }
  for (int i = 20000; i < 50000; ++i) { b.addString("k" + std::to_string(i)); u.addString("k" + std::to_string(i)); }
  a.merge(b);
  CHECK(a.estimate() == u.estimate());
  CHECK(std::fabs(double(a.estimate()) - 50000.0) <= 0.05 * 50000);
}

// ---------------------------------------------------------------- labels
static void testLabelSet() {
  std::string err;
  CHECK(!LabelSet::create({}, &err));
  CHECK(!LabelSet::create({{"1bad", "x"}}, &err));
  CHECK(!LabelSet::create({{"bad-name", "x"}}, &err));
  CHECK(!LabelSet::create({{"ok", ""}}, &err));
  CHECK(!LabelSet::create({{"a", "1"}, {"a", "2"}}, &err) && err.find("duplicate") != std::string::npos);
  CHECK(!LabelSet::create({{"a", std::string(257, 'x')}}, &err));
  std::vector<Label> many;
  for (int i = 0; i < 33; ++i) many.push_back({"l" + std::to_string(i), "v"});
  CHECK(!LabelSet::create(many, &err));
  auto x = LabelSet::create({{"b", "2"}, {"a", "1"}});
  auto y = LabelSet::create({{"a", "1"}, {"b", "2"}});
  CHECK(x && y && *x == *y && x->hash() == y->hash());  // order-insensitive identity
  CHECK(!(mk({{"ab", "c"}}) == mk({{"a", "bc"}})));      // no concatenation ambiguity
  CHECK(*x->find("a") == "1" && x->find("zz") == nullptr);
}

// ---------------------------------------------------------------- index vs brute force
static void testIndexFuzz() {
  std::mt19937_64 rng(2024);
  InvertedIndex idx;
  std::vector<LabelSet> all;
  const char* keys[] = {"a", "b", "c", "d"};
  for (int i = 0; i < 4000; ++i) {
    std::vector<Label> l = {{"__name__", "m" + std::to_string(rng() % 5)}};
    for (const char* k : keys)
      if (rng() % 3) l.push_back({k, "v" + std::to_string(rng() % 12)});
    LabelSet ls = mk(l);
    if (idx.lookup(ls) != kInvalidSeries) continue;  // dedup like the engine does
    SeriesId id = idx.insert(ls);
    CHECK(id == all.size());
    all.push_back(ls);
    CHECK(idx.lookup(ls) == id);
  }
  CHECK(idx.seriesCount() == all.size());
  for (int q = 0; q < 1500; ++q) {
    std::vector<Matcher> ms;
    const int k = 1 + rng() % 4;
    for (int i = 0; i < k; ++i) {
      const std::string name = (rng() % 6 == 0) ? "missing" : (rng() % 5 == 0 ? "__name__" : keys[rng() % 4]);
      const std::string val = (name == "__name__" ? "m" : "v") + std::to_string(rng() % 14);
      switch (rng() % 4) {
        case 0: ms.push_back(Matcher::eq(name, val)); break;
        case 1: ms.push_back(Matcher::neq(name, val)); break;
        case 2: ms.push_back(Matcher::prefix(name, val.substr(0, 1 + rng() % 2))); break;
        default: ms.push_back(Matcher::exists(name)); break;
      }
    }
    PostingList got;
    std::string err;
    CHECK(idx.select(ms, got, &err));
    PostingList want;
    for (SeriesId id = 0; id < all.size(); ++id) {
      bool ok = true;
      for (auto& m : ms) ok = ok && m.matches(all[id]);
      if (ok) want.push_back(id);
    }
    CHECK(got == want);
  }
  std::string err;
  PostingList out;
  CHECK(!idx.select({}, out, &err));
  CHECK(!idx.select({Matcher::eq("a", "")}, out, &err));
  CHECK(!idx.select({Matcher::eq("", "x")}, out, &err));
}

// ---------------------------------------------------------------- engine
static void testEngineBasics() {
  EngineConfig cfg;
  cfg.samples_per_chunk = 10;
  StorageEngine eng(cfg);
  auto ls = mk({{"__name__", "cpu"}, {"host", "h1"}});
  CHECK(eng.append(ls, -5, 1) == AppendStatus::InvalidTimestamp);
  for (int i = 0; i < 95; ++i) CHECK(eng.append(ls, 1000 + i * 100LL, i) == AppendStatus::Ok);
  CHECK(eng.append(ls, 1000 + 94 * 100LL, 1) == AppendStatus::DuplicateTimestamp);
  CHECK(eng.append(ls, 5, 1) == AppendStatus::OutOfOrder);
  CHECK(eng.appendById(123456, 1, 1) == AppendStatus::UnknownSeries);

  QueryEngine qe(eng);
  std::vector<SeriesResult> res;
  std::string err;
  CHECK(qe.query({Matcher::eq("host", "h1")}, 0, INT64_MAX, res, &err));
  CHECK(res.size() == 1 && res[0].samples.size() == 95);
  for (int i = 0; i < 95; ++i) CHECK(res[0].samples[i].ts == 1000 + i * 100LL && res[0].samples[i].value == i);
  // Inclusive bounds across sealed-chunk / head boundaries.
  for (auto [lo, hi] : std::vector<std::pair<int, int>>{{0, 0}, {9, 10}, {10, 19}, {5, 94}, {90, 94}, {94, 94}, {33, 71}}) {
    CHECK(qe.query({Matcher::eq("host", "h1")}, 1000 + lo * 100LL, 1000 + hi * 100LL, res, &err));
    CHECK(res.size() == 1 && res[0].samples.size() == size_t(hi - lo + 1) && res[0].samples.front().value == lo);
  }
  CHECK(qe.query({Matcher::eq("host", "h1")}, 0, 500, res, &err) && res.empty());   // before data
  CHECK(qe.query({Matcher::eq("host", "h1")}, 99999, 1e9, res, &err) && res.empty()); // after data
  CHECK(!qe.query({Matcher::eq("host", "h1")}, 10, 5, res, &err));                    // inverted range
  CHECK(!qe.query({}, 0, 10, res, &err));
  const EngineStats st = eng.stats();
  CHECK(st.series == 1 && st.samples_resident == 95 && st.sealed_chunks == 9);
  CHECK(st.rejected_duplicate == 1 && st.rejected_out_of_order == 1);
  CHECK(st.compressionRatio() > 1.0);

  bool threw = false;
  try { EngineConfig bad; bad.samples_per_chunk = 1; StorageEngine e(bad); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
}

static void testDownsample() {
  std::vector<Sample> in = {{0, 1}, {5, 3}, {10, NAN}, {10, 5}, {25, 7}, {26, 9}};
  std::vector<Bucket> b;
  std::string err;
  CHECK(QueryEngine::downsample(in, 0, 30, 10, AggFn::Sum, b, &err));
  CHECK(b.size() == 3 && b[0].start == 0 && b[0].value == 4 && b[1].start == 10 && b[1].value == 5 && b[2].start == 20 && b[2].value == 16);
  CHECK(QueryEngine::downsample(in, 0, 30, 10, AggFn::Avg, b, &err) && b[2].value == 8);
  CHECK(QueryEngine::downsample(in, 0, 30, 10, AggFn::Max, b, &err) && b[0].value == 3);
  CHECK(QueryEngine::downsample(in, 0, 30, 10, AggFn::Min, b, &err) && b[0].value == 1);
  CHECK(QueryEngine::downsample(in, 0, 30, 10, AggFn::Count, b, &err) && b[1].value == 1);  // NaN ignored
  CHECK(QueryEngine::downsample(in, 0, 30, 10, AggFn::Last, b, &err) && b[2].value == 9);
  CHECK(!QueryEngine::downsample(in, 0, 30, 0, AggFn::Sum, b, &err));
  CHECK(!QueryEngine::downsample(in, 30, 0, 10, AggFn::Sum, b, &err));
  CHECK(QueryEngine::downsample({}, 0, 30, 10, AggFn::Sum, b, &err) && b.empty());
}

static void testRetention() {
  EngineConfig cfg;
  cfg.samples_per_chunk = 10;
  cfg.retention_ms = 1000;
  StorageEngine eng(cfg);
  auto ls = mk({{"__name__", "t"}});
  SeriesId id;
  CHECK(eng.resolve(ls, id) == AppendStatus::Ok);
  for (int i = 0; i < 500; ++i) eng.appendById(id, i * 10LL, i);
  std::vector<Sample> out;
  CHECK(eng.readSeries(id, 0, INT64_MAX, out));
  const Timestamp newest = 499 * 10;
  CHECK(!out.empty() && out.back().ts == newest);
  CHECK(out.front().ts >= newest - 1000 - 10 * 10);  // window + <= 1 chunk of slack
  CHECK(eng.stats().chunks_dropped_by_retention > 0);
  CHECK(eng.enforceRetention(newest + 100000) > 0);
  out.clear();
  CHECK(eng.readSeries(id, 0, INT64_MAX, out) && out.empty());
  CHECK(eng.appendById(id, newest + 5, 1) == AppendStatus::Ok);   // ordering survives the sweep
  CHECK(eng.appendById(id, newest, 1) == AppendStatus::OutOfOrder);   // newest < newest+5 now
}

static void testCardinalityGuard() {
  EngineConfig cfg;
  cfg.max_distinct_values_per_label = 100;
  StorageEngine eng(cfg);
  int ok = 0, shed = 0;
  for (int i = 0; i < 1000; ++i)
    (eng.append(mk({{"__name__", "m"}, {"uid", "u" + std::to_string(i)}}), 10 + i, 1) == AppendStatus::Ok) ? ++ok : ++shed;
  CHECK(ok == 100 && shed == 900);
  CHECK(eng.append(mk({{"__name__", "m"}, {"uid", "u5"}}), 99999, 2) == AppendStatus::Ok);  // existing series still writable
  bool found = false;
  for (auto& r : eng.cardinalityReport())
    if (r.key == "uid") { found = true; CHECK(r.admitted_exact == 100); CHECK(std::fabs(double(r.attempted_estimate) - 1000.0) < 50); }
  CHECK(found);
  // Hostile label-NAME flood must not grow guard memory without bound (tracked keys are capped).
  EngineConfig c2;
  StorageEngine e2(c2);
  for (int i = 0; i < 2000; ++i) e2.append(mk({{"n" + std::to_string(i), "x"}}), 1, 1);
  CHECK(e2.cardinalityReport().size() <= 256);

  EngineConfig c3;
  c3.max_series = 5;
  StorageEngine e3(c3);
  int a = 0;
  for (int i = 0; i < 20; ++i) a += e3.append(mk({{"__name__", "m" + std::to_string(i)}}), 1, 1) == AppendStatus::Ok;
  CHECK(a == 5);
}

static void testConcurrency() {
  EngineConfig cfg;
  cfg.samples_per_chunk = 32;
  StorageEngine eng(cfg);
  constexpr int kThreads = 8, kSeries = 300, kSamples = 200;
  std::vector<LabelSet> sets;
  for (int s = 0; s < kSeries; ++s) sets.push_back(mk({{"__name__", "load"}, {"id", std::to_string(s)}}));

  // Phase 1: all threads race to register the SAME series. Double-checked locking must yield
  // exactly one id per series and identical ids across threads.
  std::vector<std::vector<SeriesId>> ids(kThreads, std::vector<SeriesId>(kSeries, kInvalidSeries));
  {
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t)
      pool.emplace_back([&, t] { for (int s = 0; s < kSeries; ++s) eng.resolve(sets[(s + t * 37) % kSeries], ids[t][(s + t * 37) % kSeries]); });
    for (auto& th : pool) th.join();
  }
  CHECK(eng.stats().series == kSeries);
  for (int s = 0; s < kSeries; ++s)
    for (int t = 1; t < kThreads; ++t) CHECK(ids[t][s] == ids[0][s]);

  // Phase 2: disjoint series per thread appended concurrently while readers query.
  std::atomic<bool> stop{false};
  std::thread reader([&] {
    QueryEngine qe(eng);
    std::vector<SeriesResult> res;
    std::string err;
    while (!stop.load()) {
      CHECK(qe.query({Matcher::eq("__name__", "load")}, 0, INT64_MAX, res, &err));
      for (auto& r : res)  // every observed prefix must be a gap-free, correctly ordered run
        for (size_t i = 0; i < r.samples.size(); ++i) { if (r.samples[i].ts != Timestamp(i) * 10) { CHECK(false); break; } }
    }
  });
  {
    std::vector<std::thread> pool;
    for (int t = 0; t < kThreads; ++t)
      pool.emplace_back([&, t] {
        for (int i = 0; i < kSamples; ++i)
          for (int s = t; s < kSeries; s += kThreads) {
            if (eng.appendById(ids[0][s], Timestamp(i) * 10, double(s * 1000 + i)) != AppendStatus::Ok) { CHECK(false); return; }
          }
      });
    for (auto& th : pool) th.join();
  }
  stop = true;
  reader.join();
  QueryEngine qe(eng);
  std::vector<SeriesResult> res;
  std::string err;
  CHECK(qe.query({Matcher::eq("__name__", "load")}, 0, INT64_MAX, res, &err));
  CHECK(res.size() == kSeries);
  for (auto& r : res) {
    CHECK(r.samples.size() == kSamples);
    const int s = std::stoi(*r.labels.find("id"));
    for (int i = 0; i < kSamples; ++i) CHECK(r.samples[i].value == double(s * 1000 + i));
  }
  CHECK(eng.stats().appended == std::uint64_t(kSeries) * kSamples);
}

int main() {
  run("bit stream round-trip + bounds", testBitStream);
  run("gorilla: special float values (NaN payload, -0, inf, denormal)", testGorillaSpecialValues);
  run("gorilla: int64 timestamp extremes & every dod bucket edge", testGorillaTimestampExtremes);
  run("gorilla: 300 randomized round-trips", testGorillaFuzz);
  run("gorilla: ordering rules & compression bound", testGorillaOrderingAndCompression);
  run("gorilla: truncation + bit-flip corruption safety", testGorillaCorruption);
  run("postings: intersect/union/difference/gallop vs std oracle", testPostings);
  run("hyperloglog: accuracy, duplicates, merge", testHll);
  run("label set: validation & canonical identity", testLabelSet);
  run("inverted index: 1500 random selectors vs brute force", testIndexFuzz);
  run("engine: append/query across chunk boundaries, error paths", testEngineBasics);
  run("query: downsampling & NaN handling", testDownsample);
  run("engine: retention + idle sweep", testRetention);
  run("engine: cardinality guard (value, series, key-flood)", testCardinalityGuard);
  run("engine: concurrent registration, ingest and reads", testConcurrency);
  std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
