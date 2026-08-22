#include "azzs/domain/software_optimization_catalog.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace azzs::domain::software_optimization_catalog {
namespace {

using RawValue =
    std::variant<std::string, std::uint64_t, bool, std::vector<std::string>>;

struct RawRecord final {
  std::size_t line{0};
  std::map<std::string, RawValue, std::less<>> fields;
};

struct RawDocument final {
  RawRecord file;
  std::vector<RawRecord> targets;
  std::vector<RawRecord> schemes;
  std::vector<RawRecord> options;
  std::vector<RawRecord> baselines;
};

enum class TableKind {
  file,
  target,
  scheme,
  option,
  baseline,
  ignored_display,
};

constexpr std::string_view kFileFields[]{
    "schema_version", "semantics_version", "catalog_id", "revision",
    "release_state", "default_locale"};
constexpr std::string_view kTargetFields[]{
    "id", "identity_anchor", "required_first_release", "support_mode",
    "version_min", "version_max", "install_detection_kind",
    "install_detection_definition", "version_detection_kind",
    "version_detection_definition", "installation_item_id",
    "explanation_source"};
constexpr std::string_view kSchemeFields[]{
    "id", "target_id", "required_first_release", "automation",
    "version_min", "version_max", "impact", "risk", "exit_requirement",
    "restart_requirement", "required_scheme_ids", "conflicting_scheme_ids",
    "explanation_source", "manual_emergency_explanation"};
constexpr std::string_view kOptionFields[]{
    "id", "scheme_id", "version_min", "version_max", "impact",
    "default_selected", "required", "automation", "execution_kind",
    "execution_definition", "state_detection_kind",
    "state_detection_definition", "required_option_ids",
    "conflicting_option_ids", "allowed_values", "default_value",
    "explanation_source"};
constexpr std::string_view kBaselineFields[]{
    "id", "target_id", "software_item_id", "installer_baseline_id",
    "installed_version_min", "installed_version_max"};

struct ParseDocumentResult final {
  std::optional<RawDocument> document;
  std::vector<CatalogIssue> issues;
};

[[nodiscard]] bool ascii_lower(char value) noexcept {
  return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool ascii_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1);
  }
  return value;
}

[[nodiscard]] std::string_view without_comment(
    std::string_view line) noexcept {
  bool quoted = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    auto const character = line[index];
    if (quoted && escaped) {
      escaped = false;
      continue;
    }
    if (quoted && character == '\\') {
      escaped = true;
      continue;
    }
    if (character == '"') {
      quoted = !quoted;
      continue;
    }
    if (!quoted && character == '#') {
      return line.substr(0, index);
    }
  }
  return line;
}

[[nodiscard]] bool valid_key(std::string_view value) noexcept {
  return !value.empty() && std::ranges::all_of(value, [](char character) {
           return ascii_lower(character) || ascii_digit(character) ||
                  character == '_';
         });
}

[[nodiscard]] bool ignorable_display_name(std::string_view value) noexcept {
  return value == "display" || value == "descriptions" ||
         value == "localizations" || value.ends_with(".display") ||
         value.ends_with(".descriptions") ||
         value.ends_with(".localizations");
}

[[nodiscard]] bool ignorable_display_field(
    std::string_view value) noexcept {
  return value.starts_with("display_") ||
         value.starts_with("description_") ||
         value.starts_with("explanation_") ||
         value.starts_with("localization_");
}

[[nodiscard]] bool known_field(TableKind table, std::string_view key) {
  auto contains_key = [key](auto const& fields) {
    return std::ranges::find(fields, key) != std::ranges::end(fields);
  };
  switch (table) {
    case TableKind::file:
      return contains_key(kFileFields);
    case TableKind::target:
      return contains_key(kTargetFields);
    case TableKind::scheme:
      return contains_key(kSchemeFields);
    case TableKind::option:
      return contains_key(kOptionFields);
    case TableKind::baseline:
      return contains_key(kBaselineFields);
    case TableKind::ignored_display:
      return false;
  }
  return false;
}

[[nodiscard]] std::optional<std::string> parse_string(
    std::string_view source) {
  source = trim(source);
  if (source.size() < 2 || source.front() != '"' || source.back() != '"') {
    return std::nullopt;
  }
  std::string result;
  result.reserve(source.size() - 2);
  for (std::size_t index = 1; index + 1 < source.size(); ++index) {
    auto const character = source[index];
    if (character != '\\') {
      if (character == '"' ||
          static_cast<unsigned char>(character) < 0x20U) {
        return std::nullopt;
      }
      result.push_back(character);
      continue;
    }
    if (++index + 1 >= source.size()) {
      return std::nullopt;
    }
    switch (source[index]) {
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case 't':
        result.push_back('\t');
        break;
      default:
        return std::nullopt;
    }
  }
  return result;
}

[[nodiscard]] std::optional<std::vector<std::string>> parse_string_array(
    std::string_view source) {
  source = trim(source);
  if (source.size() < 2 || source.front() != '[' || source.back() != ']') {
    return std::nullopt;
  }
  source = trim(source.substr(1, source.size() - 2));
  std::vector<std::string> result;
  while (!source.empty()) {
    if (source.front() != '"') {
      return std::nullopt;
    }
    bool escaped = false;
    std::size_t closing = std::string_view::npos;
    for (std::size_t index = 1; index < source.size(); ++index) {
      auto const character = source[index];
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        closing = index;
        break;
      }
    }
    if (closing == std::string_view::npos) {
      return std::nullopt;
    }
    auto parsed = parse_string(source.substr(0, closing + 1));
    if (!parsed.has_value()) {
      return std::nullopt;
    }
    result.push_back(std::move(*parsed));
    source = trim(source.substr(closing + 1));
    if (source.empty()) {
      break;
    }
    if (source.front() != ',') {
      return std::nullopt;
    }
    source = trim(source.substr(1));
    if (source.empty()) {
      return std::nullopt;
    }
  }
  return result;
}

[[nodiscard]] std::optional<RawValue> parse_value(std::string_view source) {
  source = trim(source);
  if (source.empty()) {
    return std::nullopt;
  }
  if (source.front() == '"') {
    auto value = parse_string(source);
    if (!value.has_value()) {
      return std::nullopt;
    }
    return RawValue{std::move(*value)};
  }
  if (source.front() == '[') {
    auto value = parse_string_array(source);
    if (!value.has_value()) {
      return std::nullopt;
    }
    return RawValue{std::move(*value)};
  }
  if (source == "true") {
    return RawValue{true};
  }
  if (source == "false") {
    return RawValue{false};
  }
  std::uint64_t number{};
  auto const [end, error] =
      std::from_chars(source.data(), source.data() + source.size(), number);
  if (error == std::errc{} && end == source.data() + source.size()) {
    return RawValue{number};
  }
  return std::nullopt;
}

void add_issue(std::vector<CatalogIssue>& issues, CatalogIssueCode code,
               std::string entity_id, std::string detail) {
  issues.push_back(CatalogIssue{
      .code = code,
      .entity_id = std::move(entity_id),
      .detail = std::move(detail),
  });
}

[[nodiscard]] ParseDocumentResult parse_document(std::string_view source) {
  RawDocument document;
  document.file.line = 1;
  RawRecord* current = &document.file;
  auto table = TableKind::file;
  std::vector<CatalogIssue> issues;
  std::size_t line_number = 0;

  while (!source.empty()) {
    ++line_number;
    auto const newline = source.find('\n');
    auto line = newline == std::string_view::npos ? source
                                                  : source.substr(0, newline);
    source = newline == std::string_view::npos ? std::string_view{}
                                               : source.substr(newline + 1);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    line = trim(without_comment(line));
    if (line.empty()) {
      continue;
    }

    if (line.starts_with('[')) {
      bool const array_table =
          line.size() >= 4 && line.starts_with("[[") && line.ends_with("]]");
      bool const normal_table = !array_table && line.size() >= 3 &&
                                line.front() == '[' && line.back() == ']';
      if (!array_table && !normal_table) {
        add_issue(issues, CatalogIssueCode::syntax_error, {},
                  "line " + std::to_string(line_number) +
                      " has an invalid table header");
        continue;
      }
      auto const name = trim(array_table
                                 ? line.substr(2, line.size() - 4)
                                 : line.substr(1, line.size() - 2));
      current = nullptr;
      if (array_table && name == "targets") {
        document.targets.push_back(RawRecord{.line = line_number});
        current = &document.targets.back();
        table = TableKind::target;
      } else if (array_table && name == "schemes") {
        document.schemes.push_back(RawRecord{.line = line_number});
        current = &document.schemes.back();
        table = TableKind::scheme;
      } else if (array_table && name == "options") {
        document.options.push_back(RawRecord{.line = line_number});
        current = &document.options.back();
        table = TableKind::option;
      } else if (array_table && name == "compatibility_baselines") {
        document.baselines.push_back(RawRecord{.line = line_number});
        current = &document.baselines.back();
        table = TableKind::baseline;
      } else if (ignorable_display_name(name)) {
        table = TableKind::ignored_display;
      } else {
        add_issue(
            issues, CatalogIssueCode::unknown_execution_semantics, {},
            "line " + std::to_string(line_number) + " uses unknown table '" +
                std::string{name} + "'");
        table = TableKind::ignored_display;
      }
      continue;
    }

    auto const equals = line.find('=');
    if (equals == std::string_view::npos) {
      add_issue(issues, CatalogIssueCode::syntax_error, {},
                "line " + std::to_string(line_number) +
                    " is not a key/value assignment");
      continue;
    }
    auto const key = trim(line.substr(0, equals));
    auto const value_source = trim(line.substr(equals + 1));
    if (!valid_key(key)) {
      add_issue(issues, CatalogIssueCode::syntax_error, {},
                "line " + std::to_string(line_number) +
                    " has an invalid key");
      continue;
    }
    if (table == TableKind::ignored_display) {
      continue;
    }
    if (!known_field(table, key)) {
      if (ignorable_display_field(key)) {
        continue;
      }
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, {},
                "line " + std::to_string(line_number) +
                    " uses unknown field '" + std::string{key} + "'");
      continue;
    }
    auto value = parse_value(value_source);
    if (!value.has_value()) {
      add_issue(issues, CatalogIssueCode::syntax_error, {},
                "line " + std::to_string(line_number) +
                    " has an unsupported or invalid value");
      continue;
    }
    if (current == nullptr) {
      add_issue(issues, CatalogIssueCode::syntax_error, {},
                "line " + std::to_string(line_number) +
                    " has no owning table");
      continue;
    }
    if (!current->fields.emplace(std::string{key}, std::move(*value)).second) {
      add_issue(issues, CatalogIssueCode::invalid_file_field, {},
                "line " + std::to_string(line_number) +
                    " repeats key '" + std::string{key} + "'");
    }
  }

  if (!issues.empty()) {
    return {.issues = std::move(issues)};
  }
  return {.document = std::move(document)};
}

template <typename Value>
[[nodiscard]] std::optional<Value> required(
    RawRecord const& record, std::string_view key, std::string_view entity,
    std::vector<CatalogIssue>& issues) {
  auto const found = record.fields.find(key);
  if (found == record.fields.end()) {
    add_issue(issues, CatalogIssueCode::missing_required_field,
              std::string{entity},
              "line " + std::to_string(record.line) + " is missing '" +
                  std::string{key} + "'");
    return std::nullopt;
  }
  auto const* value = std::get_if<Value>(&found->second);
  if (value == nullptr) {
    add_issue(issues, CatalogIssueCode::invalid_file_field,
              std::string{entity},
              "line " + std::to_string(record.line) + " field '" +
                  std::string{key} + "' has the wrong type");
    return std::nullopt;
  }
  return *value;
}

[[nodiscard]] std::optional<std::string> optional_string(
    RawRecord const& record, std::string_view key,
    std::vector<CatalogIssue>& issues, std::string_view entity) {
  auto const found = record.fields.find(key);
  if (found == record.fields.end()) {
    return std::nullopt;
  }
  auto const* value = std::get_if<std::string>(&found->second);
  if (value == nullptr) {
    add_issue(issues, CatalogIssueCode::invalid_file_field,
              std::string{entity}, "field '" + std::string{key} +
                                       "' has the wrong type");
    return std::nullopt;
  }
  return *value;
}

void reject_unknown_fields(RawRecord const& record,
                           std::span<std::string_view const> allowed,
                           std::string_view entity,
                           std::vector<CatalogIssue>& issues) {
  for (auto const& [key, value] : record.fields) {
    static_cast<void>(value);
    if (std::ranges::find(allowed, key) != allowed.end() ||
        ignorable_display_field(key)) {
      continue;
    }
    add_issue(issues, CatalogIssueCode::unknown_execution_semantics,
              std::string{entity}, "unknown field '" + key + "'");
  }
}

[[nodiscard]] std::vector<StableId> ids(
    std::vector<std::string> const& values) {
  std::vector<StableId> result;
  result.reserve(values.size());
  for (auto const& value : values) {
    result.push_back(StableId{value});
  }
  return result;
}

struct Version final {
  std::vector<std::uint32_t> parts;
};

[[nodiscard]] std::optional<Version> parse_version(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  Version result;
  while (!value.empty()) {
    auto const dot = value.find('.');
    auto const part = dot == std::string_view::npos ? value
                                                    : value.substr(0, dot);
    if (part.empty() || part.size() > 9 || result.parts.size() >= 4) {
      return std::nullopt;
    }
    std::uint32_t number{};
    auto const [end, error] =
        std::from_chars(part.data(), part.data() + part.size(), number);
    if (error != std::errc{} || end != part.data() + part.size()) {
      return std::nullopt;
    }
    result.parts.push_back(number);
    value = dot == std::string_view::npos ? std::string_view{}
                                          : value.substr(dot + 1);
  }
  while (result.parts.size() < 4) {
    result.parts.push_back(0);
  }
  return result;
}

[[nodiscard]] int compare(Version const& left, Version const& right) noexcept {
  if (left.parts < right.parts) {
    return -1;
  }
  if (left.parts > right.parts) {
    return 1;
  }
  return 0;
}

[[nodiscard]] bool valid_range(VersionRange const& range) {
  auto const minimum = parse_version(range.minimum);
  auto const maximum = parse_version(range.maximum);
  return minimum.has_value() && maximum.has_value() &&
         compare(*minimum, *maximum) <= 0;
}

[[nodiscard]] bool intersects(VersionRange const& left,
                              VersionRange const& right) {
  auto const left_minimum = parse_version(left.minimum);
  auto const left_maximum = parse_version(left.maximum);
  auto const right_minimum = parse_version(right.minimum);
  auto const right_maximum = parse_version(right.maximum);
  if (!left_minimum.has_value() || !left_maximum.has_value() ||
      !right_minimum.has_value() || !right_maximum.has_value()) {
    return false;
  }
  return compare(*left_minimum, *right_maximum) <= 0 &&
         compare(*right_minimum, *left_maximum) <= 0;
}

[[nodiscard]] bool contains_range(VersionRange const& outer,
                                  VersionRange const& inner) {
  auto const outer_minimum = parse_version(outer.minimum);
  auto const outer_maximum = parse_version(outer.maximum);
  auto const inner_minimum = parse_version(inner.minimum);
  auto const inner_maximum = parse_version(inner.maximum);
  return outer_minimum.has_value() && outer_maximum.has_value() &&
         inner_minimum.has_value() && inner_maximum.has_value() &&
         compare(*outer_minimum, *inner_minimum) <= 0 &&
         compare(*outer_maximum, *inner_maximum) >= 0;
}

[[nodiscard]] bool valid_explanation_source(std::string_view value) noexcept {
  return value.size() <= 2'048 &&
         (value.starts_with("https://") || value.starts_with("http://")) &&
         std::ranges::none_of(value, [](unsigned char character) {
           return character < 0x20U || character == 0x7fU;
         });
}

[[nodiscard]] bool valid_controlled_value(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 32 &&
         std::ranges::all_of(value, [](char character) {
           return ascii_lower(character) || ascii_digit(character) ||
                  (character >= 'A' && character <= 'Z') ||
                  character == '-' || character == '_' || character == '.';
         });
}

[[nodiscard]] std::optional<SupportMode> support_mode(
    std::string_view value) noexcept {
  if (value == "supported") {
    return SupportMode::supported;
  }
  if (value == "recognition_only") {
    return SupportMode::recognition_only;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<AutomationSupport> automation(
    std::string_view value) noexcept {
  if (value == "controlled") {
    return AutomationSupport::controlled;
  }
  if (value == "manual_only") {
    return AutomationSupport::manual_only;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RiskLevel> risk_level(
    std::string_view value) noexcept {
  if (value == "low") {
    return RiskLevel::low;
  }
  if (value == "medium") {
    return RiskLevel::medium;
  }
  if (value == "high") {
    return RiskLevel::high;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ExitRequirement> exit_requirement(
    std::string_view value) noexcept {
  if (value == "none") {
    return ExitRequirement::none;
  }
  if (value == "graceful_exit") {
    return ExitRequirement::graceful_exit;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RestartRequirement> restart_requirement(
    std::string_view value) noexcept {
  if (value == "none") {
    return RestartRequirement::none;
  }
  if (value == "explorer") {
    return RestartRequirement::explorer;
  }
  if (value == "windows") {
    return RestartRequirement::windows;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RuleKind> rule_kind(
    std::string_view value) noexcept {
  if (value == "none") {
    return RuleKind::none;
  }
  if (value == "built_in_definition") {
    return RuleKind::built_in_definition;
  }
  return std::nullopt;
}

[[nodiscard]] std::string entity_name(RawRecord const& record,
                                      std::string_view fallback) {
  auto const found = record.fields.find("id");
  if (found != record.fields.end()) {
    if (auto const* id = std::get_if<std::string>(&found->second)) {
      return *id;
    }
  }
  return std::string{fallback} + "@line-" + std::to_string(record.line);
}

[[nodiscard]] bool contains(std::vector<StableId> const& values,
                            std::string_view id) noexcept {
  return std::ranges::any_of(values, [id](StableId const& value) {
    return value.value == id;
  });
}

[[nodiscard]] bool registered_rule(
    std::span<BuiltInRuleDefinition const> definitions,
    ControlledRule const& rule, RulePurpose purpose) noexcept {
  return rule.kind == RuleKind::built_in_definition &&
         rule.definition.valid() &&
         std::ranges::any_of(
             definitions, [&](BuiltInRuleDefinition const& definition) {
               return definition.purpose == purpose &&
                      definition.id == rule.definition;
             });
}

void add_configuration_issue(SoftwareOptimizationScheme& scheme,
                             CatalogIssueCode code, std::string detail) {
  if (std::ranges::any_of(
          scheme.configuration_issues,
          [&](CatalogIssue const& issue) {
            return issue.code == code && issue.detail == detail;
          })) {
    return;
  }
  scheme.configuration_issues.push_back(CatalogIssue{
      .code = code,
      .entity_id = scheme.id.value,
      .detail = std::move(detail),
  });
  scheme.availability = SchemeAvailability::configuration_error;
}

class BehaviorFingerprint final {
 public:
  void number(std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      mix(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void text(std::string_view value) noexcept {
    number(value.size());
    for (auto const character : value) {
      mix(static_cast<unsigned char>(character));
    }
  }

  [[nodiscard]] std::string finish(std::string_view version) const {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result{version};
    result.push_back(':');
    auto append_hex = [&](std::uint64_t value) {
      for (int shift = 60; shift >= 0; shift -= 4) {
        result.push_back(kHex[(value >> shift) & 0x0fU]);
      }
    };
    append_hex(primary_);
    append_hex(secondary_);
    return result;
  }

 private:
  void mix(std::uint8_t value) noexcept {
    primary_ ^= value;
    primary_ *= 1099511628211ULL;
    secondary_ = value + (secondary_ << 6U) + (secondary_ << 16U) - secondary_;
  }

  std::uint64_t primary_{14695981039346656037ULL};
  std::uint64_t secondary_{11400714819323198485ULL};
};

void add_ids(BehaviorFingerprint& fingerprint,
             std::span<StableId const> values) {
  std::vector<std::string_view> sorted;
  sorted.reserve(values.size());
  for (auto const& value : values) {
    sorted.push_back(value.value);
  }
  std::ranges::sort(sorted);
  fingerprint.number(sorted.size());
  for (auto const value : sorted) {
    fingerprint.text(value);
  }
}

void add_strings(BehaviorFingerprint& fingerprint,
                 std::span<std::string const> values) {
  std::vector<std::string_view> sorted;
  sorted.reserve(values.size());
  for (auto const& value : values) {
    sorted.push_back(value);
  }
  std::ranges::sort(sorted);
  fingerprint.number(sorted.size());
  for (auto const value : sorted) {
    fingerprint.text(value);
  }
}

void add_rule(BehaviorFingerprint& fingerprint, ControlledRule const& rule) {
  fingerprint.number(static_cast<std::uint64_t>(rule.kind));
  fingerprint.text(rule.definition.value);
}

void add_option_behavior(BehaviorFingerprint& fingerprint,
                         SoftwareOptimizationOption const& option) {
  fingerprint.text(option.id.value);
  fingerprint.text(option.scheme_id.value);
  fingerprint.text(option.supported_versions.minimum);
  fingerprint.text(option.supported_versions.maximum);
  fingerprint.number(option.default_selected ? 1U : 0U);
  fingerprint.number(option.required ? 1U : 0U);
  fingerprint.number(static_cast<std::uint64_t>(option.automation));
  add_rule(fingerprint, option.execution);
  add_rule(fingerprint, option.state_detection);
  add_ids(fingerprint, option.required_option_ids);
  add_ids(fingerprint, option.conflicting_option_ids);
  add_strings(fingerprint, option.allowed_values);
  fingerprint.number(option.default_value.has_value() ? 1U : 0U);
  if (option.default_value.has_value()) {
    fingerprint.text(*option.default_value);
  }
}

[[nodiscard]] std::string scheme_behavior_fingerprint(
    SoftwareOptimizationScheme const& scheme) {
  BehaviorFingerprint fingerprint;
  fingerprint.text(scheme.target_id.value);
  fingerprint.number(scheme.required_first_release ? 1U : 0U);
  fingerprint.number(static_cast<std::uint64_t>(scheme.automation));
  fingerprint.text(scheme.supported_versions.minimum);
  fingerprint.text(scheme.supported_versions.maximum);
  fingerprint.number(static_cast<std::uint64_t>(scheme.risk));
  fingerprint.number(static_cast<std::uint64_t>(scheme.exit_requirement));
  fingerprint.number(static_cast<std::uint64_t>(scheme.restart_requirement));
  add_ids(fingerprint, scheme.required_scheme_ids);
  add_ids(fingerprint, scheme.conflicting_scheme_ids);

  std::vector<SoftwareOptimizationOption const*> options;
  options.reserve(scheme.options.size());
  for (auto const& option : scheme.options) {
    options.push_back(&option);
  }
  std::ranges::sort(options, {}, [](SoftwareOptimizationOption const* option) {
    return option->id.value;
  });
  fingerprint.number(options.size());
  for (auto const* option : options) {
    add_option_behavior(fingerprint, *option);
  }
  return fingerprint.finish("scheme-v2");
}

[[nodiscard]] std::string option_behavior_fingerprint(
    SoftwareOptimizationOption const& option) {
  BehaviorFingerprint fingerprint;
  add_option_behavior(fingerprint, option);
  return fingerprint.finish("option-v2");
}

[[nodiscard]] std::string identity_fingerprint(TargetSoftware const& target) {
  return target.identity_anchor.value;
}

[[nodiscard]] std::string identity_fingerprint(
    SoftwareOptimizationScheme const& scheme) {
  return scheme_behavior_fingerprint(scheme);
}

[[nodiscard]] std::string identity_fingerprint(
    SoftwareOptimizationOption const& option) {
  return option_behavior_fingerprint(option);
}

[[nodiscard]] std::string identity_fingerprint(
    CompatibilityBaseline const& baseline) {
  return baseline.target_id.value + "|" + baseline.software_item_id.value +
         "|" + baseline.installer_baseline_id.value;
}

[[nodiscard]] bool mark_scheme_cycles(SoftwareOptimizationCatalog& catalog) {
  std::unordered_map<std::string, std::size_t> indexes;
  for (std::size_t index = 0; index < catalog.schemes.size(); ++index) {
    indexes.emplace(catalog.schemes[index].id.value, index);
  }
  bool changed = false;
  for (std::size_t start = 0; start < catalog.schemes.size(); ++start) {
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    auto visit = [&](auto&& self, std::size_t index) -> bool {
      auto const& scheme = catalog.schemes[index];
      if (visiting.contains(scheme.id.value)) {
        return true;
      }
      if (!visited.insert(scheme.id.value).second) {
        return false;
      }
      visiting.insert(scheme.id.value);
      bool cycle = false;
      for (auto const& required_id : scheme.required_scheme_ids) {
        auto const found = indexes.find(required_id.value);
        if (found != indexes.end() && self(self, found->second)) {
          cycle = true;
        }
      }
      visiting.erase(scheme.id.value);
      if (cycle) {
        auto& mutable_scheme = catalog.schemes[index];
        auto const before = mutable_scheme.configuration_issues.size();
        add_configuration_issue(mutable_scheme,
                                CatalogIssueCode::relationship_cycle,
                                "required scheme relationship forms a cycle");
        changed |= before != mutable_scheme.configuration_issues.size();
      }
      return cycle;
    };
    static_cast<void>(visit(visit, start));
  }
  return changed;
}

void mark_option_cycles(SoftwareOptimizationScheme& scheme) {
  std::unordered_map<std::string, SoftwareOptimizationOption const*> options;
  for (auto const& option : scheme.options) {
    options.emplace(option.id.value, &option);
  }
  for (auto const& start : scheme.options) {
    std::unordered_set<std::string> visiting;
    std::unordered_set<std::string> visited;
    auto visit = [&](auto&& self,
                     SoftwareOptimizationOption const& option) -> bool {
      if (visiting.contains(option.id.value)) {
        return true;
      }
      if (!visited.insert(option.id.value).second) {
        return false;
      }
      visiting.insert(option.id.value);
      bool cycle = false;
      for (auto const& required_id : option.required_option_ids) {
        auto const found = options.find(required_id.value);
        if (found != options.end() && self(self, *found->second)) {
          cycle = true;
        }
      }
      visiting.erase(option.id.value);
      return cycle;
    };
    if (visit(visit, start)) {
      add_configuration_issue(scheme, CatalogIssueCode::relationship_cycle,
                              "required option relationship forms a cycle");
      return;
    }
  }
}

void validate_local_rules(
    SoftwareOptimizationCatalog& catalog,
    std::span<BuiltInRuleDefinition const> built_in_rules) {
  std::unordered_map<std::string, TargetSoftware const*> targets;
  for (auto const& target : catalog.targets) {
    targets.emplace(target.id.value, &target);
  }
  std::unordered_map<std::string, SoftwareOptimizationScheme const*> schemes;
  for (auto const& scheme : catalog.schemes) {
    schemes.emplace(scheme.id.value, &scheme);
  }

  for (auto& scheme : catalog.schemes) {
    auto const target = targets.find(scheme.target_id.value);
    if (target == targets.end()) {
      add_configuration_issue(scheme, CatalogIssueCode::missing_reference,
                              "target software does not exist");
    } else {
      auto const& definition = *target->second;
      if (!registered_rule(built_in_rules, definition.install_detection,
                           RulePurpose::install_detection) ||
          !registered_rule(built_in_rules, definition.version_detection,
                           RulePurpose::version_detection)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "target detection or version detection rule is invalid");
      }
      if (definition.installation_item_id.has_value() &&
          !definition.installation_item_id->valid()) {
        add_configuration_issue(
            scheme, CatalogIssueCode::missing_reference,
            "optional software installation item reference is invalid");
      }
      if (definition.support_mode == SupportMode::recognition_only) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "recognition-only target cannot expose an executable scheme");
      }
      if (!valid_explanation_source(definition.explanation_source)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_file_field,
            "target explanation source must be an HTTP(S) address");
      }
      if (definition.support_mode == SupportMode::supported &&
          !valid_range(definition.supported_versions)) {
        add_configuration_issue(scheme,
                                CatalogIssueCode::invalid_version_range,
                                "target supported version range is invalid");
      }
    }

    if (!valid_range(scheme.supported_versions)) {
      add_configuration_issue(scheme, CatalogIssueCode::invalid_version_range,
                              "scheme supported version range is invalid");
    }
    if (!valid_explanation_source(scheme.explanation_source)) {
      add_configuration_issue(
          scheme, CatalogIssueCode::invalid_file_field,
          "scheme explanation source must be an HTTP(S) address");
    }
    if (target != targets.end() &&
        target->second->support_mode == SupportMode::supported &&
        valid_range(target->second->supported_versions) &&
        valid_range(scheme.supported_versions) &&
        !intersects(target->second->supported_versions,
                    scheme.supported_versions)) {
      add_configuration_issue(
          scheme, CatalogIssueCode::invalid_version_range,
          "scheme version range does not intersect its target support range");
    }
    if (scheme.options.empty()) {
      add_configuration_issue(scheme, CatalogIssueCode::invalid_rule,
                              "scheme has no options");
    }
    for (auto const& required_id : scheme.required_scheme_ids) {
      if (!schemes.contains(required_id.value)) {
        add_configuration_issue(scheme, CatalogIssueCode::missing_reference,
                                "required scheme '" + required_id.value +
                                    "' does not exist");
      }
      if (contains(scheme.conflicting_scheme_ids, required_id.value)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::relationship_conflict,
            "the same scheme is both required and conflicting");
      }
    }
    for (auto const& conflicting_id : scheme.conflicting_scheme_ids) {
      if (!schemes.contains(conflicting_id.value)) {
        add_configuration_issue(scheme, CatalogIssueCode::missing_reference,
                                "conflicting scheme '" +
                                    conflicting_id.value + "' does not exist");
      }
      if (conflicting_id == scheme.id) {
        add_configuration_issue(
            scheme, CatalogIssueCode::relationship_conflict,
            "scheme cannot conflict with itself");
      }
    }

    std::unordered_map<std::string, SoftwareOptimizationOption const*>
        option_index;
    for (auto const& option : scheme.options) {
      option_index.emplace(option.id.value, &option);
    }
    for (auto const& option : scheme.options) {
      if (!valid_range(option.supported_versions)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_version_range,
            "option '" + option.id.value +
                "' has an invalid supported version range");
      }
      if (valid_range(option.supported_versions) &&
          valid_range(scheme.supported_versions) &&
          !intersects(option.supported_versions,
                      scheme.supported_versions)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_version_range,
            "option '" + option.id.value +
                "' version range does not intersect its scheme");
      }
      if (!valid_explanation_source(option.explanation_source)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_file_field,
            "option '" + option.id.value +
                "' explanation source must be an HTTP(S) address");
      }
      bool const controlled =
          option.automation == AutomationSupport::controlled;
      bool const valid_execution = registered_rule(
          built_in_rules, option.execution, RulePurpose::option_execution);
      bool const valid_detection = registered_rule(
          built_in_rules, option.state_detection,
          RulePurpose::option_state_detection);
      if ((controlled && (!valid_execution || !valid_detection)) ||
          (!controlled &&
           (option.execution.kind != RuleKind::none ||
            option.state_detection.kind != RuleKind::none ||
            !option.execution.definition.value.empty() ||
            !option.state_detection.definition.value.empty()))) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "option '" + option.id.value +
                "' has inconsistent controlled execution or detection rules");
      }
      if ((scheme.automation == AutomationSupport::controlled) != controlled) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "scheme and option automation modes are inconsistent");
      }
      if (option.required && !option.default_selected) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "required option '" + option.id.value +
                "' must remain selected by default");
      }
      if (!std::ranges::all_of(option.allowed_values,
                               valid_controlled_value)) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "option '" + option.id.value +
                "' contains an uncontrolled value choice");
      }
      if (!option.allowed_values.empty()) {
        if (!option.default_value.has_value() ||
            std::ranges::find(option.allowed_values,
                              *option.default_value) ==
                option.allowed_values.end()) {
          add_configuration_issue(
              scheme, CatalogIssueCode::invalid_rule,
              "option '" + option.id.value +
                  "' has a default outside its controlled values");
        }
      } else if (option.default_value.has_value()) {
        add_configuration_issue(
            scheme, CatalogIssueCode::invalid_rule,
            "option '" + option.id.value +
                "' declares a default without controlled values");
      }
      for (auto const& required_id : option.required_option_ids) {
        if (!option_index.contains(required_id.value)) {
          add_configuration_issue(
              scheme, CatalogIssueCode::missing_reference,
              "option '" + option.id.value + "' requires missing option '" +
                  required_id.value + "'");
        }
        if (contains(option.conflicting_option_ids, required_id.value)) {
          add_configuration_issue(
              scheme, CatalogIssueCode::relationship_conflict,
              "option '" + option.id.value +
                  "' both requires and conflicts with the same option");
        }
      }
      for (auto const& conflicting_id : option.conflicting_option_ids) {
        if (!option_index.contains(conflicting_id.value)) {
          add_configuration_issue(
              scheme, CatalogIssueCode::missing_reference,
              "option '" + option.id.value +
                  "' conflicts with missing option '" +
                  conflicting_id.value + "'");
        }
        if (conflicting_id == option.id) {
          add_configuration_issue(
              scheme, CatalogIssueCode::relationship_conflict,
              "option '" + option.id.value + "' cannot conflict with itself");
        }
      }
    }
    mark_option_cycles(scheme);
  }

  static_cast<void>(mark_scheme_cycles(catalog));
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto& scheme : catalog.schemes) {
      if (scheme.availability == SchemeAvailability::configuration_error) {
        continue;
      }
      for (auto const& required_id : scheme.required_scheme_ids) {
        auto const found = std::ranges::find_if(
            catalog.schemes, [&](SoftwareOptimizationScheme const& value) {
              return value.id == required_id;
            });
        if (found != catalog.schemes.end() &&
            found->availability == SchemeAvailability::configuration_error) {
          add_configuration_issue(
              scheme, CatalogIssueCode::missing_reference,
              "required scheme '" + required_id.value +
                  "' is disabled by a catalog configuration error");
          changed = true;
          break;
        }
      }
    }
  }
  for (auto& scheme : catalog.schemes) {
    if (scheme.availability != SchemeAvailability::configuration_error &&
        scheme.automation == AutomationSupport::manual_only) {
      scheme.availability = SchemeAvailability::manual_only;
    }
  }
}

void validate_release(SoftwareOptimizationCatalog& catalog) {
  if (catalog.publication_state != PublicationState::release) {
    add_issue(catalog.release_issues,
              CatalogIssueCode::first_release_incomplete, {},
              "catalog release_state is not release");
  }
  bool has_required_target = false;
  for (auto const& baseline : catalog.compatibility_baselines) {
    if (catalog.find_target(baseline.target_id.value) == nullptr) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::missing_reference, baseline.id.value,
                "compatibility baseline target does not exist");
    }
    if (!baseline.software_item_id.valid() ||
        !baseline.installer_baseline_id.valid() ||
        !valid_range(baseline.installed_versions)) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::compatibility_baseline_empty,
                baseline.id.value,
                "compatibility baseline identities or version range are invalid");
    }
  }
  for (auto const& target : catalog.targets) {
    if (!target.required_first_release) {
      continue;
    }
    has_required_target = true;
    if (target.support_mode != SupportMode::supported ||
        !valid_range(target.supported_versions)) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::first_release_incomplete, target.id.value,
                "first-release target has no non-empty supported range");
    }
    auto const valid_scheme = std::ranges::any_of(
        catalog.schemes, [&](SoftwareOptimizationScheme const& scheme) {
          return scheme.target_id == target.id &&
                 scheme.required_first_release &&
                 scheme.availability == SchemeAvailability::available &&
                 scheme.automation == AutomationSupport::controlled;
        });
    if (!valid_scheme) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::first_release_incomplete, target.id.value,
                "first-release target has no available controlled scheme");
    }
    auto const has_baseline = std::ranges::any_of(
        catalog.compatibility_baselines,
        [&](CompatibilityBaseline const& value) {
          return value.target_id == target.id;
        });
    auto const valid_baseline = std::ranges::any_of(
        catalog.compatibility_baselines,
        [&](CompatibilityBaseline const& value) {
          return value.target_id == target.id &&
                 valid_range(value.installed_versions) &&
                 intersects(target.supported_versions,
                            value.installed_versions);
        });
    if (!has_baseline) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::compatibility_baseline_missing,
                target.id.value,
                "first-release target has no installer compatibility baseline");
    } else if (!valid_baseline) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::compatibility_baseline_empty,
                target.id.value,
                "installer baseline and target support range do not intersect");
    }
  }
  if (!has_required_target) {
    add_issue(catalog.release_issues,
              CatalogIssueCode::first_release_incomplete, {},
              "catalog declares no first-release target");
  }
  for (auto const& scheme : catalog.schemes) {
    if (!scheme.required_first_release) {
      continue;
    }
    if (scheme.availability != SchemeAvailability::available ||
        scheme.automation != AutomationSupport::controlled) {
      add_issue(catalog.release_issues,
                CatalogIssueCode::first_release_incomplete, scheme.id.value,
                "first-release scheme is not available for controlled execution");
      continue;
    }
    for (auto const& option : scheme.options) {
      if (option.automation != AutomationSupport::controlled ||
          option.execution.kind != RuleKind::built_in_definition ||
          option.state_detection.kind != RuleKind::built_in_definition ||
          !option.execution.definition.valid() ||
          !option.state_detection.definition.valid()) {
        add_issue(catalog.release_issues,
                  CatalogIssueCode::first_release_incomplete, option.id.value,
                  "first-release option lacks controlled execution or state detection");
      }
    }
  }
}

}  // namespace

bool StableId::valid() const noexcept {
  if (value.empty() || value.size() > 96 ||
      !(ascii_lower(value.front()) || ascii_digit(value.front())) ||
      !(ascii_lower(value.back()) || ascii_digit(value.back()))) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return ascii_lower(character) || ascii_digit(character) ||
           character == '-' || character == '_' || character == '.';
  });
}

TargetSoftware const* SoftwareOptimizationCatalog::find_target(
    std::string_view id) const noexcept {
  auto const found = std::ranges::find_if(
      targets, [id](TargetSoftware const& target) {
        return target.id.value == id;
      });
  return found == targets.end() ? nullptr : &*found;
}

SoftwareOptimizationScheme const* SoftwareOptimizationCatalog::find_scheme(
    std::string_view id) const noexcept {
  auto const found = std::ranges::find_if(
      schemes, [id](SoftwareOptimizationScheme const& scheme) {
        return scheme.id.value == id;
      });
  return found == schemes.end() ? nullptr : &*found;
}

CatalogLoadResult load_catalog(
    std::string_view source,
    std::span<BuiltInRuleDefinition const> built_in_rules) {
  auto parsed = parse_document(source);
  if (!parsed.document.has_value()) {
    return {.package_issues = std::move(parsed.issues)};
  }
  auto const& raw = *parsed.document;
  std::vector<CatalogIssue> issues;

  reject_unknown_fields(raw.file, kFileFields, "catalog", issues);
  for (auto const& record : raw.targets) {
    reject_unknown_fields(record, kTargetFields, entity_name(record, "target"),
                          issues);
  }
  for (auto const& record : raw.schemes) {
    reject_unknown_fields(record, kSchemeFields, entity_name(record, "scheme"),
                          issues);
  }
  for (auto const& record : raw.options) {
    reject_unknown_fields(record, kOptionFields, entity_name(record, "option"),
                          issues);
  }
  for (auto const& record : raw.baselines) {
    reject_unknown_fields(record, kBaselineFields,
                          entity_name(record, "baseline"), issues);
  }

  auto const schema =
      required<std::uint64_t>(raw.file, "schema_version", "catalog", issues);
  auto const semantics = required<std::uint64_t>(
      raw.file, "semantics_version", "catalog", issues);
  auto const catalog_id =
      required<std::string>(raw.file, "catalog_id", "catalog", issues);
  auto const revision =
      required<std::uint64_t>(raw.file, "revision", "catalog", issues);
  auto const release_state =
      required<std::string>(raw.file, "release_state", "catalog", issues);
  static_cast<void>(optional_string(raw.file, "default_locale", issues,
                                    "catalog"));
  if (schema.has_value() && *schema != 1) {
    add_issue(issues, CatalogIssueCode::unknown_schema, "catalog",
              "unsupported schema_version " + std::to_string(*schema));
  }
  if (semantics.has_value() && *semantics != 1) {
    add_issue(issues, CatalogIssueCode::unknown_execution_semantics, "catalog",
              "unsupported semantics_version " +
                  std::to_string(*semantics));
  }
  if (catalog_id.has_value() && *catalog_id != "software-optimization") {
    add_issue(issues, CatalogIssueCode::invalid_file_field, "catalog",
              "catalog_id must be software-optimization");
  }
  if (revision.has_value() && *revision == 0) {
    add_issue(issues, CatalogIssueCode::invalid_file_field, "catalog",
              "revision must be greater than zero");
  }
  std::optional<PublicationState> publication;
  if (release_state.has_value()) {
    if (*release_state == "draft") {
      publication = PublicationState::draft;
    } else if (*release_state == "release") {
      publication = PublicationState::release;
    } else {
      add_issue(issues, CatalogIssueCode::invalid_file_field, "catalog",
                "release_state must be draft or release");
    }
  }

  SoftwareOptimizationCatalog catalog;
  if (revision.has_value()) {
    catalog.revision = *revision;
  }
  if (publication.has_value()) {
    catalog.publication_state = *publication;
  }

  for (auto const& record : raw.targets) {
    auto const entity = entity_name(record, "target");
    auto const id = required<std::string>(record, "id", entity, issues);
    auto const anchor =
        required<std::string>(record, "identity_anchor", entity, issues);
    auto const first = required<bool>(record, "required_first_release", entity,
                                      issues);
    auto const mode_text =
        required<std::string>(record, "support_mode", entity, issues);
    auto const minimum =
        required<std::string>(record, "version_min", entity, issues);
    auto const maximum =
        required<std::string>(record, "version_max", entity, issues);
    auto const install_kind_text = required<std::string>(
        record, "install_detection_kind", entity, issues);
    auto const install_definition = required<std::string>(
        record, "install_detection_definition", entity, issues);
    auto const version_kind_text = required<std::string>(
        record, "version_detection_kind", entity, issues);
    auto const version_definition = required<std::string>(
        record, "version_detection_definition", entity, issues);
    auto const installation_item =
        optional_string(record, "installation_item_id", issues, entity);
    auto const explanation =
        required<std::string>(record, "explanation_source", entity, issues);
    auto mode = mode_text.has_value() ? support_mode(*mode_text) : std::nullopt;
    auto install_kind = install_kind_text.has_value()
                            ? rule_kind(*install_kind_text)
                            : std::nullopt;
    auto version_kind = version_kind_text.has_value()
                            ? rule_kind(*version_kind_text)
                            : std::nullopt;
    if (mode_text.has_value() && !mode.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown support_mode '" + *mode_text + "'");
    }
    if (install_kind_text.has_value() && !install_kind.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown install_detection_kind '" + *install_kind_text +
                    "'");
    }
    if (version_kind_text.has_value() && !version_kind.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown version_detection_kind '" + *version_kind_text +
                    "'");
    }
    if (id && anchor && first && mode && minimum && maximum && install_kind &&
        install_definition && version_kind && version_definition &&
        explanation) {
      TargetSoftware target{
          .id = StableId{*id},
          .identity_anchor = StableId{*anchor},
          .required_first_release = *first,
          .support_mode = *mode,
          .supported_versions = {*minimum, *maximum},
          .install_detection = {*install_kind, StableId{*install_definition}},
          .version_detection = {*version_kind, StableId{*version_definition}},
          .explanation_source = *explanation,
      };
      if (installation_item.has_value() && !installation_item->empty()) {
        target.installation_item_id = StableId{*installation_item};
      }
      catalog.targets.push_back(std::move(target));
    }
  }

  for (auto const& record : raw.schemes) {
    auto const entity = entity_name(record, "scheme");
    auto const id = required<std::string>(record, "id", entity, issues);
    auto const target_id =
        required<std::string>(record, "target_id", entity, issues);
    auto const first = required<bool>(record, "required_first_release", entity,
                                      issues);
    auto const automation_text =
        required<std::string>(record, "automation", entity, issues);
    auto const minimum =
        required<std::string>(record, "version_min", entity, issues);
    auto const maximum =
        required<std::string>(record, "version_max", entity, issues);
    auto const impact =
        required<std::string>(record, "impact", entity, issues);
    auto const risk_text =
        required<std::string>(record, "risk", entity, issues);
    auto const exit_text =
        required<std::string>(record, "exit_requirement", entity, issues);
    auto const restart_text =
        required<std::string>(record, "restart_requirement", entity, issues);
    auto const required_schemes = required<std::vector<std::string>>(
        record, "required_scheme_ids", entity, issues);
    auto const conflicting_schemes = required<std::vector<std::string>>(
        record, "conflicting_scheme_ids", entity, issues);
    auto const explanation =
        required<std::string>(record, "explanation_source", entity, issues);
    auto const emergency = required<std::string>(
        record, "manual_emergency_explanation", entity, issues);
    auto automation_value = automation_text.has_value()
                                ? automation(*automation_text)
                                : std::nullopt;
    auto risk_value =
        risk_text.has_value() ? risk_level(*risk_text) : std::nullopt;
    auto exit_value = exit_text.has_value()
                          ? exit_requirement(*exit_text)
                          : std::nullopt;
    auto restart_value = restart_text.has_value()
                             ? restart_requirement(*restart_text)
                             : std::nullopt;
    if (automation_text.has_value() && !automation_value.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown automation '" + *automation_text + "'");
    }
    if (risk_text.has_value() && !risk_value.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown risk '" + *risk_text + "'");
    }
    if (exit_text.has_value() && !exit_value.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown exit_requirement '" + *exit_text + "'");
    }
    if (restart_text.has_value() && !restart_value.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown restart_requirement '" + *restart_text + "'");
    }
    if (id && target_id && first && automation_value && minimum && maximum &&
        impact && risk_value && exit_value && restart_value &&
        required_schemes && conflicting_schemes && explanation && emergency) {
      catalog.schemes.push_back(SoftwareOptimizationScheme{
          .id = StableId{*id},
          .target_id = StableId{*target_id},
          .required_first_release = *first,
          .automation = *automation_value,
          .supported_versions = {*minimum, *maximum},
          .impact = *impact,
          .risk = *risk_value,
          .exit_requirement = *exit_value,
          .restart_requirement = *restart_value,
          .required_scheme_ids = ids(*required_schemes),
          .conflicting_scheme_ids = ids(*conflicting_schemes),
          .explanation_source = *explanation,
          .manual_emergency_explanation = *emergency,
      });
    }
  }

  struct ParsedOption final {
    SoftwareOptimizationOption option;
    std::string entity;
  };
  std::vector<ParsedOption> parsed_options;
  for (auto const& record : raw.options) {
    auto const entity = entity_name(record, "option");
    auto const id = required<std::string>(record, "id", entity, issues);
    auto const scheme_id =
        required<std::string>(record, "scheme_id", entity, issues);
    auto const minimum =
        required<std::string>(record, "version_min", entity, issues);
    auto const maximum =
        required<std::string>(record, "version_max", entity, issues);
    auto const impact =
        required<std::string>(record, "impact", entity, issues);
    auto const default_selected =
        required<bool>(record, "default_selected", entity, issues);
    auto const required_value =
        required<bool>(record, "required", entity, issues);
    auto const automation_text =
        required<std::string>(record, "automation", entity, issues);
    auto const execution_kind_text =
        required<std::string>(record, "execution_kind", entity, issues);
    auto const execution_definition =
        required<std::string>(record, "execution_definition", entity, issues);
    auto const detection_kind_text = required<std::string>(
        record, "state_detection_kind", entity, issues);
    auto const detection_definition = required<std::string>(
        record, "state_detection_definition", entity, issues);
    auto const required_options = required<std::vector<std::string>>(
        record, "required_option_ids", entity, issues);
    auto const conflicting_options = required<std::vector<std::string>>(
        record, "conflicting_option_ids", entity, issues);
    auto const allowed_values = required<std::vector<std::string>>(
        record, "allowed_values", entity, issues);
    auto const default_value =
        optional_string(record, "default_value", issues, entity);
    auto const explanation =
        required<std::string>(record, "explanation_source", entity, issues);
    auto automation_value = automation_text.has_value()
                                ? automation(*automation_text)
                                : std::nullopt;
    auto execution_kind = execution_kind_text.has_value()
                              ? rule_kind(*execution_kind_text)
                              : std::nullopt;
    auto detection_kind = detection_kind_text.has_value()
                              ? rule_kind(*detection_kind_text)
                              : std::nullopt;
    if (automation_text.has_value() && !automation_value.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown automation '" + *automation_text + "'");
    }
    if (execution_kind_text.has_value() && !execution_kind.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown execution_kind '" + *execution_kind_text + "'");
    }
    if (detection_kind_text.has_value() && !detection_kind.has_value()) {
      add_issue(issues, CatalogIssueCode::unknown_execution_semantics, entity,
                "unknown state_detection_kind '" + *detection_kind_text +
                    "'");
    }
    if (id && scheme_id && minimum && maximum && impact && default_selected &&
        required_value && automation_value && execution_kind &&
        execution_definition && detection_kind && detection_definition &&
        required_options && conflicting_options && allowed_values &&
        explanation) {
      SoftwareOptimizationOption option{
          .id = StableId{*id},
          .scheme_id = StableId{*scheme_id},
          .supported_versions = {*minimum, *maximum},
          .impact = *impact,
          .default_selected = *default_selected,
          .required = *required_value,
          .automation = *automation_value,
          .execution = {*execution_kind, StableId{*execution_definition}},
          .state_detection = {*detection_kind,
                              StableId{*detection_definition}},
          .required_option_ids = ids(*required_options),
          .conflicting_option_ids = ids(*conflicting_options),
          .allowed_values = *allowed_values,
          .explanation_source = *explanation,
      };
      if (default_value.has_value() && !default_value->empty()) {
        option.default_value = *default_value;
      }
      parsed_options.push_back(
          ParsedOption{.option = std::move(option), .entity = entity});
    }
  }

  for (auto const& record : raw.baselines) {
    auto const entity = entity_name(record, "baseline");
    auto const id = required<std::string>(record, "id", entity, issues);
    auto const target_id =
        required<std::string>(record, "target_id", entity, issues);
    auto const software_id =
        required<std::string>(record, "software_item_id", entity, issues);
    auto const installer_id = required<std::string>(
        record, "installer_baseline_id", entity, issues);
    auto const minimum = required<std::string>(
        record, "installed_version_min", entity, issues);
    auto const maximum = required<std::string>(
        record, "installed_version_max", entity, issues);
    if (id && target_id && software_id && installer_id && minimum && maximum) {
      catalog.compatibility_baselines.push_back(CompatibilityBaseline{
          .id = StableId{*id},
          .target_id = StableId{*target_id},
          .software_item_id = StableId{*software_id},
          .installer_baseline_id = StableId{*installer_id},
          .installed_versions = {*minimum, *maximum},
      });
    }
  }

  if (!issues.empty()) {
    return {.package_issues = std::move(issues)};
  }

  std::unordered_map<std::string, std::string> stable_ids;
  auto register_id = [&](StableId const& id, std::string kind) {
    if (!id.valid()) {
      add_issue(issues, CatalogIssueCode::invalid_file_field, id.value,
                kind + " stable id is invalid");
      return;
    }
    auto const [found, inserted] = stable_ids.emplace(id.value, kind);
    if (!inserted) {
      add_issue(issues, CatalogIssueCode::duplicate_stable_id, id.value,
                "stable id is shared by " + found->second + " and " + kind);
    }
  };
  for (auto const& target : catalog.targets) {
    register_id(target.id, "target");
    if (!target.identity_anchor.valid()) {
      add_issue(issues, CatalogIssueCode::invalid_file_field, target.id.value,
                "target identity_anchor is invalid");
    }
  }
  for (auto const& scheme : catalog.schemes) {
    register_id(scheme.id, "scheme");
  }
  for (auto const& parsed_option : parsed_options) {
    register_id(parsed_option.option.id, "option");
  }
  for (auto const& baseline : catalog.compatibility_baselines) {
    register_id(baseline.id, "compatibility baseline");
  }
  if (!issues.empty()) {
    return {.package_issues = std::move(issues)};
  }

  for (auto& parsed_option : parsed_options) {
    auto const found = std::ranges::find_if(
        catalog.schemes, [&](SoftwareOptimizationScheme const& scheme) {
          return scheme.id == parsed_option.option.scheme_id;
        });
    if (found == catalog.schemes.end()) {
      add_issue(issues, CatalogIssueCode::missing_required_field,
                parsed_option.option.id.value,
                "option refers to a scheme that does not exist");
      continue;
    }
    found->options.push_back(std::move(parsed_option.option));
  }
  if (!issues.empty()) {
    return {.package_issues = std::move(issues)};
  }

  validate_local_rules(catalog, built_in_rules);
  validate_release(catalog);
  return {.catalog = std::move(catalog)};
}

CatalogSummary summarize(SoftwareOptimizationCatalog const& catalog) noexcept {
  CatalogSummary result{
      .revision = catalog.revision,
      .target_count = catalog.targets.size(),
      .scheme_count = catalog.schemes.size(),
      .intrinsic_release_eligible = catalog.release_issues.empty(),
  };
  for (auto const& scheme : catalog.schemes) {
    result.option_count += scheme.options.size();
    if (scheme.availability == SchemeAvailability::configuration_error) {
      ++result.disabled_scheme_count;
    }
  }
  return result;
}

std::optional<FrozenScheme> freeze_scheme(
    SoftwareOptimizationCatalog const& catalog, std::string_view scheme_id) {
  auto const* scheme = catalog.find_scheme(scheme_id);
  if (scheme == nullptr) {
    return std::nullopt;
  }
  return FrozenScheme{.catalog_revision = catalog.revision,
                      .scheme = *scheme};
}

std::vector<StableIdentityRecord> stable_identities(
    SoftwareOptimizationCatalog const& catalog) {
  std::vector<StableIdentityRecord> result;
  result.reserve(catalog.targets.size() + catalog.schemes.size() +
                 catalog.compatibility_baselines.size());
  for (auto const& target : catalog.targets) {
    result.push_back({target.id, StableEntityKind::target,
                      identity_fingerprint(target)});
  }
  for (auto const& scheme : catalog.schemes) {
    result.push_back({scheme.id, StableEntityKind::scheme,
                      identity_fingerprint(scheme)});
    for (auto const& option : scheme.options) {
      result.push_back({option.id, StableEntityKind::option,
                        identity_fingerprint(option)});
    }
  }
  for (auto const& baseline : catalog.compatibility_baselines) {
    result.push_back({baseline.id, StableEntityKind::compatibility_baseline,
                      identity_fingerprint(baseline)});
  }
  std::ranges::sort(result, {}, [](StableIdentityRecord const& record) {
    return record.id.value;
  });
  return result;
}

std::vector<CatalogIssue> validate_stable_identity_history(
    SoftwareOptimizationCatalog const& catalog,
    std::span<StableIdentityRecord const> history) {
  std::unordered_map<std::string, StableIdentityRecord const*> known;
  for (auto const& record : history) {
    known.emplace(record.id.value, &record);
  }
  std::vector<CatalogIssue> issues;
  for (auto const& candidate : stable_identities(catalog)) {
    auto const found = known.find(candidate.id.value);
    if (found == known.end()) {
      continue;
    }
    if (found->second->kind != candidate.kind ||
        found->second->semantic_fingerprint !=
            candidate.semantic_fingerprint) {
      add_issue(issues, CatalogIssueCode::stable_id_reuse,
                candidate.id.value,
                "stable id was previously assigned to different operation semantics");
    }
  }
  return issues;
}

std::optional<std::vector<StableIdentityRecord>> merge_stable_identity_history(
    std::span<StableIdentityRecord const> history,
    SoftwareOptimizationCatalog const& catalog, std::size_t maximum_count) {
  if (history.size() > maximum_count) {
    return std::nullopt;
  }
  std::vector<StableIdentityRecord> result{history.begin(), history.end()};
  std::unordered_set<std::string> known;
  for (auto const& record : result) {
    known.insert(record.id.value);
  }
  for (auto& record : stable_identities(catalog)) {
    if (known.insert(record.id.value).second) {
      if (result.size() == maximum_count) {
        return std::nullopt;
      }
      result.push_back(std::move(record));
    }
  }
  std::ranges::sort(result, {}, [](StableIdentityRecord const& record) {
    return record.id.value;
  });
  return std::optional<std::vector<StableIdentityRecord>>{std::move(result)};
}

std::vector<StableId> schemes_lost_or_changed(
    SoftwareOptimizationCatalog const& current,
    SoftwareOptimizationCatalog const& candidate) {
  std::vector<StableId> result;
  for (auto const& scheme : current.schemes) {
    auto const* replacement = candidate.find_scheme(scheme.id.value);
    if (replacement == nullptr || scheme != *replacement) {
      result.push_back(scheme.id);
    }
  }
  std::ranges::sort(result, {}, &StableId::value);
  return result;
}

CompatibilityAssessment assess_release_compatibility(
    SoftwareOptimizationCatalog const& catalog,
    std::span<SoftwareCatalogInstallerBaseline const> software_baselines) {
  CompatibilityAssessment result{.issues = catalog.release_issues};
  for (auto const& target : catalog.targets) {
    if (!target.required_first_release) {
      continue;
    }
    bool matched = false;
    bool found_matching_identity = false;
    std::vector<CatalogIssue> range_issues;
    for (auto const& optimization_baseline :
         catalog.compatibility_baselines) {
      if (optimization_baseline.target_id != target.id) {
        continue;
      }
      for (auto const& software_baseline : software_baselines) {
        if (software_baseline.software_item_id !=
                optimization_baseline.software_item_id ||
            software_baseline.installer_baseline_id !=
                optimization_baseline.installer_baseline_id) {
          continue;
        }
        found_matching_identity = true;
        std::vector<CatalogIssue> candidate_issues;
        auto add_range_issue = [&](std::string entity,
                                   std::string detail) {
          add_issue(candidate_issues,
                    CatalogIssueCode::compatibility_baseline_mismatch,
                    std::move(entity), std::move(detail));
        };
        if (!valid_range(software_baseline.installed_versions) ||
            !contains_range(optimization_baseline.installed_versions,
                            software_baseline.installed_versions)) {
          add_range_issue(
              optimization_baseline.id.value,
              "optimization baseline does not contain the installer output range");
        }
        if (!contains_range(target.supported_versions,
                            software_baseline.installed_versions)) {
          add_range_issue(
              target.id.value,
              "target support does not contain the installer output range");
        }
        for (auto const& scheme : catalog.schemes) {
          if (scheme.target_id != target.id ||
              !scheme.required_first_release) {
            continue;
          }
          if (!contains_range(scheme.supported_versions,
                              software_baseline.installed_versions)) {
            add_range_issue(
                scheme.id.value,
                "first-release scheme does not contain the installer output range");
          }
          for (auto const& option : scheme.options) {
            if (!contains_range(option.supported_versions,
                                software_baseline.installed_versions)) {
              add_range_issue(
                  option.id.value,
                  "first-release option does not contain the installer output range");
            }
          }
        }
        if (candidate_issues.empty()) {
          matched = true;
          break;
        }
        for (auto& issue : candidate_issues) {
          auto const duplicate = std::ranges::any_of(
              range_issues, [&](CatalogIssue const& existing) {
                return existing.code == issue.code &&
                       existing.entity_id == issue.entity_id &&
                       existing.detail == issue.detail;
              });
          if (!duplicate) {
            range_issues.push_back(std::move(issue));
          }
        }
      }
      if (matched) {
        break;
      }
    }
    if (!matched) {
      if (found_matching_identity && !range_issues.empty()) {
        result.issues.insert(result.issues.end(),
                             std::make_move_iterator(range_issues.begin()),
                             std::make_move_iterator(range_issues.end()));
      } else {
        add_issue(
            result.issues,
            CatalogIssueCode::compatibility_baseline_mismatch,
            target.id.value,
            "software catalog and optimization catalog installer identities do not match");
      }
    }
  }
  result.compatible = result.issues.empty();
  return result;
}

}  // namespace azzs::domain::software_optimization_catalog
