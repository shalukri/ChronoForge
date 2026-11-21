#include "chronoforge/models/label_set.hpp"

#include <algorithm>

#include "chronoforge/algorithms/hashing.hpp"

namespace cf {
namespace {

bool validName(const std::string& n) {
  if (n.empty() || n.size() > LabelSet::kMaxNameLen) return false;
  for (std::size_t i = 0; i < n.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(n[i]);
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    const bool digit = (c >= '0' && c <= '9');
    if (!(alpha || (digit && i > 0))) return false;  // identifiers cannot start with a digit
  }
  return true;
}

}  // namespace

std::optional<LabelSet> LabelSet::create(std::vector<Label> labels, std::string* error) {
  auto fail = [&](std::string msg) -> std::optional<LabelSet> {
    if (error) *error = std::move(msg);
    return std::nullopt;
  };
  if (labels.empty()) return fail("label set must contain at least one label");
  if (labels.size() > kMaxLabels) return fail("too many labels (max 32)");
  for (const Label& l : labels) {
    if (!validName(l.name)) return fail("invalid label name: '" + l.name + "'");
    // Empty values are rejected because "label=''" and "label absent" must be ONE identity.
    // Allowing both would create two series that every query treats as the same thing.
    if (l.value.empty()) return fail("empty value for label '" + l.name + "'");
    if (l.value.size() > kMaxValueLen) return fail("value too long for label '" + l.name + "'");
  }
  std::sort(labels.begin(), labels.end(), [](const Label& a, const Label& b) { return a.name < b.name; });
  for (std::size_t i = 1; i < labels.size(); ++i)
    if (labels[i].name == labels[i - 1].name) return fail("duplicate label name '" + labels[i].name + "'");

  LabelSet ls;
  ls.labels_ = std::move(labels);
  // Each string is hashed independently (length folded in) and chained through mix64, so
  // ("ab","c") and ("a","bc") cannot collide by concatenation. Residual 64-bit collisions are
  // resolved by full comparison in the registry, so correctness never depends on hash quality.
  std::uint64_t h = 0x9ae16a3b2f90404fULL;
  for (const Label& l : ls.labels_) {
    h = mix64(h ^ hashBytes(l.name));
    h = mix64(h ^ hashBytes(l.value, 0x5bd1e995ULL));
  }
  ls.hash_ = h;
  return ls;
}

const std::string* LabelSet::find(std::string_view name) const {
  auto it = std::lower_bound(labels_.begin(), labels_.end(), name,
                             [](const Label& l, std::string_view n) { return std::string_view(l.name) < n; });
  if (it != labels_.end() && it->name == name) return &it->value;
  return nullptr;
}

bool LabelSet::operator==(const LabelSet& o) const noexcept {
  if (hash_ != o.hash_ || labels_.size() != o.labels_.size()) return false;
  for (std::size_t i = 0; i < labels_.size(); ++i)
    if (labels_[i].name != o.labels_[i].name || labels_[i].value != o.labels_[i].value) return false;
  return true;
}

std::string LabelSet::toString() const {
  std::string s = "{";
  for (std::size_t i = 0; i < labels_.size(); ++i) {
    if (i) s += ",";
    s += labels_[i].name + "=\"" + labels_[i].value + "\"";
  }
  return s + "}";
}

}  // namespace cf
