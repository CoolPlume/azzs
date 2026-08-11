#include "azzs/adapters/infrastructure/structured_execution_log.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>

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
  return sanitized;
}

[[nodiscard]] std::string redact_event_token(
    std::string_view value,
    std::vector<std::string> const& sensitive_values) {
  auto sanitized = redact(value, sensitive_values);
  return valid_event_token(sanitized) ? std::move(sanitized)
                                      : std::string{"redacted"};
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
  std::string status;
  status.reserve(error.size());
  for (unsigned char const byte : error) {
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

application::CorrelationId StructuredExecutionLog::begin_correlation() {
  auto transaction = storage_.begin_transaction();
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
  auto transaction = storage_.begin_transaction();
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
  if (counters.sequence == std::numeric_limits<std::uint64_t>::max()) {
    return {.error = "execution log sequence is exhausted"};
  }
  ++counters.sequence;

  std::string records{parsed.records};
  auto const& sensitive = event.sensitive_values;
  records += "EVENT\t" + std::to_string(counters.segment) + "\t" +
             std::to_string(counters.sequence) + "\t" +
             percent_encode(correlation.value) + "\t" +
             std::to_string(clock_.now().time_since_epoch().count()) + "\t" +
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
                  ? std::string{
                        coverage_gap_kind_name(event.coverage_gap->kind)}
                  : std::string{}) +
             "\t" +
             (event.coverage_gap.has_value() &&
                      event.coverage_gap->first_missing_sequence.has_value()
                  ? std::to_string(
                        *event.coverage_gap->first_missing_sequence)
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
             "\t" + serialize_fields(event) +
             "\n";

  auto const result = transaction->replace(serialize(counters, records));
  return {
      .persisted = result.verified,
      .segment = result.verified ? counters.segment : 0,
      .sequence = result.verified ? counters.sequence : 0,
      .error = result.error.empty() ? std::move(recovery_warning)
                                    : result.error,
  };
}

application::ExecutionLogClearReceipt StructuredExecutionLog::clear() {
  auto transaction = storage_.begin_transaction();
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
  auto transaction = storage_.begin_transaction();
  auto const source_bytes = std::string{transaction->bytes()};
  auto const parsed = parse_log(source_bytes);
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
  file += "FORMAT_VERSION\t1\n";
  file += "SOURCE_LOG_STATUS\t" + source_status + "\n";
  file += "SOURCE_LOG_BYTES\t" + std::to_string(source_bytes.size()) + "\n";
  file += "SOURCE_LOG_FINGERPRINT_FNV1A64\t" +
          fnv1a64_hex(source_bytes) + "\n";
  file += "WORKBENCH_BUILD\t" + sanitize_context(context.workbench_build) +
          "\n";
  file += "RELEASE_FORM\t" + sanitize_context(context.release_form) + "\n";
  file += "PROCESS_ARCHITECTURE\t" +
          sanitize_context(context.process_architecture) + "\n";
  file += "PACKAGE_ARCHITECTURE\t" +
          sanitize_context(context.package_architecture) + "\n";
  file += "WINDOWS_VERSION\t" + sanitize_context(context.windows_version) +
          "\n";
  file += "LANGUAGE\t" + sanitize_context(context.language) + "\n";
  file += "TIMEZONE\t" + sanitize_context(context.timezone) + "\n";
  file += "COVERAGE_START_UTC_MS\t" +
          std::to_string(
              context.coverage_started_at.time_since_epoch().count()) +
          "\n";
  file += "COVERAGE_END_UTC_MS\t" +
          std::to_string(context.coverage_ended_at.time_since_epoch().count()) +
          "\n";
  if (parsed.error.empty()) {
    auto const counters = parsed.counters;
    file += "LAST_PERSISTED_POINT\t" + std::to_string(counters.segment) +
            "\t" + std::to_string(counters.sequence) + "\n";
  } else {
    file += "NOT_OBTAINED\texecution_log\t" + source_status + "\n";
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
    file += "NOT_OBTAINED\t" + sanitize_context(missing.fact) + "\t" +
            sanitize_context(missing.reason) + "\n";
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
      .file_count = exported.verified ? 1U : 0U,
      .file_name = exported.verified ? std::move(exported.file_name)
                                     : std::string{},
      .file_bytes = exported.verified ? std::move(file) : std::string{},
      .error = std::move(exported.error),
  };
}

}  // namespace azzs::adapters::infrastructure
