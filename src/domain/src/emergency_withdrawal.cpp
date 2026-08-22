#include "azzs/domain/emergency_withdrawal.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace azzs::domain::emergency_withdrawal {
namespace {

constexpr std::size_t k_max_notice_document = 4U * 1024U * 1024U;

[[nodiscard]] bool ascii_text(std::string_view value, std::size_t maximum,
                              bool allow_empty = false) noexcept {
  return (allow_empty || !value.empty()) && value.size() <= maximum &&
         std::ranges::all_of(value, [](unsigned char c) {
           return c >= 0x20U && c < 0x7fU;
         });
}

[[nodiscard]] bool version_component(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char c) {
    return c >= '0' && c <= '9';
  });
}

[[nodiscard]] std::vector<std::uint64_t> parse_version(
    std::string_view value) {
  std::vector<std::uint64_t> parts;
  std::size_t begin = 0;
  while (begin <= value.size()) {
    auto const end = value.find('.', begin);
    auto const component = value.substr(
        begin, end == std::string_view::npos ? value.size() - begin
                                             : end - begin);
    if (!version_component(component)) {
      return {};
    }
    std::uint64_t number{};
    auto const result = std::from_chars(component.data(),
                                        component.data() + component.size(),
                                        number);
    if (result.ec != std::errc{} || result.ptr != component.data() + component.size()) {
      return {};
    }
    parts.push_back(number);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return parts;
}

[[nodiscard]] int compare_versions(std::string_view left,
                                   std::string_view right) {
  auto lhs = parse_version(left);
  auto rhs = parse_version(right);
  if (lhs.empty() || rhs.empty()) {
    return 0;
  }
  auto const count = std::max(lhs.size(), rhs.size());
  for (std::size_t index = 0; index < count; ++index) {
    auto const l = index < lhs.size() ? lhs[index] : 0;
    auto const r = index < rhs.size() ? rhs[index] : 0;
    if (l < r) return -1;
    if (l > r) return 1;
  }
  return 0;
}

[[nodiscard]] std::string trim(std::string_view value) {
  auto begin = value.begin();
  auto end = value.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  return std::string(begin, end);
}

[[nodiscard]] std::optional<OperationCategory> parse_category(
    std::string_view value) noexcept {
  if (value == "software_installation") return OperationCategory::software_installation;
  if (value == "system_optimization") return OperationCategory::system_optimization;
  if (value == "software_optimization") return OperationCategory::software_optimization;
  return std::nullopt;
}

[[nodiscard]] std::optional<NoticeAction> parse_action(
    std::string_view value) noexcept {
  if (value == "withdraw") return NoticeAction::withdraw;
  if (value == "release") return NoticeAction::release;
  return std::nullopt;
}

[[nodiscard]] NoticeParseResult failure(NoticeParseError error,
                                        std::string detail) {
  return {.error = error, .detail = std::move(detail)};
}

[[nodiscard]] auto entry_order_key(WithdrawalEntry const& entry) {
  return std::tie(entry.stable_id, entry.category, entry.affected_versions.minimum,
                  entry.affected_versions.maximum, entry.action, entry.reason);
}

[[nodiscard]] bool ranges_overlap(VersionRange const& left,
                                  VersionRange const& right) {
  auto const starts_before_other_ends =
      left.minimum.empty() || right.maximum.empty() ||
      compare_versions(left.minimum, right.maximum) <= 0;
  auto const other_starts_before_ends =
      right.minimum.empty() || left.maximum.empty() ||
      compare_versions(right.minimum, left.maximum) <= 0;
  return starts_before_other_ends && other_starts_before_ends;
}

}  // namespace

bool VersionRange::valid() const {
  if (minimum.empty() && maximum.empty()) {
    return true;
  }
  if ((!minimum.empty() && parse_version(minimum).empty()) ||
      (!maximum.empty() && parse_version(maximum).empty())) {
    return false;
  }
  return minimum.empty() || maximum.empty() ||
         compare_versions(minimum, maximum) <= 0;
}

bool VersionRange::contains(std::string_view version) const {
  if (!valid() || parse_version(version).empty()) {
    return false;
  }
  return (minimum.empty() || compare_versions(version, minimum) >= 0) &&
         (maximum.empty() || compare_versions(version, maximum) <= 0);
}

bool WithdrawalEntry::valid() const {
  auto const category_value = static_cast<std::uint8_t>(category);
  auto const action_value = static_cast<std::uint8_t>(action);
  return ascii_text(stable_id, 256) && affected_versions.valid() &&
         ascii_text(reason, 4096) &&
         category_value >=
             static_cast<std::uint8_t>(OperationCategory::software_installation) &&
         category_value <=
             static_cast<std::uint8_t>(OperationCategory::software_optimization) &&
         action_value >= static_cast<std::uint8_t>(NoticeAction::withdraw) &&
         action_value <= static_cast<std::uint8_t>(NoticeAction::release);
}

bool EmergencyWithdrawalNotice::valid() const {
  if (!ascii_text(source, 256) || revision == 0 || entries.empty() ||
      published_at.time_since_epoch().count() < 0) {
    return false;
  }
  for (auto const& entry : entries) {
    if (!entry.valid()) return false;
  }
  for (std::size_t i = 0; i < entries.size(); ++i) {
    for (std::size_t j = i + 1; j < entries.size(); ++j) {
      if (entries[i].stable_id == entries[j].stable_id &&
          entries[i].category == entries[j].category &&
          (entries[i].affected_versions == entries[j].affected_versions ||
           (entries[i].action != entries[j].action &&
            ranges_overlap(entries[i].affected_versions,
                           entries[j].affected_versions)))) {
        return false;
      }
    }
  }
  return true;
}

NoticeParseResult parse_notice(std::string_view document) {
  if (document.size() > k_max_notice_document) {
    return failure(NoticeParseError::input_too_large,
                   "notification document exceeds the maximum size");
  }
  EmergencyWithdrawalNotice notice;
  bool source_seen = false;
  bool revision_seen = false;
  bool published_seen = false;
  std::size_t line_start = 0;
  while (line_start <= document.size()) {
    auto const line_end = document.find('\n', line_start);
    auto line = trim(document.substr(
        line_start, line_end == std::string_view::npos
                       ? document.size() - line_start
                       : line_end - line_start));
    if (!line.empty() && line.front() != '#') {
      auto const separator = line.find('=');
      if (separator == std::string::npos) {
        return failure(NoticeParseError::invalid_field,
                       "notification line is missing '='");
      }
      auto const key = trim(std::string_view{line}.substr(0, separator));
      auto const value = trim(std::string_view{line}.substr(separator + 1));
      if (key == "source") {
        if (source_seen || !ascii_text(value, 256)) {
          return failure(NoticeParseError::invalid_field,
                         "source is duplicated or invalid");
        }
        source_seen = true;
        notice.source = value;
      } else if (key == "revision") {
        if (revision_seen || value.empty()) {
          return failure(NoticeParseError::invalid_field,
                         "revision is duplicated or empty");
        }
        auto const result = std::from_chars(value.data(), value.data() + value.size(), notice.revision);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || notice.revision == 0) {
          return failure(NoticeParseError::invalid_field, "revision is invalid");
        }
        revision_seen = true;
      } else if (key == "published_at_ms") {
        if (published_seen || value.empty()) {
          return failure(NoticeParseError::invalid_field,
                         "published_at_ms is duplicated or empty");
        }
        std::int64_t millis{};
        auto const result = std::from_chars(value.data(), value.data() + value.size(), millis);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
          return failure(NoticeParseError::invalid_field, "published_at_ms is invalid");
        }
        notice.published_at = Timestamp{std::chrono::milliseconds{millis}};
        published_seen = true;
      } else if (key == "entry") {
        std::vector<std::string> fields;
        std::size_t begin = 0;
        while (begin <= value.size()) {
          auto const end = value.find('|', begin);
          fields.emplace_back(value.substr(
              begin, end == std::string_view::npos ? value.size() - begin
                                                   : end - begin));
          if (end == std::string_view::npos) break;
          begin = end + 1;
        }
        if (fields.size() != 6) {
          return failure(NoticeParseError::invalid_field,
                         "entry must contain six fields");
        }
        auto category = parse_category(fields[1]);
        auto action = parse_action(fields[5]);
        if (!category.has_value()) {
          return failure(NoticeParseError::unknown_field,
                         "entry category is unknown");
        }
        if (!action.has_value()) {
          return failure(NoticeParseError::unknown_field,
                         "entry action is unknown");
        }
        WithdrawalEntry entry{
            .stable_id = std::move(fields[0]),
            .category = *category,
            .affected_versions = VersionRange{std::move(fields[2]), std::move(fields[3])},
            .reason = std::move(fields[4]),
            .action = *action,
        };
        if (!entry.valid()) {
          return failure(NoticeParseError::invalid_field,
                         "entry contains an invalid field");
        }
        notice.entries.push_back(std::move(entry));
      } else {
        return failure(NoticeParseError::unknown_field,
                       "notification contains an unknown field");
      }
    }
    if (line_end == std::string_view::npos) break;
    line_start = line_end + 1;
  }
  if (!source_seen || !revision_seen || !published_seen) {
    return failure(NoticeParseError::missing_field,
                   "notification requires source, revision and published_at_ms");
  }
  if (!notice.valid()) {
    return failure(NoticeParseError::duplicate_entry,
                   "notification entries are invalid or duplicated");
  }
  std::ranges::sort(notice.entries, {}, entry_order_key);
  return {.notice = std::move(notice)};
}

bool OperationTarget::valid() const {
  auto const category_value = static_cast<std::uint8_t>(category);
  return ascii_text(stable_id, 256) &&
         category_value >=
             static_cast<std::uint8_t>(OperationCategory::software_installation) &&
         category_value <=
             static_cast<std::uint8_t>(OperationCategory::software_optimization) &&
         (!version.has_value() || !parse_version(*version).empty());
}

bool entry_matches(WithdrawalEntry const& entry,
                   OperationTarget const& target) {
  if (entry.action != NoticeAction::withdraw || !entry.valid() ||
      !target.valid() || entry.stable_id != target.stable_id ||
      entry.category != target.category) {
    return false;
  }
  if (entry.affected_versions.minimum.empty() &&
      entry.affected_versions.maximum.empty()) {
    return true;
  }
  return target.version.has_value() &&
         entry.affected_versions.contains(*target.version);
}

}  // namespace azzs::domain::emergency_withdrawal
