#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chronoforge/models/types.hpp"

namespace cf {

struct Label {
  std::string name;
  std::string value;
};

// An immutable, canonical identity of a time series: labels sorted by name, unique names,
// validated, with a cached 64-bit hash.
//
// WHY immutable + validated at construction: every downstream structure (registry, postings,
// matchers) may then assume the invariants without re-checking on the hot path. The only way to
// obtain a LabelSet is create(), so an invalid one is unrepresentable ("parse, don't validate").
class LabelSet {
 public:
  static constexpr std::size_t kMaxLabels = 32;
  static constexpr std::size_t kMaxNameLen = 64;
  static constexpr std::size_t kMaxValueLen = 256;

  // Returns nullopt (and fills *error) if: empty, too many labels, bad name charset,
  // empty/oversized value, or duplicate label names.
  static std::optional<LabelSet> create(std::vector<Label> labels, std::string* error = nullptr);

  const std::vector<Label>& labels() const noexcept { return labels_; }
  std::uint64_t hash() const noexcept { return hash_; }
  const std::string* find(std::string_view name) const;  // O(log L) over a tiny sorted vector
  bool operator==(const LabelSet& other) const noexcept;
  std::string toString() const;

 private:
  LabelSet() = default;
  std::vector<Label> labels_;
  std::uint64_t hash_ = 0;
};

}  // namespace cf
