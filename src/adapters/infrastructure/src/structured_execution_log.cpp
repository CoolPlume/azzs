#include "azzs/adapters/infrastructure/structured_execution_log.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::adapters::infrastructure {
namespace {

constexpr std::string_view kHeader{"AZZS-EXECUTION-LOG\t1\n"};

struct Counters final {
  std::uint64_t segment{1};
  std::uint64_t sequence{0};
  std::uint64_t correlation{0};
};

struct ParsedLog final {
  Counters counters;
  std::string_view records;
  std::string error;
};

[[nodiscard]] bool parse_uint64(std::string_view text,
                                std::uint64_t& value) noexcept {
  auto const* begin = text.data();
  auto const* end = begin + text.size();
  auto const result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] std::size_t field_count(std::string_view line) noexcept {
  return 1 + static_cast<std::size_t>(std::ranges::count(line, '\t'));
}

[[nodiscard]] std::string_view field_at(std::string_view line,
                                        std::size_t wanted) noexcept {
  std::size_t current = 0;
  std::size_t start = 0;
  while (current < wanted) {
    auto const separator = line.find('\t', start);
    if (separator == std::string_view::npos) {
      return {};
    }
    start = separator + 1;
    ++current;
  }
  auto const separator = line.find('\t', start);
  return line.substr(start, separator == std::string_view::npos
                                ? std::string_view::npos
                                : separator - start);
}

[[nodiscard]] bool valid_integer_field(std::string_view line,
                                       std::size_t index,
                                       bool optional = false) noexcept {
  auto const field = field_at(line, index);
  if (optional && field.empty()) {
    return true;
  }
  std::uint64_t ignored = 0;
  return parse_uint64(field, ignored);
}

[[nodiscard]] bool parse_int64(std::string_view text,
                               std::int64_t& value) noexcept {
  auto const* begin = text.data();
  auto const* end = begin + text.size();
  auto const result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] std::optional<std::uint64_t> correlation_number(
    std::string_view value) noexcept {
  constexpr std::string_view prefix{"correlation-"};
  if (!value.starts_with(prefix)) {
    return std::nullopt;
  }
  std::uint64_t number{};
  auto const digits = value.substr(prefix.size());
  if (digits.empty() || digits.front() == '0' ||
      !parse_uint64(digits, number) || number == 0) {
    return std::nullopt;
  }
  return number;
}

[[nodiscard]] bool one_of(std::string_view value,
                          std::initializer_list<std::string_view> allowed) {
  return std::ranges::find(allowed, value) != allowed.end();
}

[[nodiscard]] bool valid_event_token(std::string_view value) noexcept {
  if (value.empty() || value.size() > 96 ||
      !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= '0' && value.front() <= '9'))) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char byte) {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') ||
           byte == '-' || byte == '_' || byte == '.';
  });
}

[[nodiscard]] bool validate_records(std::string_view records,
                                    Counters const& counters) noexcept {
  std::uint64_t observed_sequence = 0;
  bool expect_segment_start = false;
  bool completed_boundary = false;
  std::size_t line_index = 0;
  while (!records.empty()) {
    auto const line_end = records.find('\n');
    if (line_end == std::string_view::npos) {
      return false;
    }
    auto const line = records.substr(0, line_end);
    auto const is_last = line_end + 1 == records.size();
    if (line.starts_with("EVENT\t")) {
      std::uint64_t segment{};
      std::uint64_t sequence{};
      std::int64_t error_code{};
      auto const correlation = correlation_number(field_at(line, 3));
      auto const error_field = field_at(line, 10);
      if (expect_segment_start || field_count(line) != 19 ||
          (line_index == 0 && counters.segment != 1) ||
          !parse_uint64(field_at(line, 1), segment) ||
          !parse_uint64(field_at(line, 2), sequence) ||
          segment != counters.segment ||
          observed_sequence == std::numeric_limits<std::uint64_t>::max() ||
          sequence != observed_sequence + 1 || !correlation.has_value() ||
          *correlation > counters.correlation ||
          !valid_integer_field(line, 4) ||
          (!error_field.empty() && !parse_int64(error_field, error_code)) ||
          !valid_integer_field(line, 12, true) ||
          !valid_integer_field(line, 15, true) ||
          !valid_integer_field(line, 16, true) ||
          !one_of(field_at(line, 5),
                  {"user_command", "state_transition", "adapter_result",
                   "coverage_gap"}) ||
          !one_of(field_at(line, 8),
                  {"started", "succeeded", "failed", "cancelled",
                   "unknown"}) ||
          !valid_event_token(field_at(line, 6)) ||
          !valid_event_token(field_at(line, 7)) ||
          (!field_at(line, 14).empty() &&
           !one_of(field_at(line, 14),
                   {"dropped", "truncated", "abnormal_exit",
                    "platform_unavailable", "flush_failed",
                    "permission_denied", "redaction_gap",
                    "unknown_after_last_persisted"}))) {
        return false;
      }
      observed_sequence = sequence;
    } else if (line.starts_with("CLEAR_CUTOFF\t")) {
      std::uint64_t segment{};
      std::uint64_t sequence{};
      if (field_count(line) != 4 ||
          !parse_uint64(field_at(line, 1), segment) || segment == 0 ||
          !parse_uint64(field_at(line, 2), sequence) || sequence == 0 ||
          !valid_integer_field(line, 3)) {
        return false;
      }
      if (line_index == 0 && segment != counters.segment &&
          segment != std::numeric_limits<std::uint64_t>::max() &&
          segment + 1 == counters.segment) {
        completed_boundary = true;
        expect_segment_start = true;
      } else if (is_last && segment == counters.segment &&
                 observed_sequence !=
                     std::numeric_limits<std::uint64_t>::max() &&
                 sequence == observed_sequence + 1 &&
                 sequence == counters.sequence) {
        observed_sequence = sequence;
      } else {
        return false;
      }
    } else if (line.starts_with("SEGMENT_START\t")) {
      std::uint64_t segment{};
      if (!expect_segment_start || line_index != 1 ||
          field_count(line) != 3 ||
          !parse_uint64(field_at(line, 1), segment) ||
          segment != counters.segment || !valid_integer_field(line, 2)) {
        return false;
      }
      expect_segment_start = false;
    } else {
      return false;
    }
    records.remove_prefix(line_end + 1);
    ++line_index;
  }
  return (line_index != 0 || counters.segment == 1) &&
         !expect_segment_start && observed_sequence == counters.sequence &&
         (!completed_boundary || line_index >= 2);
}

[[nodiscard]] ParsedLog parse_log(std::string_view bytes) noexcept {
  if (bytes.empty()) {
    return {};
  }
  if (!bytes.starts_with(kHeader)) {
    if (bytes.starts_with("AZZS-EXECUTION-LOG\t")) {
      return {.error = "unsupported execution log format"};
    }
    return {.error = "corrupt execution log header"};
  }

  constexpr std::string_view kCountersPrefix{"COUNTERS\t"};
  auto const counter_start = kHeader.size();
  if (!bytes.substr(counter_start).starts_with(kCountersPrefix)) {
    return {.error = "corrupt execution log counters"};
  }
  auto const line_end = bytes.find('\n', counter_start);
  if (line_end == std::string_view::npos) {
    return {.error = "corrupt execution log counters"};
  }
  auto const line = bytes.substr(counter_start + kCountersPrefix.size(),
                                 line_end - counter_start -
                                     kCountersPrefix.size());
  auto const first_tab = line.find('\t');
  auto const second_tab =
      first_tab == std::string_view::npos
          ? std::string_view::npos
          : line.find('\t', first_tab + 1);
  if (first_tab == std::string_view::npos ||
      second_tab == std::string_view::npos) {
    return {.error = "corrupt execution log counters"};
  }

  Counters counters;
  if (!parse_uint64(line.substr(0, first_tab), counters.segment) ||
      !parse_uint64(
          line.substr(first_tab + 1, second_tab - first_tab - 1),
          counters.sequence) ||
      !parse_uint64(line.substr(second_tab + 1), counters.correlation)) {
    return {.error = "corrupt execution log counters"};
  }
  if (counters.segment == 0) {
    return {.error = "corrupt execution log segment"};
  }
  auto const records = bytes.substr(line_end + 1);
  if (!validate_records(records, counters)) {
    return {.error = "corrupt execution log records"};
  }
  return {
      .counters = counters,
      .records = records,
  };
}

[[nodiscard]] std::string_view event_kind_name(
    application::ExecutionEventKind kind) noexcept {
  switch (kind) {
    case application::ExecutionEventKind::user_command:
      return "user_command";
    case application::ExecutionEventKind::state_transition:
      return "state_transition";
    case application::ExecutionEventKind::adapter_result:
      return "adapter_result";
    case application::ExecutionEventKind::coverage_gap:
      return "coverage_gap";
  }
  return "unknown";
}

[[nodiscard]] std::string_view execution_result_name(
    application::ExecutionResult result) noexcept {
  switch (result) {
    case application::ExecutionResult::started:
      return "started";
    case application::ExecutionResult::succeeded:
      return "succeeded";
    case application::ExecutionResult::failed:
      return "failed";
    case application::ExecutionResult::cancelled:
      return "cancelled";
    case application::ExecutionResult::unknown:
      return "unknown";
  }
  return "unknown";
}

[[nodiscard]] std::string_view coverage_gap_kind_name(
    application::CoverageGapKind kind) noexcept {
  switch (kind) {
    case application::CoverageGapKind::dropped:
      return "dropped";
    case application::CoverageGapKind::truncated:
      return "truncated";
    case application::CoverageGapKind::abnormal_exit:
      return "abnormal_exit";
    case application::CoverageGapKind::platform_unavailable:
      return "platform_unavailable";
    case application::CoverageGapKind::flush_failed:
      return "flush_failed";
    case application::CoverageGapKind::permission_denied:
      return "permission_denied";
    case application::CoverageGapKind::redaction_gap:
      return "redaction_gap";
    case application::CoverageGapKind::unknown_after_last_persisted:
      return "unknown_after_last_persisted";
  }
  return "platform_unavailable";
}

[[nodiscard]] std::string percent_encode(std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size());
  for (unsigned char const byte : value) {
    if (byte == '%' || byte < 0x20 || byte == 0x7f) {
      encoded.push_back('%');
      encoded.push_back(kHex[(byte >> 4U) & 0x0FU]);
      encoded.push_back(kHex[byte & 0x0FU]);
    } else {
      encoded.push_back(static_cast<char>(byte));
    }
  }
  return encoded;
}

[[nodiscard]] std::optional<unsigned char> hex_value(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned char>(value - '0');
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<unsigned char>(value - 'A' + 10);
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned char>(value - 'a' + 10);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> percent_decode(
    std::string_view value) noexcept {
  std::string decoded;
  decoded.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '%') {
      decoded.push_back(value[index]);
      continue;
    }
    if (index + 2 >= value.size()) {
      return std::nullopt;
    }
    auto const high = hex_value(value[index + 1]);
    auto const low = hex_value(value[index + 2]);
    if (!high.has_value() || !low.has_value()) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((*high << 4U) | *low));
    index += 2;
  }
  return decoded;
}

void replace_all(std::string& text, std::string_view needle,
                 std::string_view replacement) {
  if (needle.empty()) {
    return;
  }
  std::size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    text.replace(position, needle.size(), replacement);
    position += replacement.size();
  }
}

[[nodiscard]] std::string lower_ascii(std::string_view text) {
  std::string lowered{text};
  std::ranges::transform(lowered, lowered.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return lowered;
}

[[nodiscard]] bool is_sensitive_key(std::string_view key) {
  auto const lowered = lower_ascii(key);
  constexpr std::string_view markers[]{
      "password",      "passcode",      "verification_code",
      "otp",           "token",         "cookie",
      "authorization", "computer_name", "hostname",
      "user_name",     "username",      "device_id",
      "unique_id",     "serial",        "mac_address",
      "ip_address",    "ssid",          "bssid",
      "user_path",     "sensitive_url",
  };
  return std::ranges::any_of(markers, [&](std::string_view marker) {
    return lowered.find(marker) != std::string::npos;
  });
}

[[nodiscard]] std::string redact(
    std::string_view input,
    std::vector<std::string> const& sensitive_values) {
  std::string sanitized{input};
  for (auto const& sensitive : sensitive_values) {
    replace_all(sanitized, sensitive, "[redacted]");
  }

  static std::regex const windows_user_path{
      R"([A-Za-z]:[\\/]Users[\\/][^\\/;\r\n]+[\\/])",
      std::regex_constants::optimize};
  static std::regex const slash_user_path{
      R"((?:/Users/|/home/)[^/;\r\n]+/)",
      std::regex_constants::optimize};
  static std::regex const url{
      R"([A-Za-z][A-Za-z0-9+.-]*://[^\s]+)",
      std::regex_constants::optimize};
  static std::regex const mac{
      R"(\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b)",
      std::regex_constants::optimize};
  static std::regex const ipv4{
      R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)",
      std::regex_constants::optimize};
  static std::regex const ipv6{
      R"(\b[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{0,4}){2,7}\b)",
      std::regex_constants::optimize};
  static std::regex const ipv6_leading_compression{
      R"((^|[^0-9A-Fa-f:])::(?:[0-9A-Fa-f]{0,4}:){0,6}[0-9A-Fa-f]{1,4}(?:%[0-9A-Za-z_.-]+)?)",
      std::regex_constants::optimize};
  static std::regex const bearer{
      R"(((?:authorization\s*:\s*)?bearer\s+)[^\s,;]+)",
      std::regex_constants::icase | std::regex_constants::optimize};
  static std::regex const credential{
      R"((password|passcode|token|cookie|authorization|bearer)(\s*[:=]\s*)[^\s,;]+)",
      std::regex_constants::icase | std::regex_constants::optimize};
  static std::regex const identity_assignment{
      R"((\b(?:computer(?:[_ -]?name)?|host(?:[_ -]?name)?|user(?:[_ -]?name)?|username|login(?:[_ -]?name)?|ssid|bssid|device(?:[_ -]?(?:id|identifier|unique(?:[_ -]?(?:id|identifier))?))?|machine(?:[_ -]?(?:guid|id))?|hardware(?:[_ -]?(?:id|identifier))?|serial(?:[_ -]?(?:number|id)?)?)\s*[:=]\s*)(?:"[^"]*"|'[^']*'|[^,;\r\n]+))",
      std::regex_constants::icase | std::regex_constants::optimize};

  sanitized = std::regex_replace(sanitized, windows_user_path,
                                 "%USERPROFILE%/");
  sanitized = std::regex_replace(sanitized, slash_user_path,
                                 "%USERPROFILE%/");
  sanitized = std::regex_replace(sanitized, url, "[redacted-url]");
  sanitized = std::regex_replace(sanitized, mac, "[redacted-mac]");
  sanitized = std::regex_replace(sanitized, ipv4, "[redacted-ip]");
  sanitized = std::regex_replace(sanitized, ipv6_leading_compression,
                                 "$1[redacted-ip]");
  sanitized = std::regex_replace(sanitized, ipv6, "[redacted-ip]");
  sanitized = std::regex_replace(sanitized, bearer, "$1[redacted]");
  sanitized = std::regex_replace(sanitized, credential,
                                 "$1$2[redacted]");
  sanitized = std::regex_replace(sanitized, identity_assignment,
                                 "$1[redacted]");
  return sanitized;
}

[[nodiscard]] std::string redact_event_token(
    std::string_view value,
    std::vector<std::string> const& sensitive_values) {
  auto sanitized = redact(value, sensitive_values);
  return valid_event_token(sanitized) ? std::move(sanitized)
                                      : std::string{"redacted"};
}

[[nodiscard]] std::optional<application::ExecutionEventKind> event_kind_from(
    std::string_view value) noexcept {
  if (value == "user_command") {
    return application::ExecutionEventKind::user_command;
  }
  if (value == "state_transition") {
    return application::ExecutionEventKind::state_transition;
  }
  if (value == "adapter_result") {
    return application::ExecutionEventKind::adapter_result;
  }
  if (value == "coverage_gap") {
    return application::ExecutionEventKind::coverage_gap;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<application::ExecutionResult> execution_result_from(
    std::string_view value) noexcept {
  if (value == "started") {
    return application::ExecutionResult::started;
  }
  if (value == "succeeded") {
    return application::ExecutionResult::succeeded;
  }
  if (value == "failed") {
    return application::ExecutionResult::failed;
  }
  if (value == "cancelled") {
    return application::ExecutionResult::cancelled;
  }
  if (value == "unknown") {
    return application::ExecutionResult::unknown;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<application::CoverageGapKind> coverage_gap_from(
    std::string_view value) noexcept {
  if (value == "dropped") {
    return application::CoverageGapKind::dropped;
  }
  if (value == "truncated") {
    return application::CoverageGapKind::truncated;
  }
  if (value == "abnormal_exit") {
    return application::CoverageGapKind::abnormal_exit;
  }
  if (value == "platform_unavailable") {
    return application::CoverageGapKind::platform_unavailable;
  }
  if (value == "flush_failed") {
    return application::CoverageGapKind::flush_failed;
  }
  if (value == "permission_denied") {
    return application::CoverageGapKind::permission_denied;
  }
  if (value == "redaction_gap") {
    return application::CoverageGapKind::redaction_gap;
  }
  if (value == "unknown_after_last_persisted") {
    return application::CoverageGapKind::unknown_after_last_persisted;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<application::ExecutionLogFieldProjection>>
parse_field_projection(std::string_view encoded_fields) {
  std::vector<application::ExecutionLogFieldProjection> fields;
  while (!encoded_fields.empty()) {
    auto const separator = encoded_fields.find(';');
    if (separator == std::string_view::npos) {
      return std::nullopt;
    }
    auto const field = encoded_fields.substr(0, separator);
    encoded_fields.remove_prefix(separator + 1);
    if (field.empty()) {
      continue;
    }
    auto const equals = field.find('=');
    if (equals == std::string_view::npos) {
      return std::nullopt;
    }
    auto key = percent_decode(field.substr(0, equals));
    auto value = percent_decode(field.substr(equals + 1));
    if (!key.has_value() || !value.has_value()) {
      return std::nullopt;
    }
    fields.push_back({.key = redact(*key, {}), .value = redact(*value, {})});
  }
  return fields;
}

[[nodiscard]] std::optional<application::ExecutionLogEventProjection>
parse_event_projection(std::string_view line) {
  application::ExecutionLogEventProjection event;
  std::int64_t timestamp{};
  if (!parse_uint64(field_at(line, 1), event.segment) ||
      !parse_uint64(field_at(line, 2), event.sequence) ||
      !parse_int64(field_at(line, 4), timestamp)) {
    return std::nullopt;
  }
  auto correlation = percent_decode(field_at(line, 3));
  auto kind = event_kind_from(field_at(line, 5));
  auto result = execution_result_from(field_at(line, 8));
  auto component = percent_decode(field_at(line, 6));
  auto stage = percent_decode(field_at(line, 7));
  auto fields = parse_field_projection(field_at(line, 18));
  if (!correlation.has_value() || !kind.has_value() || !result.has_value() ||
      !component.has_value() || !stage.has_value() || !fields.has_value()) {
    return std::nullopt;
  }
  event.correlation.value = redact(*correlation, {});
  event.recorded_at_milliseconds = timestamp;
  event.kind = *kind;
  event.component = redact_event_token(*component, {});
  event.stage = redact_event_token(*stage, {});
  event.result = *result;
  event.fields = std::move(*fields);

  auto error_source = percent_decode(field_at(line, 9));
  auto error_message = percent_decode(field_at(line, 11));
  std::int64_t error_code{};
  if (!error_source.has_value() || !error_message.has_value() ||
      (!field_at(line, 10).empty() &&
       !parse_int64(field_at(line, 10), error_code))) {
    return std::nullopt;
  }
  if (!error_source->empty() || !error_message->empty() ||
      !field_at(line, 10).empty()) {
    event.error = application::ExecutionError{
        .source = redact(*error_source, {}),
        .code = error_code,
        .message = redact(*error_message, {}),
    };
  }

  std::uint64_t generation{};
  auto trusted_summary = percent_decode(field_at(line, 13));
  if (!trusted_summary.has_value() ||
      (!field_at(line, 12).empty() &&
       !parse_uint64(field_at(line, 12), generation))) {
    return std::nullopt;
  }
  if (!field_at(line, 12).empty() || !trusted_summary->empty()) {
    event.last_trusted_state = application::LastTrustedState{
        .generation = generation,
        .summary = redact(*trusted_summary, {}),
    };
  }

  auto gap_reason = percent_decode(field_at(line, 17));
  if (!gap_reason.has_value()) {
    return std::nullopt;
  }
  if (!field_at(line, 14).empty()) {
    auto gap_kind = coverage_gap_from(field_at(line, 14));
    std::uint64_t first{};
    std::uint64_t last{};
    if (!gap_kind.has_value() ||
        (!field_at(line, 15).empty() &&
         !parse_uint64(field_at(line, 15), first)) ||
        (!field_at(line, 16).empty() &&
         !parse_uint64(field_at(line, 16), last))) {
      return std::nullopt;
    }
    event.coverage_gap = application::CoverageGap{
        .kind = *gap_kind,
        .first_missing_sequence = field_at(line, 15).empty()
                                      ? std::nullopt
                                      : std::optional{first},
        .last_missing_sequence = field_at(line, 16).empty()
                                     ? std::nullopt
                                     : std::optional{last},
        .reason = redact(*gap_reason, {}),
    };
  }
  return event;
}

[[nodiscard]] std::size_t coverage_gap_count(ParsedLog const& parsed) {
  std::size_t count{};
  auto records = parsed.records;
  while (!records.empty()) {
    auto const line_end = records.find('\n');
    if (line_end == std::string_view::npos) {
      return count;
    }
    auto const line = records.substr(0, line_end);
    if (line.starts_with("EVENT\t")) {
      auto const event = parse_event_projection(line);
      if (event.has_value() && event->coverage_gap.has_value()) {
        ++count;
      }
    }
    records.remove_prefix(line_end + 1);
  }
  return count;
}

[[nodiscard]] std::uint64_t saturated_add(std::uint64_t left,
                                          std::uint64_t right) noexcept {
  return right > std::numeric_limits<std::uint64_t>::max() - left
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

[[nodiscard]] std::uint64_t persisted_noncritical_drop_count(
    ParsedLog const& parsed) {
  std::uint64_t count{};
  auto records = parsed.records;
  while (!records.empty()) {
    auto const line_end = records.find('\n');
    if (line_end == std::string_view::npos) {
      return count;
    }
    auto const line = records.substr(0, line_end);
    if (line.starts_with("EVENT\t")) {
      auto const event = parse_event_projection(line);
      if (event.has_value() && event->coverage_gap.has_value() &&
          event->coverage_gap->kind == application::CoverageGapKind::dropped &&
          event->stage == "capacity-recovery") {
        for (auto const& field : event->fields) {
          if (field.key != "noncritical_dropped_count") {
            continue;
          }
          std::uint64_t dropped{};
          if (parse_uint64(field.value, dropped)) {
            count = saturated_add(count, dropped);
          }
          break;
        }
      }
    }
    records.remove_prefix(line_end + 1);
  }
  return count;
}

[[nodiscard]] std::string serialize_fields(
    application::ExecutionEvent const& event) {
  std::string fields;
  for (auto const& field : event.fields) {
    auto const key = redact(field.key, event.sensitive_values);
    auto const value =
        field.disposition ==
                    application::DiagnosticValueDisposition::sensitive ||
                is_sensitive_key(field.key)
            ? std::string{"[redacted]"}
            : redact(field.value, event.sensitive_values);
    fields += percent_encode(key) + "=" + percent_encode(value) + ";";
  }
  return fields;
}

[[nodiscard]] bool is_critical(application::ExecutionEvent const& event) {
  return event.criticality == application::ExecutionLogCriticality::critical ||
         event.kind == application::ExecutionEventKind::state_transition ||
         event.kind == application::ExecutionEventKind::coverage_gap;
}

[[nodiscard]] std::string serialize_event(
    Counters const& counters, application::CorrelationId const& correlation,
    application::ExecutionEvent const& event,
    application::Clock const& clock) {
  auto const& sensitive = event.sensitive_values;
  return "EVENT\t" + std::to_string(counters.segment) + "\t" +
         std::to_string(counters.sequence) + "\t" +
         percent_encode(correlation.value) + "\t" +
         std::to_string(clock.now().time_since_epoch().count()) + "\t" +
         std::string{event_kind_name(event.kind)} + "\t" +
         redact_event_token(event.component, sensitive) + "\t" +
         redact_event_token(event.stage, sensitive) + "\t" +
         std::string{execution_result_name(event.result)} + "\t" +
         percent_encode(redact(event.error.has_value()
                                   ? event.error->source
                                   : std::string_view{},
                               sensitive)) +
         "\t" +
         (event.error.has_value() ? std::to_string(event.error->code)
                                  : std::string{}) +
         "\t" +
         percent_encode(redact(event.error.has_value()
                                   ? event.error->message
                                   : std::string_view{},
                               sensitive)) +
         "\t" +
         (event.last_trusted_state.has_value()
              ? std::to_string(event.last_trusted_state->generation)
              : std::string{}) +
         "\t" +
         percent_encode(redact(event.last_trusted_state.has_value()
                                   ? event.last_trusted_state->summary
                                   : std::string_view{},
                               sensitive)) +
         "\t" +
         (event.coverage_gap.has_value()
              ? std::string{coverage_gap_kind_name(event.coverage_gap->kind)}
              : std::string{}) +
         "\t" +
         (event.coverage_gap.has_value() &&
                  event.coverage_gap->first_missing_sequence.has_value()
              ? std::to_string(*event.coverage_gap->first_missing_sequence)
              : std::string{}) +
         "\t" +
         (event.coverage_gap.has_value() &&
                  event.coverage_gap->last_missing_sequence.has_value()
              ? std::to_string(*event.coverage_gap->last_missing_sequence)
              : std::string{}) +
         "\t" +
         percent_encode(redact(event.coverage_gap.has_value()
                                   ? event.coverage_gap->reason
                                   : std::string_view{},
                               sensitive)) +
         "\t" + serialize_fields(event) + "\n";
}

[[nodiscard]] application::ExecutionEvent capacity_recovery_event(
    std::uint64_t dropped_count) {
  return {
      .kind = application::ExecutionEventKind::coverage_gap,
      .component = "execution-log",
      .stage = "capacity-recovery",
      .result = application::ExecutionResult::failed,
      .coverage_gap = application::CoverageGap{
          .kind = application::CoverageGapKind::dropped,
          .reason =
              "noncritical diagnostic events were suppressed while log storage capacity was exhausted",
      },
      .criticality = application::ExecutionLogCriticality::critical,
      .fields = {
          application::DiagnosticField{
              .key = "noncritical_dropped_count",
              .value = std::to_string(dropped_count),
              .disposition = application::DiagnosticValueDisposition::retain,
          },
      },
  };
}

[[nodiscard]] std::string serialize(Counters const& counters,
                                    std::string_view records) {
  std::string bytes{kHeader};
  bytes += "COUNTERS\t" + std::to_string(counters.segment) + "\t" +
           std::to_string(counters.sequence) + "\t" +
           std::to_string(counters.correlation) + "\n";
  bytes.append(records);
  return bytes;
}

[[nodiscard]] std::string diagnostic_status(std::string_view error) {
  // Storage failures can include host-provided details. The compact status is
  // written to the diagnostic file, so it must take the same redaction path as
  // every other exported value.
  auto const sanitized = redact(error, {});
  std::string status;
  status.reserve(sanitized.size());
  for (unsigned char const byte : sanitized) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
      status.push_back(static_cast<char>(byte));
    } else if (status.empty() || status.back() != '_') {
      status.push_back('_');
    }
  }
  while (!status.empty() && status.back() == '_') {
    status.pop_back();
  }
  return status.empty() ? "unavailable" : status;
}

[[nodiscard]] std::string fnv1a64_hex(std::string_view bytes) {
  std::uint64_t fingerprint = 14'695'981'039'346'656'037ULL;
  for (unsigned char const byte : bytes) {
    fingerprint ^= byte;
    fingerprint *= 1'099'511'628'211ULL;
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(16, '0');
  for (std::size_t index = 0; index < result.size(); ++index) {
    auto const shift = static_cast<unsigned>((result.size() - index - 1) * 4);
    result[index] = kHex[(fingerprint >> shift) & 0x0fU];
  }
  return result;
}

struct PendingClear final {
  std::uint64_t segment{};
  std::uint64_t sequence{};
  std::string timestamp;
};

[[nodiscard]] std::optional<PendingClear> pending_clear(
    ParsedLog const& parsed) {
  if (parsed.records.empty()) {
    return std::nullopt;
  }
  auto records = parsed.records;
  records.remove_suffix(1);
  auto const separator = records.rfind('\n');
  auto const line = records.substr(separator == std::string_view::npos
                                       ? 0
                                       : separator + 1);
  if (!line.starts_with("CLEAR_CUTOFF\t") || field_count(line) != 4) {
    return std::nullopt;
  }
  PendingClear pending;
  if (!parse_uint64(field_at(line, 1), pending.segment) ||
      !parse_uint64(field_at(line, 2), pending.sequence) ||
      pending.segment != parsed.counters.segment ||
      pending.sequence != parsed.counters.sequence) {
    return std::nullopt;
  }
  pending.timestamp = std::string{field_at(line, 3)};
  return pending;
}

[[nodiscard]] std::optional<application::ExecutionLogClearReceipt>
recover_pending_clear(LogStorageTransaction& transaction,
                      ParsedLog const& parsed) {
  auto pending = pending_clear(parsed);
  if (!pending.has_value()) {
    return std::nullopt;
  }
  if (pending->segment == std::numeric_limits<std::uint64_t>::max()) {
    return application::ExecutionLogClearReceipt{
        .error = "execution log segment is exhausted"};
  }
  Counters const next{
      .segment = pending->segment + 1,
      .sequence = 0,
      .correlation = parsed.counters.correlation,
  };
  auto const boundary =
      "CLEAR_CUTOFF\t" + std::to_string(pending->segment) + "\t" +
      std::to_string(pending->sequence) + "\t" + pending->timestamp + "\n" +
      "SEGMENT_START\t" + std::to_string(next.segment) + "\t" +
      pending->timestamp + "\n";
  auto result = transaction.replace(serialize(next, boundary));
  return application::ExecutionLogClearReceipt{
      .cleared = result.verified,
      .cutoff_segment = result.verified ? pending->segment : 0,
      .cutoff_sequence = result.verified ? pending->sequence : 0,
      .active_segment = result.verified ? next.segment : 0,
      .error = std::move(result.error),
  };
}

}  // namespace

StructuredExecutionLog::StructuredExecutionLog(LogStorage& storage,
                                               application::Clock const& clock)
    : storage_(storage), clock_(clock) {}

application::ExecutionLogDebugModeResult
StructuredExecutionLog::set_debug_mode(bool enabled) {
  std::scoped_lock state_lock{state_mutex_};
  debug_mode_enabled_ = enabled;
  return {.status = application::ExecutionLogDebugModeStatus::applied,
          .enabled = debug_mode_enabled_};
}

application::ExecutionLogDebugModeRead StructuredExecutionLog::debug_mode()
    const {
  std::scoped_lock state_lock{state_mutex_};
  return {.available = true, .enabled = debug_mode_enabled_};
}

application::CorrelationId StructuredExecutionLog::begin_correlation() {
  auto transaction = storage_.begin_transaction();
  if (!transaction->read_error().empty()) {
    return {};
  }
  auto parsed = parse_log(transaction->bytes());
  if (!parsed.error.empty()) {
    return {};
  }
  if (auto recovered = recover_pending_clear(*transaction, parsed)) {
    if (!recovered->cleared) {
      return {};
    }
    parsed = parse_log(transaction->bytes());
    if (!parsed.error.empty()) {
      return {};
    }
  }
  auto counters = parsed.counters;
  if (counters.correlation == std::numeric_limits<std::uint64_t>::max()) {
    return {};
  }
  ++counters.correlation;
  auto const correlation =
      application::CorrelationId{"correlation-" +
                                 std::to_string(counters.correlation)};
  auto const result = transaction->replace(
      serialize(counters, parsed.records));
  if (!result.verified) {
    return {};
  }
  return correlation;
}

application::ExecutionLogReceipt StructuredExecutionLog::append(
    application::CorrelationId const& correlation,
    application::ExecutionEvent const& event) {
  if (!valid_event_token(event.component) || !valid_event_token(event.stage)) {
    return {.error = "execution event component or stage is invalid"};
  }
  std::scoped_lock state_lock{state_mutex_};
  auto transaction = storage_.begin_transaction();
  if (auto const error = transaction->read_error(); !error.empty()) {
    return {.error = std::string{error}};
  }
  auto parsed = parse_log(transaction->bytes());
  if (!parsed.error.empty()) {
    return {.error = parsed.error};
  }
  std::string recovery_warning;
  if (auto recovered = recover_pending_clear(*transaction, parsed)) {
    if (!recovered->cleared) {
      return {.error = std::move(recovered->error)};
    }
    recovery_warning = std::move(recovered->error);
    parsed = parse_log(transaction->bytes());
    if (!parsed.error.empty()) {
      return {.error = parsed.error};
    }
  }
  auto const correlation_id = correlation_number(correlation.value);
  if (!correlation_id.has_value() ||
      *correlation_id > parsed.counters.correlation) {
    return {.error = "correlation identifier is invalid"};
  }
  auto counters = parsed.counters;
  auto const recovered_dropped_count = pending_noncritical_dropped_count_;
  auto const event_is_critical = is_critical(event);
  if (counters.sequence == std::numeric_limits<std::uint64_t>::max()) {
    return {.error = "execution log sequence is exhausted"};
  }
  auto include_recovery_gap =
      recovered_dropped_count != 0 &&
      counters.sequence < std::numeric_limits<std::uint64_t>::max() - 1;
  if (recovered_dropped_count != 0 && !include_recovery_gap &&
      !event_is_critical) {
    return {.error = "execution log sequence is exhausted"};
  }

  std::string records{parsed.records};
  if (include_recovery_gap) {
    ++counters.sequence;
    records += serialize_event(counters, correlation,
                               capacity_recovery_event(recovered_dropped_count),
                               clock_);
  }
  ++counters.sequence;
  records += serialize_event(counters, correlation, event, clock_);

  auto result = transaction->replace(serialize(counters, records));
  std::string recovery_annotation_error;
  if (!result.verified &&
      result.failure == LogStorageWriteFailure::capacity_exhausted &&
      include_recovery_gap && event_is_critical) {
    // A durable transition is more valuable than a recovery annotation. Keep
    // the annotation pending and retry the transition without it.
    recovery_annotation_error = result.error;
    counters = parsed.counters;
    records = std::string{parsed.records};
    ++counters.sequence;
    records += serialize_event(counters, correlation, event, clock_);
    include_recovery_gap = false;
    result = transaction->replace(serialize(counters, records));
  }
  if (result.verified) {
    if (recovered_dropped_count == 0 || include_recovery_gap) {
      capacity_state_ = application::ExecutionLogCapacityState::available;
      pending_noncritical_dropped_count_ = 0;
    } else {
      capacity_state_ = application::ExecutionLogCapacityState::space_exhausted;
    }
    return {
        .persisted = true,
        .segment = counters.segment,
        .sequence = counters.sequence,
        .capacity_state = capacity_state_,
        .noncritical_dropped_count = recovered_dropped_count,
        .error = !recovery_annotation_error.empty()
                     ? std::move(recovery_annotation_error)
                     : (result.error.empty() ? std::move(recovery_warning)
                                             : result.error),
    };
  }
  if (result.failure == LogStorageWriteFailure::capacity_exhausted) {
    capacity_state_ = application::ExecutionLogCapacityState::space_exhausted;
    if (!is_critical(event)) {
      pending_noncritical_dropped_count_ =
          saturated_add(pending_noncritical_dropped_count_, 1);
      return {
          .suppressed = true,
          .capacity_state = capacity_state_,
          .noncritical_dropped_count = pending_noncritical_dropped_count_,
          .error = result.error,
      };
    }
  }
  return {
      .capacity_state = capacity_state_,
      .noncritical_dropped_count = pending_noncritical_dropped_count_,
      .error = result.error.empty() ? std::move(recovery_warning)
                                    : result.error,
  };
}

application::ExecutionLogSnapshot StructuredExecutionLog::snapshot() {
  std::scoped_lock state_lock{state_mutex_};
  auto transaction = storage_.begin_transaction();
  if (auto const error = transaction->read_error(); !error.empty()) {
    return {.error = std::string{error}};
  }
  auto const bytes = transaction->bytes();
  auto const parsed = parse_log(bytes);
  if (!parsed.error.empty()) {
    return {.error = parsed.error};
  }
  if (auto const pending = pending_clear(parsed)) {
    return {
        .available = true,
        .durable_bytes = bytes.size(),
        .capacity_state = capacity_state_,
        .noncritical_dropped_count = pending_noncritical_dropped_count_,
        .pending_clear = application::ExecutionLogPendingClearProjection{
            .cutoff_segment = pending->segment,
            .cutoff_sequence = pending->sequence,
        },
    };
  }

  application::ExecutionLogSnapshot result{
      .available = true,
      .active_segment = parsed.counters.segment,
      .last_sequence = parsed.counters.sequence,
      .durable_bytes = bytes.size(),
      .capacity_state = capacity_state_,
      .noncritical_dropped_count = saturated_add(
          persisted_noncritical_drop_count(parsed),
          pending_noncritical_dropped_count_),
  };
  auto records = parsed.records;
  while (!records.empty()) {
    auto const line_end = records.find('\n');
    if (line_end == std::string_view::npos) {
      return {.error = "corrupt execution log projection"};
    }
    auto const line = records.substr(0, line_end);
    if (line.starts_with("EVENT\t")) {
      auto event = parse_event_projection(line);
      if (!event.has_value()) {
        return {.error = "corrupt execution log projection"};
      }
      auto const recorded_at = application::WallClockTime{
          std::chrono::milliseconds{event->recorded_at_milliseconds}};
      if (!result.coverage_started_at.has_value()) {
        result.coverage_started_at = recorded_at;
      }
      result.coverage_ended_at = recorded_at;
      if (event->coverage_gap.has_value()) {
        ++result.coverage_gap_count;
      }
      result.events.push_back(std::move(*event));
    }
    records.remove_prefix(line_end + 1);
  }
  return result;
}

application::ExecutionLogClearReceipt StructuredExecutionLog::clear() {
  std::scoped_lock state_lock{state_mutex_};
  auto transaction = storage_.begin_transaction();
  if (auto const error = transaction->read_error(); !error.empty()) {
    return {.error = std::string{error}};
  }
  auto const parsed = parse_log(transaction->bytes());
  if (!parsed.error.empty()) {
    return {.error = parsed.error};
  }
  if (auto recovered = recover_pending_clear(*transaction, parsed)) {
    return *recovered;
  }
  auto counters = parsed.counters;
  if (counters.sequence == std::numeric_limits<std::uint64_t>::max() ||
      counters.segment == std::numeric_limits<std::uint64_t>::max()) {
    return {.error = "execution log counters are exhausted"};
  }
  auto const cutoff_segment = counters.segment;
  auto const cutoff_sequence = ++counters.sequence;
  auto const timestamp =
      std::to_string(clock_.now().time_since_epoch().count());

  std::string records{parsed.records};
  records += "CLEAR_CUTOFF\t" + std::to_string(cutoff_segment) + "\t" +
             std::to_string(cutoff_sequence) + "\t" + timestamp + "\n";
  auto result = transaction->replace(serialize(counters, records));
  if (!result.verified) {
    if (result.failure == LogStorageWriteFailure::capacity_exhausted) {
      capacity_state_ = application::ExecutionLogCapacityState::space_exhausted;
    }
    return {.error = std::move(result.error)};
  }
  auto first_phase_warning = std::move(result.error);

  Counters const next{
      .segment = cutoff_segment + 1,
      .sequence = 0,
      .correlation = counters.correlation,
  };
  std::string boundary =
      "CLEAR_CUTOFF\t" + std::to_string(cutoff_segment) + "\t" +
      std::to_string(cutoff_sequence) + "\t" + timestamp + "\n" +
      "SEGMENT_START\t" + std::to_string(next.segment) + "\t" + timestamp +
      "\n";
  result = transaction->replace(serialize(next, boundary));
  if (!result.verified &&
      result.failure == LogStorageWriteFailure::capacity_exhausted) {
    capacity_state_ = application::ExecutionLogCapacityState::space_exhausted;
  }
  if (result.verified && pending_noncritical_dropped_count_ == 0) {
    capacity_state_ = application::ExecutionLogCapacityState::available;
  }
  return {
      .cleared = result.verified,
      .cutoff_segment = cutoff_segment,
      .cutoff_sequence = cutoff_sequence,
      .active_segment = result.verified ? next.segment : 0,
      .error = result.error.empty() ? std::move(first_phase_warning)
                                    : std::move(result.error),
  };
}

application::DiagnosticExportReceipt
StructuredExecutionLog::export_diagnostic(
    application::DiagnosticContext const& context) {
  std::scoped_lock state_lock{state_mutex_};
  auto transaction = storage_.begin_transaction();
  auto const storage_error = std::string{transaction->read_error()};
  auto const source_bytes = storage_error.empty()
                                ? std::string{transaction->bytes()}
                                : std::string{};
  auto const parsed = storage_error.empty()
                          ? parse_log(source_bytes)
                          : ParsedLog{.error = storage_error};
  auto source_status = parsed.error.empty()
                           ? std::string{"ok"}
                           : diagnostic_status(parsed.error);
  std::string log_bytes;
  if (!parsed.error.empty()) {
    log_bytes = "AZZS-EXECUTION-LOG-UNAVAILABLE\t1\nSTATUS\t" +
                source_status + "\n";
  } else if (auto const pending = pending_clear(parsed)) {
    source_status = "pending_clear";
    log_bytes = serialize(
        parsed.counters,
        "CLEAR_CUTOFF\t" + std::to_string(pending->segment) + "\t" +
            std::to_string(pending->sequence) + "\t" + pending->timestamp +
            "\n");
  } else {
    log_bytes = source_bytes.empty()
                    ? serialize(parsed.counters, parsed.records)
                    : source_bytes;
  }
  auto const sanitize_context = [&](std::string_view text) {
    return percent_encode(redact(text, context.sensitive_values));
  };

  std::string file{"AZZS-DIAGNOSTIC\t1\n"};
  std::vector<std::string> missing_fact_names;
  std::size_t missing_fact_count{};
  auto const missing_reason = [&](std::string_view fact,
                                  std::string_view fallback) {
    for (auto const& missing : context.missing_facts) {
      if (missing.fact == fact && !missing.reason.empty()) {
        return std::string_view{missing.reason};
      }
    }
    return fallback;
  };
  auto append_not_obtained = [&](std::string_view fact,
                                 std::string_view reason) {
    if (std::ranges::any_of(missing_fact_names,
                            [&](std::string const& value) {
                              return value == fact;
                            })) {
      return;
    }
    missing_fact_names.emplace_back(fact);
    ++missing_fact_count;
    file += "NOT_OBTAINED\t" + sanitize_context(fact) + "\t" +
            sanitize_context(reason) + "\n";
  };
  auto append_context_value = [&](std::string_view label,
                                  std::string_view fact,
                                  std::string const& value) {
    if (value.empty()) {
      append_not_obtained(
          fact, missing_reason(fact, "the diagnostic caller did not provide it"));
      return;
    }
    file += std::string{label} + "\t" + sanitize_context(value) + "\n";
  };
  auto append_diagnostic_fact = [&](std::string_view label,
                                    std::string_view fact_name,
                                    application::DiagnosticFact const& fact,
                                    std::string_view default_reason) {
    if (fact.value.empty()) {
      auto const fallback = fact.unavailable_reason.empty()
                                ? default_reason
                                : std::string_view{fact.unavailable_reason};
      append_not_obtained(fact_name, missing_reason(fact_name, fallback));
      return;
    }
    auto const value =
        fact.disposition == application::DiagnosticValueDisposition::sensitive ||
                is_sensitive_key(fact_name)
            ? std::string{"[redacted]"}
            : redact(fact.value, context.sensitive_values);
    file += std::string{label} + "\t" + percent_encode(value) + "\n";
  };
  file += "FORMAT_VERSION\t1\n";
  file += "SOURCE_LOG_STATUS\t" + source_status + "\n";
  file += "SOURCE_LOG_BYTES\t" +
          (storage_error.empty() ? std::to_string(source_bytes.size())
                                 : std::string{"NOT_OBTAINED"}) +
          "\n";
  file += "SOURCE_LOG_FINGERPRINT_FNV1A64\t" +
          (storage_error.empty() ? fnv1a64_hex(source_bytes)
                                 : std::string{"NOT_OBTAINED"}) +
          "\n";
  file += "LOG_CAPACITY_STATE\t" +
          std::string{capacity_state_ ==
                              application::ExecutionLogCapacityState::available
                          ? "available"
                          : "space_exhausted"} +
          "\n";
  auto const durable_dropped_count = parsed.error.empty()
                                         ? persisted_noncritical_drop_count(parsed)
                                         : 0;
  auto const noncritical_dropped_count =
      saturated_add(durable_dropped_count, pending_noncritical_dropped_count_);
  file += "NONCRITICAL_DROPPED_COUNT\t" +
          std::to_string(noncritical_dropped_count) + "\n";
  append_context_value("WORKBENCH_BUILD", "workbench_build",
                       context.workbench_build);
  append_context_value("RELEASE_FORM", "release_form", context.release_form);
  append_context_value("PROCESS_ARCHITECTURE", "process_architecture",
                       context.process_architecture);
  append_context_value("PACKAGE_ARCHITECTURE", "package_architecture",
                       context.package_architecture);
  append_context_value("WINDOWS_VERSION", "windows_version",
                       context.windows_version);
  append_context_value("LANGUAGE", "language", context.language);
  append_context_value("TIMEZONE", "timezone", context.timezone);
  append_diagnostic_fact(
      "FROZEN_DIRECTORY_IDENTITY", "frozen_directory_identity",
      context.frozen_directory_identity,
      "the frozen directory owner did not provide it");
  append_diagnostic_fact(
      "DIRECTORY_APPLICATION_ASSOCIATION", "directory_application_association",
      context.directory_application_association,
      "the directory application association owner did not provide it");
  append_diagnostic_fact("DIRECTORY_LOAD_RESULT", "directory_load_result",
                         context.directory_load_result,
                         "the directory load result owner did not provide it");
  append_diagnostic_fact(
      "DIRECTORY_RELEASE_RESULT", "directory_release_result",
      context.directory_release_result,
      "the directory release result owner did not provide it");
  append_diagnostic_fact("BATCH_PLAN", "batch_plan", context.batch_plan,
                         "the batch plan owner did not provide it");
  append_diagnostic_fact(
      "DEBUG_LOG_COVERAGE", "debug_log_coverage", context.debug_log_coverage,
      "the debug coverage owner did not provide it");
  if (context.coverage_started_at.has_value()) {
    file += "COVERAGE_START_UTC_MS\t" +
            std::to_string(
                context.coverage_started_at->time_since_epoch().count()) +
            "\n";
  } else {
    append_not_obtained(
        "coverage_start",
        missing_reason("coverage_start", "the diagnostic caller did not provide it"));
  }
  if (context.coverage_ended_at.has_value()) {
    file += "COVERAGE_END_UTC_MS\t" +
            std::to_string(context.coverage_ended_at->time_since_epoch().count()) +
            "\n";
  } else {
    append_not_obtained(
        "coverage_end",
        missing_reason("coverage_end", "the diagnostic caller did not provide it"));
  }
  if (parsed.error.empty()) {
    auto const counters = parsed.counters;
    file += "LAST_PERSISTED_POINT\t" + std::to_string(counters.segment) +
            "\t" + std::to_string(counters.sequence) + "\n";
  } else {
    append_not_obtained("execution_log",
                        storage_error.empty() ? parsed.error : storage_error);
  }
  if (parsed.error.empty()) {
    if (pending_clear(parsed).has_value()) {
      append_not_obtained(
          "active_log_segment",
          "clear was committed but new segment completion is unconfirmed");
    }
    if (auto const gaps = coverage_gap_count(parsed); gaps != 0) {
      append_not_obtained(
          "execution_log_coverage",
          std::to_string(gaps) + " recorded coverage gap events are present");
    }
  }
  if (pending_noncritical_dropped_count_ != 0) {
    append_not_obtained(
        "execution_log_coverage",
        std::to_string(pending_noncritical_dropped_count_) +
            " noncritical events were suppressed while storage capacity was exhausted");
  }

  for (auto const& field : context.fields) {
    auto const value =
        field.disposition ==
                    application::DiagnosticValueDisposition::sensitive ||
                is_sensitive_key(field.key)
            ? std::string{"[redacted]"}
            : redact(field.value, context.sensitive_values);
    file += "CONTEXT_FIELD\t" + sanitize_context(field.key) + "\t" +
            percent_encode(value) + "\n";
  }
  for (auto const& missing : context.missing_facts) {
    append_not_obtained(missing.fact, missing.reason);
  }

  file += "LOG_BEGIN\n";
  file += log_bytes;
  if (!file.ends_with('\n')) {
    file.push_back('\n');
  }
  file += "LOG_END\n";
  auto exported = transaction->write_diagnostic(file);
  return {
      .produced = exported.verified,
      .complete = exported.verified && missing_fact_count == 0,
      .file_count = exported.verified ? 1U : 0U,
      .missing_fact_count = exported.verified ? missing_fact_count : 0U,
      .file_name = exported.verified ? std::move(exported.file_name)
                                     : std::string{},
      .file_bytes = exported.verified ? std::move(file) : std::string{},
      .error = std::move(exported.error),
  };
}

}  // namespace azzs::adapters::infrastructure
