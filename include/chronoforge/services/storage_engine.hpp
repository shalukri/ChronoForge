#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "chronoforge/algorithms/gorilla_codec.hpp"
#include "chronoforge/models/label_set.hpp"
#include "chronoforge/models/matcher.hpp"
#include "chronoforge/services/cardinality_guard.hpp"
#include "chronoforge/services/inverted_index.hpp"

namespace cf {

struct EngineConfig {
  std::size_t shard_count = 16;               // lock stripes for sample data
  std::size_t samples_per_chunk = 120;        // seal threshold (see README: chunk-size trade-off)
  Timestamp retention_ms = 0;                 // 0 = keep forever
  std::size_t max_series = 5'000'000;
  std::size_t max_distinct_values_per_label = 100'000;
};

enum class AppendStatus {
  Ok,
  OutOfOrder,         // ts < last ts of the series
  DuplicateTimestamp, // ts == last ts (idempotent retry: ignored, not an error condition per se)
  InvalidTimestamp,   // negative
  CardinalityLimit,   // new series refused by the guard
  UnknownSeries       // appendById with an id that was never registered
};
const char* toString(AppendStatus s);

struct SeriesRef {
  SeriesId id;
  LabelSet labels;
};

struct EngineStats {
  std::uint64_t series = 0;
  std::uint64_t samples_resident = 0;   // currently stored (after retention)
  std::uint64_t sealed_chunks = 0;
  std::uint64_t encoded_bytes = 0;      // compressed payload bytes (chunk bit-buffers only)
  std::uint64_t index_bytes = 0;        // approximate inverted-index footprint
  std::uint64_t appended = 0;
  std::uint64_t rejected_out_of_order = 0;
  std::uint64_t rejected_duplicate = 0;
  std::uint64_t rejected_cardinality = 0;
  std::uint64_t chunks_dropped_by_retention = 0;

  double bytesPerSample() const { return samples_resident ? double(encoded_bytes) / double(samples_resident) : 0.0; }
  double compressionRatio() const { return encoded_bytes ? 16.0 * double(samples_resident) / double(encoded_bytes) : 0.0; }
};

// Sharded, concurrent, in-memory TSDB core.
//
// Locking design (the part that matters at scale):
//   index_mu_  (shared_mutex) : protects registry + postings + guard.
//                               Readers: lookups, selectors.  Writer: only NEW series creation.
//   shard.mu   (mutex)        : protects that shard's series data. series id -> shard = id % N.
//   Lock order : index_mu_  ->  shard.mu   (never the reverse; appenders drop index_mu_ first).
//
// Why this scales: in steady state (series already exist) an append touches ONE shard mutex
// and never the global lock if the caller caches the SeriesId (resolve() once, appendById()
// forever) -- exactly how a scrape loop keeps per-target series refs. N shards => ~N-way
// parallelism, and ids are dense so id % N is a perfectly even partition.
class StorageEngine {
 public:
  explicit StorageEngine(EngineConfig cfg = {});
  ~StorageEngine();
  StorageEngine(const StorageEngine&) = delete;
  StorageEngine& operator=(const StorageEngine&) = delete;

  // Registers the series if needed (slow path: takes the index write-lock exactly once per
  // series lifetime). Returns Ok or CardinalityLimit.
  AppendStatus resolve(const LabelSet& ls, SeriesId& id_out);

  AppendStatus appendById(SeriesId id, Timestamp ts, double value);
  AppendStatus append(const LabelSet& ls, Timestamp ts, double value);  // resolve + appendById

  // Index-only: no sample decoding. `max_results` guards against selectors that match millions
  // of series (0 = unlimited).
  bool selectSeries(const std::vector<Matcher>& m, std::vector<SeriesRef>& out, std::string* err,
                    std::size_t max_results = 0) const;

  // APPENDS samples in [from, to] (inclusive) to `out`. false if id unknown or data corrupt.
  bool readSeries(SeriesId id, Timestamp from, Timestamp to, std::vector<Sample>& out) const;

  // Drops chunks wholly older than now - retention (also invoked opportunistically on seal).
  std::size_t enforceRetention(Timestamp now);

  EngineStats stats() const;
  std::vector<CardinalityGuard::KeyReport> cardinalityReport() const;
  const EngineConfig& config() const noexcept { return cfg_; }

 private:
  struct Shard;

  AppendStatus appendLocked(Shard& shard, SeriesId id, Timestamp ts, double value);

  EngineConfig cfg_;
  mutable std::shared_mutex index_mu_;
  InvertedIndex index_;
  CardinalityGuard guard_;
  std::vector<std::unique_ptr<Shard>> shards_;

  std::atomic<std::uint64_t> appended_{0}, rej_ooo_{0}, rej_dup_{0}, rej_card_{0}, dropped_{0};
};

}  // namespace cf
