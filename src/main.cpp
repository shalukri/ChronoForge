// ChronoForge demo driver: exercises every subsystem and prints measured numbers.
//   ./chronoforge [--hosts N] [--samples N] [--threads N]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "chronoforge/services/query_engine.hpp"
#include "chronoforge/services/storage_engine.hpp"
#include "simulation/workload.hpp"

using namespace cf;

namespace {

struct Stopwatch {
  using Clk = std::chrono::steady_clock;
  Clk::time_point t0 = Clk::now();
  double ms() const { return std::chrono::duration<double, std::milli>(Clk::now() - t0).count(); }
};

struct Options {
  std::size_t hosts = 2000;
  std::size_t samples = 720;  // per series: 3 h at a 15 s scrape interval
  std::size_t threads = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
};

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i];
    const auto v = static_cast<std::size_t>(std::strtoull(argv[i + 1], nullptr, 10));
    if (k == "--hosts") o.hosts = std::max<std::size_t>(1, v);
    else if (k == "--samples") o.samples = std::max<std::size_t>(2, v);
    else if (k == "--threads") o.threads = std::max<std::size_t>(1, v);
    else { std::cerr << "unknown flag " << k << "\n"; std::exit(2); }
  }
  return o;
}

LabelSet mk(std::vector<Label> l) {
  std::string err;
  auto ls = LabelSet::create(std::move(l), &err);
  if (!ls) { std::cerr << "bad labels: " << err << "\n"; std::exit(1); }
  return *ls;
}

void banner(const std::string& t) { std::cout << "\n=== " << t << " ===\n"; }

// ------------------------------------------------------------------ 1. codec
void codecBenchmark() {
  banner("1. Gorilla codec: compression & speed by workload shape (120-sample chunks)");
  std::cout << std::left << std::setw(38) << "shape" << std::right << std::setw(12) << "bytes/sample"
            << std::setw(10) << "ratio" << std::setw(12) << "enc ns/s" << std::setw(12) << "dec ns/s" << "  lossless\n";
  constexpr int kChunks = 4000, kPer = 120;
  for (sim::Shape shape : {sim::Shape::Constant, sim::Shape::Counter, sim::Shape::Gauge, sim::Shape::Noise}) {
    std::vector<std::vector<Sample>> data(kChunks);
    for (int c = 0; c < kChunks; ++c) {
      sim::ValueSource vs(shape, 1000 + c);
      sim::Clock clk(1'700'000'000'000LL, 15'000, 77 + c);
      for (int i = 0; i < kPer; ++i) data[c].push_back({clk.at(i), vs.next()});
    }
    std::vector<GorillaChunk> chunks(kChunks);
    Stopwatch enc;
    for (int c = 0; c < kChunks; ++c)
      for (const Sample& s : data[c]) chunks[c].append(s.ts, s.value);
    const double enc_ms = enc.ms();

    std::size_t bytes = 0;
    for (auto& ch : chunks) { ch.seal(); bytes += ch.encodedBytes(); }

    Stopwatch dec;
    bool ok = true;
    std::uint64_t sink = 0;
    for (int c = 0; c < kChunks; ++c) {
      auto it = chunks[c].iterator();
      Sample s;
      int i = 0;
      while (it.next(s)) {
        ok &= (s.ts == data[c][i].ts) && (std::bit_cast<std::uint64_t>(s.value) == std::bit_cast<std::uint64_t>(data[c][i].value));
        sink += static_cast<std::uint64_t>(s.ts);
        ++i;
      }
      ok &= (i == kPer);
    }
    const double dec_ms = dec.ms();
    const double n = double(kChunks) * kPer;
    std::cout << std::left << std::setw(38) << sim::name(shape) << std::right << std::fixed << std::setprecision(2)
              << std::setw(12) << bytes / n << std::setw(9) << 16.0 * n / bytes << "x" << std::setprecision(0)
              << std::setw(12) << enc_ms * 1e6 / n << std::setw(12) << dec_ms * 1e6 / n << "  "
              << (ok ? "yes" : "NO!") << (sink == 1 ? " " : "") << "\n";
  }
}

// ------------------------------------------------------------------ 2. fleet
struct FleetSeries {
  LabelSet labels;
  SeriesId id = kInvalidSeries;
  sim::ValueSource src;
  sim::Clock clk;
};

std::vector<FleetSeries> buildFleet(std::size_t hosts) {
  static const char* regions[] = {"us-east", "us-west", "eu-west", "ap-south"};
  struct M { const char* name; sim::Shape shape; };
  static const M metrics[] = {{"cpu_util", sim::Shape::Gauge}, {"mem_util", sim::Shape::Gauge},
                              {"requests_total", sim::Shape::Counter}, {"errors_total", sim::Shape::Counter},
                              {"up", sim::Shape::Constant}};
  std::vector<FleetSeries> fleet;
  fleet.reserve(hosts * 5);
  for (std::size_t h = 0; h < hosts; ++h) {
    char host[32];
    std::snprintf(host, sizeof host, "host-%05zu", h);
    for (const M& m : metrics) {
      const std::uint64_t seed = h * 31 + fleet.size();
      fleet.push_back(FleetSeries{
          mk({{"__name__", m.name}, {"host", host}, {"region", regions[h % 4]}, {"service", "svc-" + std::to_string(h % 8)}}),
          kInvalidSeries, sim::ValueSource(m.shape, seed), sim::Clock(1'700'000'000'000LL, 15'000, seed ^ 0xABCDEF)});
    }
  }
  return fleet;
}

// Returns wall ms for the append phase.
double ingest(StorageEngine& eng, std::vector<FleetSeries>& fleet, std::size_t samples, std::size_t threads) {
  std::vector<std::thread> pool;
  Stopwatch sw;
  for (std::size_t t = 0; t < threads; ++t) {
    pool.emplace_back([&, t] {
      // Thread t owns series {i : i % threads == t}. Per-series timestamps stay monotonic because
      // each series is touched by exactly one thread, in step order.
      for (std::size_t step = 0; step < samples; ++step)
        for (std::size_t i = t; i < fleet.size(); i += threads) {
          FleetSeries& fs = fleet[i];
          const AppendStatus st = eng.appendById(fs.id, fs.clk.at(step), fs.src.next());
          if (st != AppendStatus::Ok) { std::cerr << "append failed: " << toString(st) << "\n"; std::exit(1); }
        }
    });
  }
  for (auto& th : pool) th.join();
  return sw.ms();
}

void fleetDemo(const Options& opt, StorageEngine& eng, std::vector<FleetSeries>& fleet_out) {
  banner("2. Fleet ingestion (" + std::to_string(opt.hosts) + " hosts x 5 metrics, " + std::to_string(opt.samples) + " samples each)");
  fleet_out = buildFleet(opt.hosts);

  Stopwatch reg;
  for (FleetSeries& fs : fleet_out) {
    if (eng.resolve(fs.labels, fs.id) != AppendStatus::Ok) { std::cerr << "resolve failed\n"; std::exit(1); }
  }
  const double reg_ms = reg.ms();
  std::cout << "series registration : " << fleet_out.size() << " series in " << std::fixed << std::setprecision(1) << reg_ms
            << " ms (" << std::setprecision(2) << reg_ms * 1000.0 / fleet_out.size() << " us/series, index write-lock path)\n";

  const double total = double(fleet_out.size()) * opt.samples;

  // Baseline: identical workload on 1 thread (separate engine) to expose the parallel speedup.
  if (opt.threads > 1) {
    StorageEngine base(eng.config());
    auto f2 = buildFleet(opt.hosts);
    for (auto& fs : f2) base.resolve(fs.labels, fs.id);
    const double ms = ingest(base, f2, opt.samples, 1);
    std::cout << "ingest, 1 thread    : " << std::setprecision(2) << total / ms / 1000.0 << " M samples/s  (" << std::setprecision(0) << ms << " ms)\n";
  }
  const double ms = ingest(eng, fleet_out, opt.samples, opt.threads);
  std::cout << "ingest, " << opt.threads << " thread" << (opt.threads > 1 ? "s" : " ") << "   : " << std::setprecision(2) << total / ms / 1000.0
            << " M samples/s  (" << std::setprecision(0) << ms << " ms)   [hardware threads: " << std::thread::hardware_concurrency()
            << "; timing includes workload generation]\n";

  const EngineStats st = eng.stats();
  std::cout << "series=" << st.series << "  samples=" << st.samples_resident << "  sealed_chunks=" << st.sealed_chunks << "\n"
            << std::setprecision(2) << "payload: " << st.encoded_bytes / 1048576.0 << " MiB vs " << st.samples_resident * 16 / 1048576.0
            << " MiB raw  ->  " << st.bytesPerSample() << " B/sample, " << st.compressionRatio() << "x compression (fleet mix)\n"
            << "inverted index (approx): " << st.index_bytes / 1048576.0 << " MiB for " << st.series << " series\n";
}

// ------------------------------------------------------------------ 3. queries
void queryDemo(StorageEngine& eng, const Options& opt, const std::vector<FleetSeries>& fleet) {
  banner("3. Label queries: inverted index vs brute-force scan");
  QueryEngine qe(eng);
  const Timestamp t0 = 1'700'000'000'000LL, t1 = t0 + Timestamp(opt.samples) * 15'000;

  struct Q { const char* title; std::vector<Matcher> m; };
  const std::vector<Q> queries = {
      {"cpu_util in us-east",                         {Matcher::eq("__name__", "cpu_util"), Matcher::eq("region", "us-east")}},
      {"requests_total, svc-3, NOT eu-west",          {Matcher::eq("__name__", "requests_total"), Matcher::eq("service", "svc-3"), Matcher::neq("region", "eu-west")}},
      {"host prefix 'host-0001', metric=up",          {Matcher::prefix("host", "host-0001"), Matcher::eq("__name__", "up")}},
      {"selective: one host, any metric",             {Matcher::eq("host", "host-00042")}},
      {"negative-only: everything except 'up'",       {Matcher::neq("__name__", "up")}},
  };
  std::cout << std::left << std::setw(40) << "query" << std::right << std::setw(9) << "series" << std::setw(12) << "samples"
            << std::setw(12) << "index ms" << std::setw(12) << "scan ms" << std::setw(10) << "speedup\n";
  for (const Q& q : queries) {
    std::string err;
    // Both paths MATERIALIZE their result (copy of each matching LabelSet), so the comparison is
    // apples-to-apples. Best-of-20 removes scheduler noise from these sub-millisecond timings.
    constexpr int kReps = 20;
    double idx_ms = 1e18, scan_ms = 1e18;
    std::vector<SeriesRef> refs;
    std::size_t brute = 0;
    for (int rep = 0; rep < kReps; ++rep) {
      Stopwatch a;
      eng.selectSeries(q.m, refs, &err);
      idx_ms = std::min(idx_ms, a.ms());

      Stopwatch b;
      std::vector<LabelSet> hits;
      for (const FleetSeries& fs : fleet) {
        bool all = true;
        for (const Matcher& m : q.m) if (!m.matches(fs.labels)) { all = false; break; }
        if (all) hits.push_back(fs.labels);
      }
      scan_ms = std::min(scan_ms, b.ms());
      brute = hits.size();
    }
    if (brute != refs.size()) { std::cerr << "MISMATCH index vs brute force!\n"; std::exit(1); }

    std::vector<SeriesResult> res;
    qe.query(q.m, t0, t1, res, &err);
    std::size_t n = 0;
    for (auto& r : res) n += r.samples.size();
    std::cout << std::left << std::setw(40) << q.title << std::right << std::setw(9) << refs.size() << std::setw(12) << n
              << std::fixed << std::setprecision(3) << std::setw(12) << idx_ms << std::setw(12) << scan_ms << std::setprecision(1)
              << std::setw(9) << (idx_ms > 0 ? scan_ms / idx_ms : 0.0) << "x\n";
  }

  std::vector<SeriesResult> res;
  std::string err;
  qe.query({Matcher::eq("__name__", "cpu_util"), Matcher::eq("host", "host-00007")}, t0, t1, res, &err);
  if (!res.empty()) {
    std::vector<Bucket> b;
    QueryEngine::downsample(res[0].samples, t0, t1, 15 * 60 * 1000, AggFn::Avg, b, &err);
    std::cout << "\ndownsample " << res[0].labels.toString() << " -> 15-min avg (" << b.size() << " buckets), first 3:\n";
    for (std::size_t i = 0; i < std::min<std::size_t>(3, b.size()); ++i)
      std::cout << "  t=+" << (b[i].start - t0) / 60000 << "min  avg=" << std::setprecision(2) << b[i].value << "  (n=" << b[i].samples << ")\n";
  }
}

// ------------------------------------------------------------------ 4. cardinality
void cardinalityDemo() {
  banner("4. Cardinality explosion defence (rogue client puts user_id in a label)");
  EngineConfig cfg;
  cfg.max_distinct_values_per_label = 50'000;
  StorageEngine eng(cfg);
  constexpr int kAttempts = 200'000;
  Stopwatch sw;
  int ok = 0, rejected = 0;
  for (int i = 0; i < kAttempts; ++i) {
    auto ls = mk({{"__name__", "http_requests_total"}, {"service", "checkout"}, {"user_id", "u" + std::to_string(i)}});
    (eng.append(ls, 1000 + i, 1.0) == AppendStatus::Ok) ? ++ok : ++rejected;
  }
  // Existing series must keep working while NEW user_ids are shed.
  auto known = mk({{"__name__", "http_requests_total"}, {"service", "checkout"}, {"user_id", "u123"}});
  const AppendStatus s_known = eng.append(known, 999'999'999, 2.0);
  std::cout << "attempted " << kAttempts << " unique user_id series in " << std::fixed << std::setprecision(0) << sw.ms() << " ms: admitted "
            << ok << ", shed " << rejected << "\n"
            << "append to an already-admitted series afterwards: " << toString(s_known) << "  (graceful degradation)\n"
            << std::left << std::setw(14) << "label key" << std::right << std::setw(20) << "attempted (HLL)" << std::setw(18) << "admitted (exact)\n";
  for (const auto& r : eng.cardinalityReport())
    std::cout << std::left << std::setw(14) << r.key << std::right << std::setw(20) << r.attempted_estimate << std::setw(18) << r.admitted_exact << "\n";
  std::cout << "HLL memory per label key: " << HyperLogLog<12>::memoryBytes() << " B, expected std error "
            << std::setprecision(2) << HyperLogLog<12>::standardError() * 100 << "%  (true attempted: " << kAttempts << ")\n";
}

// ------------------------------------------------------------------ 5. retention
void retentionDemo() {
  banner("5. Chunk-granular retention (10 min window, 4 h of data, 30-sample chunks)");
  EngineConfig cfg;
  cfg.samples_per_chunk = 30;
  cfg.retention_ms = 10 * 60 * 1000;
  StorageEngine eng(cfg);
  auto ls = mk({{"__name__", "temp"}, {"sensor", "s1"}});
  SeriesId id;
  eng.resolve(ls, id);
  const Timestamp interval = 15'000;
  const int n = 4 * 3600 / 15;
  for (int i = 0; i < n; ++i) eng.appendById(id, 1000 + i * interval, 20.0 + (i % 7));
  std::vector<Sample> all;
  eng.readSeries(id, 0, INT64_MAX, all);
  const EngineStats st = eng.stats();
  std::cout << "ingested " << n << " samples; resident " << st.samples_resident << " in " << st.sealed_chunks << " sealed chunk(s); dropped "
            << st.chunks_dropped_by_retention << " chunks\n"
            << "oldest retained sample is " << (1000 + (n - 1) * interval - all.front().ts) / 60000.0 << " min behind the newest "
            << "(window 10 min + up to one 7.5-min chunk of slack, since only whole chunks are dropped)\n";
  const std::size_t dropped = eng.enforceRetention(1000 + Timestamp(n - 1) * interval + 3600'000);
  std::cout << "idle sweep one hour later dropped " << dropped << " more chunk(s); resident now " << eng.stats().samples_resident << " samples\n";
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);
  std::cout << "ChronoForge -- high-cardinality time-series engine (Gorilla codec + inverted index + HLL guard)\n";
  codecBenchmark();

  EngineConfig cfg;
  StorageEngine eng(cfg);
  std::vector<FleetSeries> fleet;
  fleetDemo(opt, eng, fleet);
  queryDemo(eng, opt, fleet);
  cardinalityDemo();
  retentionDemo();
  std::cout << "\nDone.\n";
  return 0;
}
