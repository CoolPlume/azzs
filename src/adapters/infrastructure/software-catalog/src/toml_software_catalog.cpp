#include "azzs/adapters/infrastructure/software_catalog_file.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace azzs::adapters::infrastructure {
namespace {

namespace app_catalog = application::software_catalog;
namespace catalog = domain::software_catalog;

enum class ValueKind {
  text,
  integer,
  boolean,
  text_list,
};

struct ParsedValue final {
  ValueKind kind{ValueKind::text};
  std::string text;
  std::uint64_t integer{0};
  bool boolean{false};
  std::vector<std::string> text_list;
  std::size_t line{0};
};

struct ParsedTable final {
  std::string location;
  std::size_t line{0};
  std::unordered_map<std::string, ParsedValue> values;
};

struct ParsedSource final {
  ParsedTable table;
  std::vector<ParsedTable> history;
};

struct ParsedCategory final {
  ParsedTable table;
  std::vector<std::pair<std::string, ParsedTable>> localizations;
};

struct ParsedSoftware final {
  ParsedTable table;
  std::vector<ParsedSource> sources;
  std::optional<ParsedTable> education;
  std::vector<std::pair<std::string, ParsedTable>> localizations;
};

struct ParsedDriver final {
  ParsedTable table;
  std::vector<ParsedSource> sources;
  std::vector<std::pair<std::string, ParsedTable>> localizations;
};

struct ParsedCatalog final {
  ParsedTable root{.location = "root", .line = 1};
  std::vector<ParsedCategory> categories;
  std::vector<ParsedSoftware> software;
  std::vector<ParsedDriver> drivers;
  std::vector<catalog::CatalogIssue> issues;
};

[[nodiscard]] std::string_view trim(std::string_view value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return value;
}

void add_issue(ParsedCatalog& parsed, catalog::CatalogIssueCode code,
               std::string location, std::string message) {
  parsed.issues.push_back(catalog::CatalogIssue{
      .scope = catalog::CatalogIssueScope::package,
      .code = code,
      .location = std::move(location),
      .message = std::move(message),
  });
}

[[nodiscard]] bool valid_utf8(std::string_view bytes) {
  std::size_t index{};
  while (index < bytes.size()) {
    auto const lead = static_cast<unsigned char>(bytes[index]);
    if (lead <= 0x7f) {
      ++index;
      continue;
    }
    std::size_t continuation{};
    std::uint32_t code_point{};
    if ((lead & 0xe0U) == 0xc0U) {
      continuation = 1;
      code_point = lead & 0x1fU;
    } else if ((lead & 0xf0U) == 0xe0U) {
      continuation = 2;
      code_point = lead & 0x0fU;
    } else if ((lead & 0xf8U) == 0xf0U) {
      continuation = 3;
      code_point = lead & 0x07U;
    } else {
      return false;
    }
    if (index + continuation >= bytes.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      auto const byte = static_cast<unsigned char>(bytes[index + offset]);
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (byte & 0x3fU);
    }
    if ((continuation == 1 && code_point < 0x80U) ||
        (continuation == 2 && code_point < 0x800U) ||
        (continuation == 3 && code_point < 0x10000U) ||
        code_point > 0x10ffffU ||
        (code_point >= 0xd800U && code_point <= 0xdfffU)) {
      return false;
    }
    index += continuation + 1;
  }
  return true;
}

[[nodiscard]] std::string_view without_comment(std::string_view line) {
  bool quoted = false;
  bool literal = false;
  bool escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    auto const character = line[index];
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (character == '\\') {
        escaped = true;
      } else if (character == '"') {
        quoted = false;
      }
      continue;
    }
    if (literal) {
      if (character == '\'') {
        literal = false;
      }
      continue;
    }
    if (character == '"') {
      quoted = true;
    } else if (character == '\'') {
      literal = true;
    } else if (character == '#') {
      return line.substr(0, index);
    }
  }
  return line;
}

[[nodiscard]] int hex_value(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

void append_utf8(std::string& output, std::uint32_t code_point) {
  if (code_point <= 0x7fU) {
    output.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else if (code_point <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
    output.push_back(
        static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
    output.push_back(
        static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
    output.push_back(
        static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
  }
}

[[nodiscard]] std::optional<std::string> parse_quoted_text(
    std::string_view value) {
  if (value.size() < 2) {
    return std::nullopt;
  }
  auto const delimiter = value.front();
  if ((delimiter != '"' && delimiter != '\'') || value.back() != delimiter) {
    return std::nullopt;
  }
  std::string result;
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    auto const character = value[index];
    if (character == delimiter) {
      return std::nullopt;
    }
    if (delimiter == '\'') {
      result.push_back(character);
      continue;
    }
    if (character != '\\') {
      if (static_cast<unsigned char>(character) < 0x20U) {
        return std::nullopt;
      }
      result.push_back(character);
      continue;
    }
    if (++index + 1 >= value.size()) {
      return std::nullopt;
    }
    auto const escaped = value[index];
    switch (escaped) {
      case 'b':
        result.push_back('\b');
        break;
      case 't':
        result.push_back('\t');
        break;
      case 'n':
        result.push_back('\n');
        break;
      case 'f':
        result.push_back('\f');
        break;
      case 'r':
        result.push_back('\r');
        break;
      case '"':
        result.push_back('"');
        break;
      case '\\':
        result.push_back('\\');
        break;
      case 'u':
      case 'U': {
        auto const digits = escaped == 'u' ? 4U : 8U;
        if (index + digits + 1 > value.size()) {
          return std::nullopt;
        }
        std::uint32_t code_point{};
        for (unsigned digit = 0; digit < digits; ++digit) {
          auto const hex = hex_value(value[++index]);
          if (hex < 0) {
            return std::nullopt;
          }
          code_point = (code_point << 4U) | static_cast<std::uint32_t>(hex);
        }
        if (code_point > 0x10ffffU ||
            (code_point >= 0xd800U && code_point <= 0xdfffU)) {
          return std::nullopt;
        }
        append_utf8(result, code_point);
        break;
      }
      default:
        return std::nullopt;
    }
  }
  return result;
}

[[nodiscard]] std::optional<ParsedValue> parse_value(std::string_view input,
                                                     std::size_t line) {
  auto value = trim(input);
  if (value.empty()) {
    return std::nullopt;
  }
  if (value.front() == '"' || value.front() == '\'') {
    auto text = parse_quoted_text(value);
    if (!text.has_value()) {
      return std::nullopt;
    }
    return ParsedValue{.kind = ValueKind::text,
                       .text = std::move(*text),
                       .line = line};
  }
  if (value == "true" || value == "false") {
    return ParsedValue{.kind = ValueKind::boolean,
                       .boolean = value == "true",
                       .line = line};
  }
  if (value.front() == '[' && value.back() == ']') {
    std::vector<std::string> entries;
    auto body = trim(value.substr(1, value.size() - 2));
    while (!body.empty()) {
      bool quoted = false;
      bool literal = false;
      bool escaped = false;
      std::size_t comma = std::string_view::npos;
      for (std::size_t index = 0; index < body.size(); ++index) {
        auto const character = body[index];
        if (quoted) {
          if (escaped) {
            escaped = false;
          } else if (character == '\\') {
            escaped = true;
          } else if (character == '"') {
            quoted = false;
          }
        } else if (literal) {
          if (character == '\'') {
            literal = false;
          }
        } else if (character == '"') {
          quoted = true;
        } else if (character == '\'') {
          literal = true;
        } else if (character == ',') {
          comma = index;
          break;
        }
      }
      auto entry = trim(body.substr(0, comma));
      if (entry.empty()) {
        if (comma == std::string_view::npos) {
          break;
        }
        return std::nullopt;
      }
      auto parsed = parse_quoted_text(entry);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      entries.push_back(std::move(*parsed));
      if (comma == std::string_view::npos) {
        body = {};
      } else {
        body = trim(body.substr(comma + 1));
      }
    }
    return ParsedValue{.kind = ValueKind::text_list,
                       .text_list = std::move(entries),
                       .line = line};
  }

  auto digits = value;
  if (digits.starts_with('+')) {
    digits.remove_prefix(1);
  }
  if (digits.empty()) {
    return std::nullopt;
  }
  std::string normalized;
  normalized.reserve(digits.size());
  for (std::size_t index = 0; index < digits.size(); ++index) {
    auto const character = digits[index];
    if (character >= '0' && character <= '9') {
      normalized.push_back(character);
      continue;
    }
    if (character != '_' || index == 0 || index + 1 == digits.size() ||
        digits[index - 1] < '0' || digits[index - 1] > '9' ||
        digits[index + 1] < '0' || digits[index + 1] > '9') {
      return std::nullopt;
    }
  }
  if (normalized.size() > 1 && normalized.front() == '0') {
    return std::nullopt;
  }
  std::uint64_t integer{};
  auto const* first = normalized.data();
  auto const parsed = std::from_chars(first,
                                      normalized.data() + normalized.size(),
                                      integer);
  if (parsed.ec == std::errc{} &&
      parsed.ptr == normalized.data() + normalized.size()) {
    return ParsedValue{.kind = ValueKind::integer,
                       .integer = integer,
                       .line = line};
  }
  return std::nullopt;
}

[[nodiscard]] bool valid_bare_key(std::string_view key) {
  return !key.empty() && std::ranges::all_of(key, [](char character) {
           return (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') ||
                  character == '_' || character == '-';
         });
}

[[nodiscard]] std::optional<std::string> parse_key(std::string_view value) {
  value = trim(value);
  if (valid_bare_key(value)) {
    return std::string{value};
  }
  return parse_quoted_text(value);
}

[[nodiscard]] std::optional<std::string> localization_locale(
    std::string_view header, std::string_view prefix) {
  if (!header.starts_with(prefix)) {
    return std::nullopt;
  }
  auto suffix = trim(header.substr(prefix.size()));
  if (suffix.empty()) {
    return std::nullopt;
  }
  if (valid_bare_key(suffix)) {
    return std::string{suffix};
  }
  return parse_quoted_text(suffix);
}

class Parser final {
 public:
  explicit Parser(std::string_view bytes) : bytes_(bytes) {}

  [[nodiscard]] ParsedCatalog parse() {
    if (bytes_.starts_with("\xef\xbb\xbf")) {
      bytes_.remove_prefix(3);
    }
    if (!valid_utf8(bytes_)) {
      add_issue(parsed_, catalog::CatalogIssueCode::malformed_toml, "root",
                "catalog must be valid UTF-8");
      return std::move(parsed_);
    }

    current_ = &parsed_.root;
    std::size_t start{};
    std::size_t line_number{1};
    while (start <= bytes_.size()) {
      auto const end = bytes_.find('\n', start);
      auto line = bytes_.substr(
          start, end == std::string_view::npos ? bytes_.size() - start
                                               : end - start);
      parse_line(trim(without_comment(line)), line_number);
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
      ++line_number;
    }
    return std::move(parsed_);
  }

 private:
  void parse_line(std::string_view line, std::size_t line_number) {
    if (line.empty()) {
      return;
    }
    if (line.front() == '[') {
      parse_header(line, line_number);
      return;
    }
    if (current_ == nullptr) {
      add_issue(parsed_, catalog::CatalogIssueCode::malformed_toml,
                "line:" + std::to_string(line_number),
                "assignment follows an unsupported table");
      return;
    }
    bool quoted = false;
    bool literal = false;
    bool escaped = false;
    std::size_t equals = std::string_view::npos;
    for (std::size_t index = 0; index < line.size(); ++index) {
      auto const character = line[index];
      if (quoted) {
        if (escaped) {
          escaped = false;
        } else if (character == '\\') {
          escaped = true;
        } else if (character == '"') {
          quoted = false;
        }
      } else if (literal) {
        if (character == '\'') {
          literal = false;
        }
      } else if (character == '"') {
        quoted = true;
      } else if (character == '\'') {
        literal = true;
      } else if (character == '=') {
        equals = index;
        break;
      }
    }
    if (equals == std::string_view::npos) {
      add_issue(parsed_, catalog::CatalogIssueCode::malformed_toml,
                "line:" + std::to_string(line_number),
                "catalog assignment is missing '='");
      return;
    }
    auto key = parse_key(line.substr(0, equals));
    auto value = parse_value(line.substr(equals + 1), line_number);
    if (!key.has_value() || !value.has_value()) {
      add_issue(parsed_, catalog::CatalogIssueCode::malformed_toml,
                "line:" + std::to_string(line_number),
                "catalog assignment uses unsupported TOML syntax");
      return;
    }
    if (!current_->values.emplace(*key, std::move(*value)).second) {
      add_issue(parsed_, catalog::CatalogIssueCode::duplicate_field,
                current_->location + "." + *key,
                "field is assigned more than once");
    }
  }

  void parse_header(std::string_view line, std::size_t line_number) {
    auto array = line.starts_with("[[") && line.ends_with("]]" );
    auto standard = !array && line.starts_with('[') && line.ends_with(']');
    if (!array && !standard) {
      current_ = nullptr;
      add_issue(parsed_, catalog::CatalogIssueCode::malformed_toml,
                "line:" + std::to_string(line_number),
                "catalog table header is malformed");
      return;
    }
    auto header = trim(array ? line.substr(2, line.size() - 4)
                             : line.substr(1, line.size() - 2));
    if (array) {
      if (header == "categories") {
        auto const index = parsed_.categories.size();
        parsed_.categories.push_back(ParsedCategory{
            .table = {.location = "categories[" + std::to_string(index) + "]",
                      .line = line_number},
        });
        current_ = &parsed_.categories.back().table;
        return;
      }
      if (header == "software") {
        auto const index = parsed_.software.size();
        parsed_.software.push_back(ParsedSoftware{
            .table = {.location = "software[" + std::to_string(index) + "]",
                      .line = line_number},
        });
        current_ = &parsed_.software.back().table;
        return;
      }
      if (header == "software.sources" && !parsed_.software.empty()) {
        auto& software = parsed_.software.back();
        auto const index = software.sources.size();
        software.sources.push_back(ParsedSource{
            .table = {.location = software.table.location + ".sources[" +
                                      std::to_string(index) + "]",
                      .line = line_number},
        });
        current_ = &software.sources.back().table;
        return;
      }
      if (header == "software.sources.history" &&
          !parsed_.software.empty() &&
          !parsed_.software.back().sources.empty()) {
        auto& source = parsed_.software.back().sources.back();
        auto const index = source.history.size();
        source.history.push_back(ParsedTable{
            .location = source.table.location + ".history[" +
                        std::to_string(index) + "]",
            .line = line_number,
        });
        current_ = &source.history.back();
        return;
      }
      if (header == "drivers") {
        auto const index = parsed_.drivers.size();
        parsed_.drivers.push_back(ParsedDriver{
            .table = {.location = "drivers[" + std::to_string(index) + "]",
                      .line = line_number},
        });
        current_ = &parsed_.drivers.back().table;
        return;
      }
      if (header == "drivers.sources" && !parsed_.drivers.empty()) {
        auto& driver = parsed_.drivers.back();
        auto const index = driver.sources.size();
        driver.sources.push_back(ParsedSource{
            .table = {.location = driver.table.location + ".sources[" +
                                      std::to_string(index) + "]",
                      .line = line_number},
        });
        current_ = &driver.sources.back().table;
        return;
      }
      if (header == "drivers.sources.history" && !parsed_.drivers.empty() &&
          !parsed_.drivers.back().sources.empty()) {
        auto& source = parsed_.drivers.back().sources.back();
        auto const index = source.history.size();
        source.history.push_back(ParsedTable{
            .location = source.table.location + ".history[" +
                        std::to_string(index) + "]",
            .line = line_number,
        });
        current_ = &source.history.back();
        return;
      }
    } else {
      if (header == "software.education" && !parsed_.software.empty()) {
        auto& software = parsed_.software.back();
        if (software.education.has_value()) {
          add_issue(parsed_, catalog::CatalogIssueCode::duplicate_field,
                    software.table.location + ".education",
                    "education table is defined more than once");
          current_ = nullptr;
          return;
        }
        software.education = ParsedTable{
            .location = software.table.location + ".education",
            .line = line_number,
        };
        current_ = &*software.education;
        return;
      }
      if (!parsed_.categories.empty()) {
        auto locale = localization_locale(
            header, "categories.localizations.");
        if (locale.has_value()) {
          auto& category = parsed_.categories.back();
          category.localizations.push_back({
              *locale,
              ParsedTable{
                  .location = category.table.location + ".localizations." +
                              *locale,
                  .line = line_number,
              },
          });
          current_ = &category.localizations.back().second;
          return;
        }
      }
      if (!parsed_.software.empty()) {
        auto locale = localization_locale(header, "software.localizations.");
        if (locale.has_value()) {
          auto& software = parsed_.software.back();
          software.localizations.push_back({
              *locale,
              ParsedTable{
                  .location = software.table.location + ".localizations." +
                              *locale,
                  .line = line_number,
              },
          });
          current_ = &software.localizations.back().second;
          return;
        }
      }
      if (!parsed_.drivers.empty()) {
        auto locale = localization_locale(header, "drivers.localizations.");
        if (locale.has_value()) {
          auto& driver = parsed_.drivers.back();
          driver.localizations.push_back({
              *locale,
              ParsedTable{
                  .location = driver.table.location + ".localizations." +
                              *locale,
                  .line = line_number,
              },
          });
          current_ = &driver.localizations.back().second;
          return;
        }
      }
    }
    current_ = nullptr;
    add_issue(parsed_, catalog::CatalogIssueCode::unknown_execution_semantics,
              "line:" + std::to_string(line_number),
              "unknown catalog table may change runtime semantics");
  }

  std::string_view bytes_;
  ParsedCatalog parsed_;
  ParsedTable* current_{nullptr};
};

[[nodiscard]] ParsedValue const* find_value(ParsedTable const& table,
                                            std::string_view key) {
  auto const found = table.values.find(std::string{key});
  return found == table.values.end() ? nullptr : &found->second;
}

[[nodiscard]] std::optional<std::string> text_value(
    ParsedCatalog& parsed, ParsedTable const& table, std::string_view key) {
  auto const* value = find_value(table, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->kind != ValueKind::text) {
    add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
              table.location + "." + std::string{key},
              "field must be a TOML string");
    return std::nullopt;
  }
  return value->text;
}

[[nodiscard]] std::optional<bool> bool_value(ParsedCatalog& parsed,
                                             ParsedTable const& table,
                                             std::string_view key) {
  auto const* value = find_value(table, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->kind != ValueKind::boolean) {
    add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
              table.location + "." + std::string{key},
              "field must be a TOML boolean");
    return std::nullopt;
  }
  return value->boolean;
}

[[nodiscard]] std::optional<std::uint64_t> integer_value(
    ParsedCatalog& parsed, ParsedTable const& table, std::string_view key) {
  auto const* value = find_value(table, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->kind != ValueKind::integer) {
    add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
              table.location + "." + std::string{key},
              "field must be a non-negative TOML integer");
    return std::nullopt;
  }
  return value->integer;
}

[[nodiscard]] std::optional<std::vector<std::string>> text_list_value(
    ParsedCatalog& parsed, ParsedTable const& table, std::string_view key) {
  auto const* value = find_value(table, key);
  if (value == nullptr) {
    return std::nullopt;
  }
  if (value->kind != ValueKind::text_list) {
    add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
              table.location + "." + std::string{key},
              "field must be an array of TOML strings");
    return std::nullopt;
  }
  return value->text_list;
}

[[nodiscard]] bool display_extension_key(std::string_view key) {
  static constexpr std::string_view exact[] = {
      "description", "summary",       "subtitle",    "help_text",
      "release_notes", "documentation", "display_name", "display_text",
  };
  if (std::ranges::find(exact, key) != std::end(exact)) {
    return true;
  }
  return key.starts_with("display_") || key.starts_with("description_") ||
         key.starts_with("x_display_") ||
         key.starts_with("x_description_") || key.starts_with("x_note_") ||
         key.ends_with("_label") || key.ends_with("_summary") ||
         key.ends_with("_description") || key.ends_with("_notice") ||
         key.ends_with("_text");
}

void collect_extensions(ParsedCatalog& parsed, ParsedTable const& table,
                        std::set<std::string> const& allowed,
                        std::vector<catalog::DisplayExtension>& extensions) {
  for (auto const& [key, value] : table.values) {
    if (allowed.contains(key)) {
      continue;
    }
    if (!display_extension_key(key) ||
        (value.kind != ValueKind::text &&
         value.kind != ValueKind::text_list)) {
      add_issue(parsed,
                catalog::CatalogIssueCode::unknown_execution_semantics,
                table.location + "." + key,
                "unknown field is not an ignorable display extension");
      continue;
    }
    extensions.push_back(catalog::DisplayExtension{
        .key = key,
        .text = value.text,
        .text_list = value.text_list,
        .list = value.kind == ValueKind::text_list,
    });
  }
  std::ranges::sort(extensions, {}, &catalog::DisplayExtension::key);
}

[[nodiscard]] std::optional<catalog::SoftwareTier> parse_tier(
    ParsedCatalog& parsed, ParsedTable const& table) {
  auto value = text_value(parsed, table, "tier");
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (*value == "basic") {
    return catalog::SoftwareTier::basic;
  }
  if (*value == "normal") {
    return catalog::SoftwareTier::normal;
  }
  add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
            table.location + ".tier", "tier must be basic or normal");
  return std::nullopt;
}

[[nodiscard]] std::optional<catalog::VersionPolicy> parse_version_policy(
    ParsedCatalog& parsed, ParsedTable const& table) {
  auto value = text_value(parsed, table, "version_policy");
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (*value == "latest_stable") {
    return catalog::VersionPolicy::latest_stable;
  }
  if (*value == "latest_stable_with_history") {
    return catalog::VersionPolicy::latest_stable_with_history;
  }
  if (*value == "fixed") {
    return catalog::VersionPolicy::fixed;
  }
  if (*value == "maintainer_provided") {
    return catalog::VersionPolicy::maintainer_provided;
  }
  add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
            table.location + ".version_policy",
            "version_policy value is not recognized");
  return std::nullopt;
}

[[nodiscard]] std::optional<catalog::SourcePurpose> parse_source_purpose(
    ParsedCatalog& parsed, ParsedTable const& table) {
  auto value = text_value(parsed, table, "purpose");
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (*value == "primary") {
    return catalog::SourcePurpose::primary;
  }
  if (*value == "alternative") {
    return catalog::SourcePurpose::alternative;
  }
  if (*value == "project_backup") {
    return catalog::SourcePurpose::project_backup;
  }
  add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
            table.location + ".purpose",
            "source purpose value is not recognized");
  return std::nullopt;
}

[[nodiscard]] std::optional<catalog::DriverEntryType> parse_driver_type(
    ParsedCatalog& parsed, ParsedTable const& table) {
  auto value = text_value(parsed, table, "entry_type");
  if (!value.has_value()) {
    return std::nullopt;
  }
  if (*value == "assistant") {
    return catalog::DriverEntryType::assistant;
  }
  if (*value == "vendor_page") {
    return catalog::DriverEntryType::vendor_page;
  }
  add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
            table.location + ".entry_type",
            "driver entry_type must be assistant or vendor_page");
  return std::nullopt;
}

[[nodiscard]] catalog::CatalogLocalization convert_localization(
    ParsedCatalog& parsed, std::string locale, ParsedTable const& table,
    std::set<std::string> allowed) {
  catalog::CatalogLocalization value{
      .locale = std::move(locale),
      .name = text_value(parsed, table, "name"),
      .notice = text_value(parsed, table, "notice"),
      .optimization_note = text_value(parsed, table, "optimization_note"),
      .education_description =
          text_value(parsed, table, "education_description"),
  };
  collect_extensions(parsed, table, allowed, value.display_extensions);
  return value;
}

[[nodiscard]] catalog::CatalogSource convert_source(ParsedCatalog& parsed,
                                                    ParsedSource const& input) {
  catalog::CatalogSource source{
      .purpose = parse_source_purpose(parsed, input.table),
      .address = text_value(parsed, input.table, "address").value_or(""),
      .version = text_value(parsed, input.table, "version"),
  };
  collect_extensions(parsed, input.table,
                     {"purpose", "address", "version"},
                     source.display_extensions);
  for (auto const& history_table : input.history) {
    catalog::SourceHistory history{
        .version = text_value(parsed, history_table, "version").value_or(""),
        .address = text_value(parsed, history_table, "address").value_or(""),
        .reason = text_value(parsed, history_table, "reason").value_or(""),
        .visible = bool_value(parsed, history_table, "visible").value_or(true),
    };
    collect_extensions(parsed, history_table,
                       {"version", "address", "reason", "visible"},
                       history.display_extensions);
    source.history.push_back(std::move(history));
  }
  return source;
}

[[nodiscard]] app_catalog::CatalogDecodeResult convert(ParsedCatalog parsed) {
  catalog::SoftwareCatalogDocument document;
  auto schema = integer_value(parsed, parsed.root, "schema_version");
  if (schema.has_value() &&
      *schema <= std::numeric_limits<std::uint32_t>::max()) {
    document.schema_version = static_cast<std::uint32_t>(*schema);
  } else if (schema.has_value()) {
    add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
              "schema_version", "schema_version is outside uint32 range");
  }
  document.catalog_id =
      text_value(parsed, parsed.root, "catalog_id").value_or("");
  document.revision = integer_value(parsed, parsed.root, "revision").value_or(0);
  auto release_state = text_value(parsed, parsed.root, "release_state");
  if (release_state == "draft") {
    document.release_state = catalog::ReleaseState::draft;
  } else if (release_state == "release") {
    document.release_state = catalog::ReleaseState::release;
  } else if (release_state.has_value()) {
    add_issue(parsed, catalog::CatalogIssueCode::invalid_field,
              "release_state", "release_state must be draft or release");
  }
  document.default_locale =
      text_value(parsed, parsed.root, "default_locale").value_or("");
  collect_extensions(parsed, parsed.root,
                     {"schema_version", "catalog_id", "revision",
                      "release_state", "default_locale"},
                     document.display_extensions);

  for (auto const& input : parsed.categories) {
    catalog::CatalogCategory category{
        .id = text_value(parsed, input.table, "id").value_or(""),
        .name = text_value(parsed, input.table, "name").value_or(""),
    };
    collect_extensions(parsed, input.table, {"id", "name"},
                       category.display_extensions);
    for (auto const& [locale, table] : input.localizations) {
      category.localizations.push_back(convert_localization(
          parsed, locale, table, {"name"}));
    }
    document.categories.push_back(std::move(category));
  }

  for (auto const& input : parsed.software) {
    auto enabled = bool_value(parsed, input.table, "enabled");
    auto dependencies = text_list_value(parsed, input.table, "dependencies");
    auto bundled = text_list_value(parsed, input.table, "bundled_editions");
    catalog::SoftwareDefinition software{
        .id = text_value(parsed, input.table, "id").value_or(""),
        .enabled = enabled.value_or(false),
        .enabled_declared = enabled.has_value(),
        .name = text_value(parsed, input.table, "name").value_or(""),
        .tier = parse_tier(parsed, input.table),
        .category_id =
            text_value(parsed, input.table, "category_id").value_or(""),
        .branch = text_value(parsed, input.table, "branch").value_or(""),
        .version_policy = parse_version_policy(parsed, input.table),
        .fixed_version = text_value(parsed, input.table, "fixed_version"),
        .dependencies = dependencies.value_or(std::vector<std::string>{}),
        .dependencies_declared = dependencies.has_value(),
        .bundled_editions = bundled.value_or(std::vector<std::string>{}),
        .bundled_editions_declared = bundled.has_value(),
        .notice = text_value(parsed, input.table, "notice").value_or(""),
        .optimization_note =
            text_value(parsed, input.table, "optimization_note"),
        .install_profile = text_value(parsed, input.table, "install_profile"),
    };
    collect_extensions(
        parsed, input.table,
        {"id", "enabled", "name", "tier", "category_id", "branch",
         "version_policy", "fixed_version", "dependencies",
         "bundled_editions", "notice", "optimization_note",
         "install_profile"},
        software.display_extensions);
    for (auto const& source : input.sources) {
      software.sources.push_back(convert_source(parsed, source));
    }
    if (input.education.has_value()) {
      software.education = catalog::EducationResource{
          .address = text_value(parsed, *input.education, "address")
                         .value_or(""),
          .description = text_value(parsed, *input.education, "description")
                             .value_or(""),
      };
      collect_extensions(parsed, *input.education,
                         {"address", "description"},
                         software.education->display_extensions);
    }
    for (auto const& [locale, table] : input.localizations) {
      software.localizations.push_back(convert_localization(
          parsed, locale, table,
          {"name", "notice", "optimization_note",
           "education_description"}));
    }
    document.software.push_back(std::move(software));
  }

  for (auto const& input : parsed.drivers) {
    auto enabled = bool_value(parsed, input.table, "enabled");
    auto hardware = text_list_value(parsed, input.table, "hardware_kinds");
    catalog::DriverDefinition driver{
        .id = text_value(parsed, input.table, "id").value_or(""),
        .enabled = enabled.value_or(false),
        .enabled_declared = enabled.has_value(),
        .name = text_value(parsed, input.table, "name").value_or(""),
        .entry_type = parse_driver_type(parsed, input.table),
        .hardware_kinds = hardware.value_or(std::vector<std::string>{}),
        .hardware_kinds_declared = hardware.has_value(),
        .branch = text_value(parsed, input.table, "branch").value_or(""),
        .version_policy = parse_version_policy(parsed, input.table),
        .fixed_version = text_value(parsed, input.table, "fixed_version"),
        .notice = text_value(parsed, input.table, "notice").value_or(""),
    };
    collect_extensions(
        parsed, input.table,
        {"id", "enabled", "name", "entry_type", "hardware_kinds",
         "branch", "version_policy", "fixed_version", "notice"},
        driver.display_extensions);
    for (auto const& source : input.sources) {
      driver.sources.push_back(convert_source(parsed, source));
    }
    for (auto const& [locale, table] : input.localizations) {
      driver.localizations.push_back(convert_localization(
          parsed, locale, table, {"name", "notice"}));
    }
    document.drivers.push_back(std::move(driver));
  }

  return app_catalog::CatalogDecodeResult{
      .document = std::move(document),
      .issues = std::move(parsed.issues),
  };
}

[[nodiscard]] std::string quoted(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string output{"\""};
  for (auto const byte : value) {
    auto const character = static_cast<unsigned char>(byte);
    switch (character) {
      case '\b':
        output += "\\b";
        break;
      case '\t':
        output += "\\t";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\r':
        output += "\\r";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      default:
        if (character < 0x20U || character == 0x7fU) {
          output += "\\u00";
          output.push_back(hex[character >> 4U]);
          output.push_back(hex[character & 0x0fU]);
        } else {
          output.push_back(static_cast<char>(character));
        }
        break;
    }
  }
  output.push_back('"');
  return output;
}

void write_text(std::ostringstream& output, std::string_view key,
                std::string_view value) {
  output << key << " = " << quoted(value) << '\n';
}

void write_list(std::ostringstream& output, std::string_view key,
                std::vector<std::string> const& values) {
  output << key << " = [";
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }
    output << quoted(values[index]);
  }
  output << "]\n";
}

void write_extensions(std::ostringstream& output,
                      std::vector<catalog::DisplayExtension> const& values) {
  for (auto const& value : values) {
    if (value.list) {
      write_list(output, value.key, value.text_list);
    } else {
      write_text(output, value.key, value.text);
    }
  }
}

[[nodiscard]] std::string_view release_state_name(catalog::ReleaseState value) {
  return value == catalog::ReleaseState::release ? "release" : "draft";
}

[[nodiscard]] std::string_view tier_name(catalog::SoftwareTier value) {
  return value == catalog::SoftwareTier::basic ? "basic" : "normal";
}

[[nodiscard]] std::string_view version_policy_name(
    catalog::VersionPolicy value) {
  switch (value) {
    case catalog::VersionPolicy::latest_stable:
      return "latest_stable";
    case catalog::VersionPolicy::latest_stable_with_history:
      return "latest_stable_with_history";
    case catalog::VersionPolicy::fixed:
      return "fixed";
    case catalog::VersionPolicy::maintainer_provided:
      return "maintainer_provided";
  }
  return "latest_stable";
}

[[nodiscard]] std::string_view purpose_name(catalog::SourcePurpose value) {
  switch (value) {
    case catalog::SourcePurpose::primary:
      return "primary";
    case catalog::SourcePurpose::alternative:
      return "alternative";
    case catalog::SourcePurpose::project_backup:
      return "project_backup";
  }
  return "primary";
}

[[nodiscard]] std::string_view driver_type_name(
    catalog::DriverEntryType value) {
  return value == catalog::DriverEntryType::assistant ? "assistant"
                                                       : "vendor_page";
}

void write_localization(std::ostringstream& output, std::string_view owner,
                        catalog::CatalogLocalization const& localization) {
  output << '\n'
         << '[' << owner << ".localizations." << quoted(localization.locale)
         << "]\n";
  if (localization.name.has_value()) {
    write_text(output, "name", *localization.name);
  }
  if (localization.notice.has_value()) {
    write_text(output, "notice", *localization.notice);
  }
  if (localization.optimization_note.has_value()) {
    write_text(output, "optimization_note",
               *localization.optimization_note);
  }
  if (localization.education_description.has_value()) {
    write_text(output, "education_description",
               *localization.education_description);
  }
  write_extensions(output, localization.display_extensions);
}

void write_sources(std::ostringstream& output, std::string_view owner,
                   std::vector<catalog::CatalogSource> const& sources) {
  for (auto const& source : sources) {
    output << '\n' << "[[" << owner << ".sources]]\n";
    if (source.purpose.has_value()) {
      write_text(output, "purpose", purpose_name(*source.purpose));
    }
    if (!source.address.empty()) {
      write_text(output, "address", source.address);
    }
    if (source.version.has_value()) {
      write_text(output, "version", *source.version);
    }
    write_extensions(output, source.display_extensions);
    for (auto const& history : source.history) {
      output << '\n' << "[[" << owner << ".sources.history]]\n";
      write_text(output, "version", history.version);
      write_text(output, "address", history.address);
      write_text(output, "reason", history.reason);
      output << "visible = " << (history.visible ? "true" : "false") << '\n';
      write_extensions(output, history.display_extensions);
    }
  }
}

}  // namespace

app_catalog::CatalogDecodeResult TomlSoftwareCatalogCodec::decode(
    std::string_view bytes) const {
  return convert(Parser{bytes}.parse());
}

std::string TomlSoftwareCatalogCodec::encode(
    catalog::SoftwareCatalogDocument const& document) const {
  std::ostringstream output;
  output << "schema_version = " << document.schema_version << '\n';
  write_text(output, "catalog_id", document.catalog_id);
  output << "revision = " << document.revision << '\n';
  if (document.release_state.has_value()) {
    write_text(output, "release_state",
               release_state_name(*document.release_state));
  }
  write_text(output, "default_locale", document.default_locale);
  write_extensions(output, document.display_extensions);

  for (auto const& category : document.categories) {
    output << "\n[[categories]]\n";
    write_text(output, "id", category.id);
    write_text(output, "name", category.name);
    write_extensions(output, category.display_extensions);
    for (auto const& localization : category.localizations) {
      write_localization(output, "categories", localization);
    }
  }

  for (auto const& software : document.software) {
    output << "\n[[software]]\n";
    write_text(output, "id", software.id);
    if (software.enabled_declared) {
      output << "enabled = " << (software.enabled ? "true" : "false") << '\n';
    }
    write_text(output, "name", software.name);
    if (software.tier.has_value()) {
      write_text(output, "tier", tier_name(*software.tier));
    }
    if (!software.category_id.empty()) {
      write_text(output, "category_id", software.category_id);
    }
    if (!software.branch.empty()) {
      write_text(output, "branch", software.branch);
    }
    if (software.version_policy.has_value()) {
      write_text(output, "version_policy",
                 version_policy_name(*software.version_policy));
    }
    if (software.fixed_version.has_value()) {
      write_text(output, "fixed_version", *software.fixed_version);
    }
    if (software.dependencies_declared) {
      write_list(output, "dependencies", software.dependencies);
    }
    if (software.bundled_editions_declared) {
      write_list(output, "bundled_editions", software.bundled_editions);
    }
    write_text(output, "notice", software.notice);
    if (software.optimization_note.has_value()) {
      write_text(output, "optimization_note", *software.optimization_note);
    }
    if (software.install_profile.has_value()) {
      write_text(output, "install_profile", *software.install_profile);
    }
    write_extensions(output, software.display_extensions);
    write_sources(output, "software", software.sources);
    if (software.education.has_value()) {
      output << "\n[software.education]\n";
      write_text(output, "address", software.education->address);
      write_text(output, "description", software.education->description);
      write_extensions(output, software.education->display_extensions);
    }
    for (auto const& localization : software.localizations) {
      write_localization(output, "software", localization);
    }
  }

  for (auto const& driver : document.drivers) {
    output << "\n[[drivers]]\n";
    write_text(output, "id", driver.id);
    if (driver.enabled_declared) {
      output << "enabled = " << (driver.enabled ? "true" : "false") << '\n';
    }
    write_text(output, "name", driver.name);
    if (driver.entry_type.has_value()) {
      write_text(output, "entry_type", driver_type_name(*driver.entry_type));
    }
    if (driver.hardware_kinds_declared) {
      write_list(output, "hardware_kinds", driver.hardware_kinds);
    }
    if (!driver.branch.empty()) {
      write_text(output, "branch", driver.branch);
    }
    if (driver.version_policy.has_value()) {
      write_text(output, "version_policy",
                 version_policy_name(*driver.version_policy));
    }
    if (driver.fixed_version.has_value()) {
      write_text(output, "fixed_version", *driver.fixed_version);
    }
    write_text(output, "notice", driver.notice);
    write_extensions(output, driver.display_extensions);
    write_sources(output, "drivers", driver.sources);
    for (auto const& localization : driver.localizations) {
      write_localization(output, "drivers", localization);
    }
  }
  return output.str();
}

}  // namespace azzs::adapters::infrastructure
