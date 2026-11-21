#include "chronoforge/services/storage_engine.hpp"

#include <algorithm>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace cf {

const char* toString(AppendStatus s) {
  switch (s) {
    case AppendStatus::Ok: return "ok";
    case AppendStatus::OutOfOrder: return "out_of_order";
    case AppendStatus::DuplicateTimestamp: return "duplicate_timestamp";
    case AppendStatus::InvalidTimestamp: return "invalid_timestamp";
    case AppendStatus::CardinalityLimit: return "cardinality_limit";
    case AppendStatus::UnknownSeries: return "unknown_series";
  }
  return "?";
}

namespace {

// Per-series storage: immutable sealed chunks + one appendable head chunk.
struct SeriesData {
  // WHY std::deque and not std::vector for sealed chunks:
  //   * Retention pops from the FRONT. vector::erase(begin) is O(C) and moves every chunk;
  //     deque::pop_front is O(1).
  //   * We still need binary search over chunks by time -> deque has random-access iterators,
  //     so std::lower_bound stays O(log C).
  //   * push_back never relocates existing chunks (a vector would move every chunk's buffer
  //     pointer-triple on growth).
  std::deque<GorillaChunk> sealed;
  GorillaChunk head;
  bool has_last = false;
  Timestamp last_ts = 0;  // survives chunk seal & head reset => ordering check is global per series
};

}  // namespace

// alignas(64): each shard's mutex + map header start on their own cache line. Without it,
// neighbouring shards share a line and threads working on DIFFERENT shards still invalidate
// each other's caches (false sharing), erasing the benefit of striping.
struct alignas(64) StorageEngine::Shard {
  mutable std::mutex mu;
  std::unordered_map<SeriesId, SeriesData> series;
};

StorageEngine::StorageEngine(EngineConfig cfg)
    : cfg_(cfg), guard_(CardinalityGuard::Limits{cfg.max_series, cfg.max_distinct_values_per_label}) {
  if (cfg_.shard_count == 0 || cfg_.shard_count > 4096) throw std::invalid_argument("shard_count must be in [1,4096]");
  if (cfg_.samples_per_chunk < 2 || cfg_.samples_per_chunk > 65535)
    throw std::invalid_argument("samples_per_chunk must be in [2,65535]");
  if (cfg_.retention_ms < 0) throw std::invalid_argument("retention_ms must be >= 0");
  shards_.reserve(cfg_.shard_count);
  for (std::size_t i = 0; i < cfg_.shard_count; ++i) shards_.push_back(std::make_unique<Shard>());
}

StorageEngine::~StorageEngine() = default;

AppendStatus StorageEngine::resolve(const LabelSet& ls, SeriesId& id_out) {
  {
    std::shared_lock<std::shared_mutex> rl(index_mu_);  // fast path: concurrent readers
    id_out = index_.lookup(ls);
  }
  if (id_out != kInvalidSeries) return AppendStatus::Ok;

  std::unique_lock<std::shared_mutex> wl(index_mu_);
  // Double-checked: another thread may have registered this series between our two locks.
  // Without this re-check two racing threads would create two ids for ONE series.
  id_out = index_.lookup(ls);
  if (id_out != kInvalidSeries) return AppendStatus::Ok;

  guard_.recordAttempt(ls);
  if (guard_.admit(ls, index_) != CardinalityGuard::Verdict::Admit) {
    rej_card_.fetch_add(1, std::memory_order_relaxed);
    return AppendStatus::CardinalityLimit;
  }
  id_out = index_.insert(ls);
  if (id_out == kInvalidSeries) {
    rej_card_.fetch_add(1, std::memory_order_relaxed);
    return AppendStatus::CardinalityLimit;
  }
  // Create the shard entry BEFORE releasing the index lock. Any thread that can now observe
  // the id (via lookup) is guaranteed to find its SeriesData: no "id exists, data doesn't" window.
  Shard& sh = *shards_[id_out % shards_.size()];
  std::lock_guard<std::mutex> sl(sh.mu);  // lock order: index -> shard
  sh.series.try_emplace(id_out);
  return AppendStatus::Ok;
}

AppendStatus StorageEngine::appendById(SeriesId id, Timestamp ts, double value) {
  if (ts < 0) return AppendStatus::InvalidTimestamp;
  Shard& sh = *shards_[id % shards_.size()];
  return appendLocked(sh, id, ts, value);
}

AppendStatus StorageEngine::append(const LabelSet& ls, Timestamp ts, double value) {
  if (ts < 0) return AppendStatus::InvalidTimestamp;
  SeriesId id;
  if (AppendStatus s = resolve(ls, id); s != AppendStatus::Ok) return s;
  return appendById(id, ts, value);
}

AppendStatus StorageEngine::appendLocked(Shard& sh, SeriesId id, Timestamp ts, double value) {
  std::lock_guard<std::mutex> lk(sh.mu);
  auto it = sh.series.find(id);
  if (it == sh.series.end()) return AppendStatus::UnknownSeries;
  SeriesData& s = it->second;

  if (s.has_last) {
    if (ts < s.last_ts) { rej_ooo_.fetch_add(1, std::memory_order_relaxed); return AppendStatus::OutOfOrder; }
    if (ts == s.last_ts) { rej_dup_.fetch_add(1, std::memory_order_relaxed); return AppendStatus::DuplicateTimestamp; }
  }
  s.head.append(ts, value);  // cannot fail: ordering was validated against last_ts above
  s.last_ts = ts;
  s.has_last = true;
  appended_.fetch_add(1, std::memory_order_relaxed);

  if (s.head.count() >= cfg_.samples_per_chunk) {
    s.head.seal();
    s.sealed.push_back(std::move(s.head));
    s.head = GorillaChunk{};  // reassign => all encoder state reset, moved-from state irrelevant
    if (cfg_.retention_ms > 0) {
      const Timestamp cutoff = ts - cfg_.retention_ms;  // ts >= 0, retention > 0 => no overflow
      while (!s.sealed.empty() && s.sealed.front().maxTime() < cutoff) {
        s.sealed.pop_front();
        dropped_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return AppendStatus::Ok;
}

bool StorageEngine::selectSeries(const std::vector<Matcher>& m, std::vector<SeriesRef>& out, std::string* err,
                                 std::size_t max_results) const {
  out.clear();
  std::shared_lock<std::shared_mutex> rl(index_mu_);
  PostingList ids;
  if (!index_.select(m, ids, err)) return false;
  if (max_results && ids.size() > max_results) {
    if (err) *err = "selector matches " + std::to_string(ids.size()) + " series, above the limit of " + std::to_string(max_results);
    return false;
  }
  out.reserve(ids.size());
  // Labels are COPIED under the read-lock: index_.series_ is a vector that may reallocate when a
  // writer inserts, so a reference escaping this scope could dangle.
  for (SeriesId id : ids) out.push_back(SeriesRef{id, index_.labelsOf(id)});
  return true;
}

bool StorageEngine::readSeries(SeriesId id, Timestamp from, Timestamp to, std::vector<Sample>& out) const {
  if (from > to) return true;
  const Shard& sh = *shards_[id % shards_.size()];
  std::lock_guard<std::mutex> lk(sh.mu);
  auto it = sh.series.find(id);
  if (it == sh.series.end()) return false;
  const SeriesData& s = it->second;

  auto decode = [&](const GorillaChunk& c) {
    auto iter = c.iterator();
    Sample smp;
    while (iter.next(smp)) {
      if (smp.ts > to) break;  // chunk is time-ordered: nothing further can match
      if (smp.ts >= from) out.push_back(smp);
    }
    return !iter.corrupted();
  };

  // Sealed chunks are time-ordered and non-overlapping (ingest is per-series monotonic), so the
  // first chunk that can contain data >= from is found by binary search on maxTime: O(log C),
  // and chunks entirely before `from` are never decoded.
  auto first = std::lower_bound(s.sealed.begin(), s.sealed.end(), from,
                                [](const GorillaChunk& c, Timestamp t) { return c.maxTime() < t; });
  for (; first != s.sealed.end() && first->minTime() <= to; ++first)
    if (!decode(*first)) return false;
  if (s.head.count() > 0 && s.head.maxTime() >= from && s.head.minTime() <= to) return decode(s.head);
  return true;
}

std::size_t StorageEngine::enforceRetention(Timestamp now) {
  if (cfg_.retention_ms == 0) return 0;
  const Timestamp cutoff = now - cfg_.retention_ms;
  std::size_t dropped = 0;
  for (auto& shp : shards_) {
    std::lock_guard<std::mutex> lk(shp->mu);
    for (auto& [id, s] : shp->series) {
      while (!s.sealed.empty() && s.sealed.front().maxTime() < cutoff) { s.sealed.pop_front(); ++dropped; }
      // An idle series may hold a stale head that no new append will ever seal. last_ts is kept
      // so out-of-order rejection stays correct after the reset.
      if (s.head.count() > 0 && s.head.maxTime() < cutoff) { s.head = GorillaChunk{}; ++dropped; }
    }
  }
  dropped_.fetch_add(dropped, std::memory_order_relaxed);
  return dropped;
}

EngineStats StorageEngine::stats() const {
  EngineStats st;
  {
    std::shared_lock<std::shared_mutex> rl(index_mu_);
    st.series = index_.seriesCount();
    st.index_bytes = index_.approxMemoryBytes();
  }
  for (const auto& shp : shards_) {
    std::lock_guard<std::mutex> lk(shp->mu);
    for (const auto& [id, s] : shp->series) {
      for (const GorillaChunk& c : s.sealed) {
        ++st.sealed_chunks;
        st.samples_resident += c.count();
        st.encoded_bytes += c.encodedBytes();
      }
      st.samples_resident += s.head.count();
      st.encoded_bytes += s.head.encodedBytes();
    }
  }
  st.appended = appended_.load();
  st.rejected_out_of_order = rej_ooo_.load();
  st.rejected_duplicate = rej_dup_.load();
  st.rejected_cardinality = rej_card_.load();
  st.chunks_dropped_by_retention = dropped_.load();
  return st;
}

std::vector<CardinalityGuard::KeyReport> StorageEngine::cardinalityReport() const {
  std::shared_lock<std::shared_mutex> rl(index_mu_);
  return guard_.report(index_);
}

}  // namespace cf
