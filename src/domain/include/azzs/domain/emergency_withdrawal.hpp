#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::domain::emergency_withdrawal {

using Timestamp = std::chrono::sys_time<std::chrono::milliseconds>;

enum class OperationCategory : std::uint8_t {
  software_installation = 1,
  system_optimization = 2,
  software_optimization = 3,
};

enum class NoticeAction : std::uint8_t {
  withdraw = 1,
  release = 2,
};

struct VersionRange final {
  std::string minimum;
  std::string maximum;

  [[nodiscard]] bool valid() const;
  [[nodiscard]] bool contains(std::string_view version) const;
  auto operator<=>(VersionRange const&) const = default;
};

struct WithdrawalEntry final {
  std::string stable_id;
  OperationCategory category{OperationCategory::software_installation};
  VersionRange affected_versions;
  std::string reason;
  NoticeAction action{NoticeAction::withdraw};

  [[nodiscard]] bool valid() const;
  auto operator<=>(WithdrawalEntry const&) const = default;
};

struct EmergencyWithdrawalNotice final {
  std::string source;
  std::uint64_t revision{};
  Timestamp published_at{};
  std::vector<WithdrawalEntry> entries;

  [[nodiscard]] bool valid() const;
  auto operator<=>(EmergencyWithdrawalNotice const&) const = default;
};

enum class NoticeParseError : std::uint8_t {
  none,
  missing_field,
  invalid_field,
  unknown_field,
  duplicate_entry,
  empty_notice,
  input_too_large,
};

struct NoticeParseResult final {
  NoticeParseError error{NoticeParseError::none};
  std::optional<EmergencyWithdrawalNotice> notice;
  std::string detail;

  [[nodiscard]] bool succeeded() const noexcept {
    return error == NoticeParseError::none && notice.has_value();
  }
};

// Canonical wire form is UTF-8 text with one key per line:
// source=<source>, revision=<positive integer>, published_at_ms=<integer>,
// and one or more entry=<id>|<category>|<min>|<max>|<reason>|<withdraw|release>.
// Blank lines and lines beginning with '#' are ignored. Unknown fields are
// rejected because they may change the blocking semantics.
[[nodiscard]] NoticeParseResult parse_notice(std::string_view document);

struct OperationTarget final {
  std::string stable_id;
  OperationCategory category{OperationCategory::software_installation};
  std::optional<std::string> version;

  [[nodiscard]] bool valid() const;
  auto operator<=>(OperationTarget const&) const = default;
};

[[nodiscard]] bool entry_matches(WithdrawalEntry const& entry,
                                 OperationTarget const& target);

}  // namespace azzs::domain::emergency_withdrawal
