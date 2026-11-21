#pragma once
#include <string>
#include <string_view>

#include "chronoforge/models/label_set.hpp"

namespace cf {

enum class MatchOp { Equal, NotEqual, Prefix, Exists };

// A label predicate. Semantics deliberately mirror Prometheus:
//   Equal     : label present AND == value
//   NotEqual  : label ABSENT or != value        (absence counts as "not equal")
//   Prefix    : label present AND starts with value
//   Exists    : label present
struct Matcher {
  std::string name;
  MatchOp op = MatchOp::Equal;
  std::string value;

  static Matcher eq(std::string n, std::string v) { return {std::move(n), MatchOp::Equal, std::move(v)}; }
  static Matcher neq(std::string n, std::string v) { return {std::move(n), MatchOp::NotEqual, std::move(v)}; }
  static Matcher prefix(std::string n, std::string v) { return {std::move(n), MatchOp::Prefix, std::move(v)}; }
  static Matcher exists(std::string n) { return {std::move(n), MatchOp::Exists, {}}; }

  // Reference (slow) evaluation against a concrete label set. The index never calls this on the
  // query path; it exists as the ground-truth oracle for the test-suite and the benchmark.
  bool matches(const LabelSet& ls) const {
    const std::string* v = ls.find(name);
    switch (op) {
      case MatchOp::Equal:    return v && *v == value;
      case MatchOp::NotEqual: return !v || *v != value;
      case MatchOp::Prefix:   return v && v->size() >= value.size() && v->compare(0, value.size(), value) == 0;
      case MatchOp::Exists:   return v != nullptr;
    }
    return false;
  }
};

}  // namespace cf
