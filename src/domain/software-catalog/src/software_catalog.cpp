#include "azzs/domain/software_catalog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace azzs::domain::software_catalog {
namespace {

[[nodiscard]] bool ascii_lower(char value) noexcept {
  return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool ascii_upper(char value) noexcept {
  return value >= 'A' && value <= 'Z';
}

[[nodiscard]] bool ascii_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

void add_issue(std::vector<CatalogIssue>& issues, CatalogIssueScope scope,
               CatalogIssueCode code, std::string location,
               std::string item_id, std::string message) {
  issues.push_back(CatalogIssue{
      .scope = scope,
      .code = code,
      .location = std::move(location),
      .item_id = std::move(item_id),
      .message = std::move(message),
  });
}

[[nodiscard]] bool has_package_error(
    std::vector<CatalogIssue> const& issues) {
  return std::ranges::any_of(issues, [](CatalogIssue const& issue) {
    return issue.scope == CatalogIssueScope::package;
  });
}

[[nodiscard]] InstallProfileSupport const* find_profile(
    SoftwareCatalogPolicy const& policy, std::string_view id) {
  auto const found = std::ranges::find(
      policy.install_profiles, id, &InstallProfileSupport::id);
  return found == policy.install_profiles.end() ? nullptr : &*found;
}

[[nodiscard]] bool profile_applies(InstallProfileSupport const& profile,
                                   std::string_view software_id) {
  return profile.runtime_status == InstallProfileRuntimeStatus::available &&
         std::ranges::find(profile.software_ids, software_id) !=
             profile.software_ids.end();
}

[[nodiscard]] CatalogSource const* find_primary_source(
    std::vector<CatalogSource> const& sources) {
  auto const found = std::ranges::find_if(
      sources, [](CatalogSource const& source) {
        return source.purpose == SourcePurpose::primary;
      });
  return found == sources.end() ? nullptr : &*found;
}

void validate_localizations(std::vector<CatalogLocalization> const& values,
                            std::string const& location,
                            std::string_view default_locale,
                            std::vector<CatalogIssue>& issues) {
  std::unordered_set<std::string> locales;
  for (auto const& value : values) {
    auto const value_location = location + ".localizations." + value.locale;
    if (!valid_locale(value.locale)) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::invalid_field, value_location, {},
                "localization locale is invalid");
    } else if (value.locale == default_locale ||
               !locales.insert(value.locale).second) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::duplicate_field, value_location, {},
                "localization locale is duplicated");
    }
  }
}

void validate_source(CatalogSource const& source, std::string const& location,
                     std::string const& item_id, bool require_complete,
                     std::vector<CatalogIssue>& issues) {
  if (!source.address.empty() && !valid_http_address(source.address)) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::invalid_field, location + ".address", item_id,
              "catalog sources accept only HTTP or HTTPS addresses");
  }
  if (require_complete && !source.purpose.has_value()) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::missing_required_field,
              location + ".purpose", item_id,
              "enabled entries require a source purpose");
  }
  if (require_complete && source.address.empty()) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::missing_required_field,
              location + ".address", item_id,
              "enabled entries require a source address");
  }
  if (require_complete && source.purpose == SourcePurpose::project_backup &&
      (!source.version.has_value() || source.version->empty())) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::missing_required_field,
              location + ".version", item_id,
              "project backup sources require an explicit version");
  }
  for (std::size_t index = 0; index < source.history.size(); ++index) {
    auto const& history = source.history[index];
    auto const history_location =
        location + ".history[" + std::to_string(index) + "]";
    auto const invalid_address =
        !history.address.empty() && !valid_http_address(history.address);
    if (invalid_address ||
        (require_complete &&
         (history.version.empty() || history.reason.empty() ||
          history.address.empty()))) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::invalid_field, history_location, item_id,
                "source history requires version, HTTP(S) address, and reason");
    }
  }
}

void validate_enabled_sources(std::vector<CatalogSource> const& sources,
                              std::string const& location,
                              std::string const& item_id,
                              std::vector<CatalogIssue>& issues) {
  std::size_t primary_count{};
  for (std::size_t index = 0; index < sources.size(); ++index) {
    auto const& source = sources[index];
    validate_source(source,
                    location + ".sources[" + std::to_string(index) + "]",
                    item_id, true, issues);
    if (source.purpose == SourcePurpose::primary) {
      ++primary_count;
    }
  }
  if (primary_count != 1) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::missing_required_field,
              location + ".sources", item_id,
              "enabled entries require exactly one primary source");
  }
}

void validate_reference_list(std::vector<std::string> const& values,
                             std::string const& location,
                             std::string const& item_id,
                             std::vector<CatalogIssue>& issues) {
  std::unordered_set<std::string> seen;
  for (auto const& value : values) {
    if (!valid_stable_id(value)) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::invalid_reference, location, item_id,
                "reference lists accept only stable identifiers");
    } else if (!seen.insert(value).second) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::duplicate_field, location, item_id,
                "reference lists must not repeat one identifier");
    }
  }
}

[[nodiscard]] bool dependency_issue(CatalogIssue const& issue) noexcept {
  return issue.code == CatalogIssueCode::missing_dependency ||
         issue.code == CatalogIssueCode::dependency_cycle ||
         issue.code == CatalogIssueCode::unavailable_dependency;
}

[[nodiscard]] bool prohibited_resource_identity(std::string_view value) {
  if (value.find("破解补丁") != std::string_view::npos ||
      value.find("破解") != std::string_view::npos ||
      value.find("授权绕过") != std::string_view::npos) {
    return true;
  }

  std::string normalized;
  normalized.reserve(value.size() + 2U);
  normalized.push_back(' ');
  for (auto const character : value) {
    if (ascii_upper(character)) {
      normalized.push_back(static_cast<char>(character - 'A' + 'a'));
    } else if (ascii_lower(character) || ascii_digit(character)) {
      normalized.push_back(character);
    } else {
      normalized.push_back(' ');
    }
  }
  normalized.push_back(' ');
  return normalized.find(" crack ") != std::string::npos ||
         normalized.find(" keygen ") != std::string::npos ||
         normalized.find(" cracking patch ") != std::string::npos ||
         normalized.find(" authorization bypass ") != std::string::npos;
}

void validate_prohibited_catalog_content(
    SoftwareCatalogDocument const& document, std::vector<CatalogIssue>& issues) {
  bool found{};
  auto check_resource = [&](std::string_view value, std::string location,
                            std::string item_id) {
    if (!found && prohibited_resource_identity(value)) {
      found = true;
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::prohibited_content, std::move(location),
                std::move(item_id),
                "catalog contains content prohibited by SEC-08");
    }
  };
  auto check_sources = [&](std::vector<CatalogSource> const& values,
                           std::string_view location,
                           std::string_view item_id) {
    for (std::size_t index = 0; index < values.size(); ++index) {
      auto const& source = values[index];
      auto const source_location =
          std::string{location} + ".sources[" + std::to_string(index) + "]";
      check_resource(source.address, source_location + ".address",
                     std::string{item_id});
      if (source.version.has_value()) {
        check_resource(*source.version, source_location + ".version",
                       std::string{item_id});
      }
      for (std::size_t history_index = 0; history_index < source.history.size();
           ++history_index) {
        auto const& history = source.history[history_index];
        auto const history_location = source_location + ".history[" +
                                      std::to_string(history_index) + "]";
        check_resource(history.version, history_location + ".version",
                       std::string{item_id});
        check_resource(history.address, history_location + ".address",
                       std::string{item_id});
      }
    }
  };

  for (std::size_t index = 0; index < document.software.size(); ++index) {
    auto const& software = document.software[index];
    auto const location = "software[" + std::to_string(index) + "]";
    check_resource(software.id, location + ".id", software.id);
    check_resource(software.name, location + ".name", software.id);
    check_resource(software.branch, location + ".branch", software.id);
    if (software.fixed_version.has_value()) {
      check_resource(*software.fixed_version, location + ".fixed_version",
                     software.id);
    }
    check_sources(software.sources, location, software.id);
    if (software.education.has_value()) {
      check_resource(software.education->address,
                     location + ".education.address", software.id);
    }
  }
  for (std::size_t index = 0; index < document.drivers.size(); ++index) {
    auto const& driver = document.drivers[index];
    auto const location = "drivers[" + std::to_string(index) + "]";
    check_resource(driver.id, location + ".id", driver.id);
    check_resource(driver.name, location + ".name", driver.id);
    check_resource(driver.branch, location + ".branch", driver.id);
    if (driver.fixed_version.has_value()) {
      check_resource(*driver.fixed_version, location + ".fixed_version",
                     driver.id);
    }
    check_sources(driver.sources, location, driver.id);
  }
}

template <typename RuntimeItem>
void analyze_dependencies(std::vector<RuntimeItem>& items,
                          std::string_view item_kind,
                          std::vector<CatalogIssue>& issues) {
  std::unordered_map<std::string, std::size_t> indices;
  indices.reserve(items.size());
  for (std::size_t index = 0; index < items.size(); ++index) {
    indices.emplace(items[index].definition.id, index);
  }

  auto disable = [&](std::size_t index, ItemAvailability availability,
                     CatalogIssueCode code, std::string reason) {
    auto& item = items[index];
    if (item.availability == ItemAvailability::available ||
        availability == ItemAvailability::dependency_cycle) {
      item.availability = availability;
    }
    if (std::ranges::find(item.reasons, reason) == item.reasons.end()) {
      item.reasons.push_back(reason);
      add_issue(issues, CatalogIssueScope::item, code,
                std::string{item_kind} + "." + item.definition.id,
                item.definition.id, std::move(reason));
    }
  };

  for (std::size_t index = 0; index < items.size(); ++index) {
    for (auto const& dependency : items[index].definition.dependencies) {
      if (!indices.contains(dependency)) {
        disable(index, ItemAvailability::missing_dependency,
                CatalogIssueCode::missing_dependency,
                "required dependency '" + dependency + "' is missing");
      }
    }
  }

  std::vector<int> discovery(items.size(), -1);
  std::vector<int> low_link(items.size(), -1);
  std::vector<std::size_t> stack;
  std::vector<bool> on_stack(items.size(), false);
  int next_discovery{};
  std::function<void(std::size_t)> visit = [&](std::size_t index) {
    discovery[index] = next_discovery;
    low_link[index] = next_discovery;
    ++next_discovery;
    stack.push_back(index);
    on_stack[index] = true;

    for (auto const& dependency : items[index].definition.dependencies) {
      auto const found = indices.find(dependency);
      if (found == indices.end()) {
        continue;
      }
      auto const target = found->second;
      if (discovery[target] < 0) {
        visit(target);
        low_link[index] = std::min(low_link[index], low_link[target]);
      } else if (on_stack[target]) {
        low_link[index] = std::min(low_link[index], discovery[target]);
      }
    }

    if (low_link[index] != discovery[index]) {
      return;
    }
    std::vector<std::size_t> component;
    while (!stack.empty()) {
      auto const member = stack.back();
      stack.pop_back();
      on_stack[member] = false;
      component.push_back(member);
      if (member == index) {
        break;
      }
    }
    auto cyclic = component.size() > 1;
    if (!cyclic && !component.empty()) {
      auto const& dependencies =
          items[component.front()].definition.dependencies;
      cyclic = std::ranges::find(
                   dependencies, items[component.front()].definition.id) !=
               dependencies.end();
    }
    if (cyclic) {
      for (auto const member : component) {
        disable(member, ItemAvailability::dependency_cycle,
                CatalogIssueCode::dependency_cycle,
                "dependency cycle includes '" +
                    items[member].definition.id + "'");
      }
    }
  };
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (discovery[index] < 0) {
      visit(index);
    }
  }

  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = 0; index < items.size(); ++index) {
      if (items[index].availability != ItemAvailability::available) {
        continue;
      }
      for (auto const& dependency : items[index].definition.dependencies) {
        auto const found = indices.find(dependency);
        if (found != indices.end() &&
            items[found->second].availability != ItemAvailability::available) {
          disable(index, ItemAvailability::unavailable_dependency,
                  CatalogIssueCode::unavailable_dependency,
                  "required dependency '" + dependency +
                      "' is unavailable");
          changed = true;
          break;
        }
      }
    }
  }
}

}  // namespace

bool valid_stable_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 64 || !ascii_lower(value.front())) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return ascii_lower(character) || ascii_digit(character) ||
           character == '-' || character == '_' || character == '.';
  });
}

bool valid_locale(std::string_view value) noexcept {
  if (value.size() < 2 || value.size() > 35 ||
      !(ascii_lower(value.front()) || ascii_upper(value.front()))) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return ascii_lower(character) || ascii_upper(character) ||
           ascii_digit(character) || character == '-';
  });
}

bool valid_http_address(std::string_view value) noexcept {
  auto const supported = value.starts_with("https://") ||
                         value.starts_with("http://");
  if (!supported || value.size() > 4096) {
    return false;
  }
  auto const authority = value.find("//") + 2;
  auto const authority_end = value.find_first_of("/?#", authority);
  if (authority >= value.size() || authority_end == authority) {
    return false;
  }
  return std::ranges::none_of(value, [](unsigned char character) {
    return character < 0x20 || character == 0x7f ||
           std::isspace(character) != 0;
  });
}

RuntimeCatalogLoad validate_for_runtime(
    SoftwareCatalogDocument const& document,
    SoftwareCatalogPolicy const& policy) {
  RuntimeCatalogLoad result;
  auto& issues = result.issues;

  validate_prohibited_catalog_content(document, issues);

  if (document.schema_version != policy.supported_schema_version) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::unsupported_schema, "schema_version", {},
              "catalog schema version is not supported by this workbench");
  }
  if (document.catalog_id != "software") {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::invalid_field, "catalog_id", {},
              "software and driver catalog_id must be 'software'");
  }
  if (document.revision == 0) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::missing_required_field, "revision", {},
              "catalog revision must be a positive integer");
  }
  if (!document.release_state.has_value()) {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::missing_required_field, "release_state", {},
              "catalog release_state is required");
  }
  if (document.default_locale != "zh-CN") {
    add_issue(issues, CatalogIssueScope::package,
              CatalogIssueCode::invalid_field, "default_locale", {},
              "schema version 1 requires default_locale 'zh-CN'");
  }

  std::unordered_set<std::string> category_ids;
  for (std::size_t index = 0; index < document.categories.size(); ++index) {
    auto const& category = document.categories[index];
    auto const location = "categories[" + std::to_string(index) + "]";
    if (!valid_stable_id(category.id) || category.name.empty()) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field, location,
                category.id,
                "categories require a stable id and default name");
    } else if (!category_ids.insert(category.id).second) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::duplicate_stable_id, location + ".id",
                category.id, "category stable id is duplicated");
    }
    validate_localizations(category.localizations, location,
                           document.default_locale, issues);
  }

  std::unordered_set<std::string> item_ids;
  for (std::size_t index = 0; index < document.software.size(); ++index) {
    auto const& software = document.software[index];
    auto const location = "software[" + std::to_string(index) + "]";
    if (!valid_stable_id(software.id) || !software.enabled_declared) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field, location,
                software.id,
                "software entries require a stable id and enabled flag");
    } else if (!item_ids.insert(software.id).second) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::duplicate_stable_id, location + ".id",
                software.id, "catalog item stable id is duplicated");
    }
    validate_localizations(software.localizations, location,
                           document.default_locale, issues);
    validate_reference_list(software.dependencies,
                            location + ".dependencies", software.id, issues);
    std::unordered_set<std::string> bundled_editions;
    for (auto const& edition : software.bundled_editions) {
      if (edition != "large_offline") {
        add_issue(issues, CatalogIssueScope::package,
                  CatalogIssueCode::unknown_execution_semantics,
                  location + ".bundled_editions", software.id,
                  "bundled edition semantics are not recognized");
      } else if (!bundled_editions.insert(edition).second) {
        add_issue(issues, CatalogIssueScope::package,
                  CatalogIssueCode::duplicate_field,
                  location + ".bundled_editions", software.id,
                  "bundled editions must not repeat one value");
      }
    }
    if (software.install_profile.has_value()) {
      auto const* profile = find_profile(policy, *software.install_profile);
      if (!valid_stable_id(*software.install_profile)) {
        add_issue(issues, CatalogIssueScope::package,
                  CatalogIssueCode::invalid_field,
                  location + ".install_profile", software.id,
                  "install profile reference must be a stable identifier");
      } else if (profile != nullptr &&
                 profile->runtime_status ==
                     InstallProfileRuntimeStatus::unknown_semantics) {
        add_issue(issues, CatalogIssueScope::package,
                  CatalogIssueCode::unknown_execution_semantics,
                  location + ".install_profile", software.id,
                  "install profile execution semantics are unavailable");
      }
    }
    if (software.enabled) {
      auto const required_profile = std::ranges::find(
          policy.required_install_profiles, software.id,
          &RequiredInstallProfile::software_id);
      if (required_profile != policy.required_install_profiles.end()) {
        if (!software.install_profile.has_value()) {
          add_issue(issues, CatalogIssueScope::package,
                    CatalogIssueCode::missing_required_field,
                    location + ".install_profile", software.id,
                    "software requires its controlled install profile");
        } else if (*software.install_profile != required_profile->profile_id) {
          add_issue(issues, CatalogIssueScope::package,
                    CatalogIssueCode::invalid_field,
                    location + ".install_profile", software.id,
                    "software references the wrong controlled install profile");
        }
      }
    }
    for (std::size_t source_index = 0;
         source_index < software.sources.size(); ++source_index) {
      validate_source(software.sources[source_index],
                      location + ".sources[" +
                          std::to_string(source_index) + "]",
                      software.id, software.enabled, issues);
    }
    if (!software.enabled) {
      continue;
    }
    if (software.name.empty() || !software.tier.has_value() ||
        software.category_id.empty() ||
        software.branch.empty() || !software.version_policy.has_value() ||
        !software.dependencies_declared ||
        !software.bundled_editions_declared) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field, location,
                software.id,
                "enabled software is missing required product fields");
    }
    if (!software.category_id.empty() &&
        !category_ids.contains(software.category_id)) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::invalid_reference,
                location + ".category_id", software.id,
                "enabled software references a missing category");
    }
    if (software.version_policy == VersionPolicy::fixed &&
        (!software.fixed_version.has_value() ||
         software.fixed_version->empty())) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field,
                location + ".fixed_version", software.id,
                "fixed version policy requires fixed_version");
    }
    validate_enabled_sources(software.sources, location, software.id, issues);
    if (software.education.has_value() &&
        (!valid_http_address(software.education->address) ||
         software.education->description.empty())) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::invalid_field, location + ".education",
                software.id,
                "education resource requires HTTP(S) address and description");
    }
  }

  for (std::size_t index = 0; index < document.drivers.size(); ++index) {
    auto const& driver = document.drivers[index];
    auto const location = "drivers[" + std::to_string(index) + "]";
    if (!valid_stable_id(driver.id) || !driver.enabled_declared) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field, location, driver.id,
                "driver entries require a stable id and enabled flag");
    } else if (!item_ids.insert(driver.id).second) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::duplicate_stable_id, location + ".id",
                driver.id, "catalog item stable id is duplicated");
    }
    validate_localizations(driver.localizations, location,
                           document.default_locale, issues);
    validate_reference_list(driver.hardware_kinds,
                            location + ".hardware_kinds", driver.id, issues);
    for (auto const& hardware_kind : driver.hardware_kinds) {
      if (valid_stable_id(hardware_kind) &&
          std::ranges::find(policy.supported_driver_hardware_kinds,
                            hardware_kind) ==
              policy.supported_driver_hardware_kinds.end()) {
        add_issue(issues, CatalogIssueScope::package,
                  CatalogIssueCode::unknown_execution_semantics,
                  location + ".hardware_kinds", driver.id,
                  "driver hardware applicability semantics are unavailable");
      }
    }
    for (std::size_t source_index = 0;
         source_index < driver.sources.size(); ++source_index) {
      validate_source(driver.sources[source_index],
                      location + ".sources[" +
                          std::to_string(source_index) + "]",
                      driver.id, driver.enabled, issues);
    }
    if (!driver.enabled) {
      continue;
    }
    if (driver.name.empty() || !driver.entry_type.has_value() ||
        !driver.hardware_kinds_declared || driver.hardware_kinds.empty() ||
        driver.branch.empty() || !driver.version_policy.has_value()) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field, location, driver.id,
                "enabled driver is missing required product fields");
    }
    if (driver.version_policy == VersionPolicy::fixed &&
        (!driver.fixed_version.has_value() || driver.fixed_version->empty())) {
      add_issue(issues, CatalogIssueScope::package,
                CatalogIssueCode::missing_required_field,
                location + ".fixed_version", driver.id,
                "fixed version policy requires fixed_version");
    }
    validate_enabled_sources(driver.sources, location, driver.id, issues);
  }

  if (has_package_error(issues)) {
    return result;
  }

  RuntimeSoftwareCatalog runtime{
      .schema_version = document.schema_version,
      .revision = document.revision,
      .release_state = *document.release_state,
      .default_locale = document.default_locale,
      .categories = document.categories,
  };
  for (auto const& software : document.software) {
    if (software.enabled) {
      runtime.software.push_back(RuntimeSoftware{.definition = software});
    }
  }
  for (auto const& driver : document.drivers) {
    if (driver.enabled) {
      runtime.drivers.push_back(RuntimeDriver{.definition = driver});
    }
  }
  for (auto& software : runtime.software) {
    if (!software.definition.install_profile.has_value()) {
      continue;
    }
    auto const* profile =
        find_profile(policy, *software.definition.install_profile);
    if (profile != nullptr &&
        profile_applies(*profile, software.definition.id)) {
      continue;
    }
    software.availability = ItemAvailability::install_profile_unavailable;
    std::string reason =
        profile == nullptr ||
                profile->runtime_status == InstallProfileRuntimeStatus::missing
            ? "install profile reference is missing"
            : "install profile is not applicable to this software and workbench";
    software.reasons.push_back(reason);
    add_issue(issues, CatalogIssueScope::item,
              CatalogIssueCode::install_profile_unavailable,
              "software." + software.definition.id + ".install_profile",
              software.definition.id, std::move(reason));
  }
  analyze_dependencies(runtime.software, "software", issues);

  result.outcome = RuntimeLoadOutcome::accepted;
  result.catalog = std::move(runtime);
  return result;
}

namespace {

SoftwareCatalogReleaseGate evaluate_release_gate_with_mode(
    SoftwareCatalogDocument const& document,
    RuntimeCatalogLoad const& runtime,
    SoftwareCatalogPolicy const& policy,
    std::string_view identity,
    SoftwareCatalogReleaseGateMode mode) {
  SoftwareCatalogReleaseGate gate;
  if (!runtime.accepted()) {
    gate.outcome = ReleaseGateOutcome::not_evaluated;
    gate.issues = runtime.issues;
    return gate;
  }

  auto& issues = gate.issues;
  if (document.release_state != ReleaseState::release) {
    add_issue(issues, CatalogIssueScope::release,
              CatalogIssueCode::draft_release_state, "release_state", {},
              "draft catalogs may run locally but cannot pass release gate");
  }
  if (std::ranges::any_of(runtime.issues, [](CatalogIssue const& issue) {
        return issue.scope == CatalogIssueScope::item &&
               dependency_issue(issue);
      })) {
    add_issue(issues, CatalogIssueScope::release,
              CatalogIssueCode::release_dependency_error, "dependencies", {},
              "local dependency errors block formal catalog release");
  }

  for (auto const& required_id : policy.required_release_software) {
    auto const definition = std::ranges::find(
        document.software, required_id, &SoftwareDefinition::id);
    if (definition == document.software.end()) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::required_item_missing,
                "software." + required_id, required_id,
                "required first-release software is missing");
      continue;
    }
    if (!definition->enabled) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::required_item_disabled,
                "software." + required_id, required_id,
                "required first-release software is not enabled");
    }
  }

  for (auto const& required_id : policy.required_release_software) {
    auto const requirement = std::ranges::find(
        policy.required_release_install_facts, required_id,
        &RequiredReleaseInstallFacts::software_id);
    if (requirement == policy.required_release_install_facts.end() ||
        !requirement->complete()) {
      if (mode == SoftwareCatalogReleaseGateMode::formal) {
        add_issue(issues, CatalogIssueScope::release,
                  CatalogIssueCode::unknown_execution_semantics,
                  "software." + required_id + ".install_facts", required_id,
                  "required first-release install facts remain unknown");
      }
    }
  }

  for (auto const& software : document.software) {
    if (!software.enabled || !software.install_profile.has_value()) {
      continue;
    }
    if (std::ranges::find(policy.required_release_software, software.id) ==
        policy.required_release_software.end()) {
      continue;
    }
    auto const* support = find_profile(policy, *software.install_profile);
    if (support == nullptr || !profile_applies(*support, software.id) ||
        (mode == SoftwareCatalogReleaseGateMode::formal &&
         !support->release_ready)) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::install_profile_not_release_ready,
                "software." + software.id + ".install_profile", software.id,
                "install profile is not ready for formal release");
    }
  }

  for (auto const& requirement : policy.required_release_drivers) {
    auto const definition = std::ranges::find(
        document.drivers, requirement.id, &DriverDefinition::id);
    if (definition == document.drivers.end()) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::required_item_missing,
                "drivers." + requirement.id, requirement.id,
                "required first-release driver is missing");
      continue;
    }
    if (!definition->enabled) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::required_item_disabled,
                "drivers." + requirement.id, requirement.id,
                "required first-release driver is not enabled");
      continue;
    }
    auto const* source = find_primary_source(definition->sources);
    if (definition->entry_type != requirement.entry_type ||
        std::ranges::find(definition->hardware_kinds,
                          requirement.hardware_kind) ==
            definition->hardware_kinds.end() ||
        source == nullptr ||
        source->address != requirement.primary_source_address) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::invalid_field,
                "drivers." + requirement.id, requirement.id,
                "required first-release driver does not match its registered entry policy");
    }
  }

  if (policy.last_published.has_value()) {
    if (document.revision < policy.last_published->revision) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::release_revision_regression, "revision", {},
                "release revision is older than the last published catalog");
    } else if (document.revision == policy.last_published->revision &&
               identity != policy.last_published->content_identity) {
      add_issue(issues, CatalogIssueScope::release,
                CatalogIssueCode::release_revision_conflict, "revision", {},
                "one release revision cannot identify different content");
    }
  }

  gate.outcome =
      issues.empty() ? ReleaseGateOutcome::passed : ReleaseGateOutcome::failed;
  return gate;
}

}  // namespace

SoftwareCatalogReleaseGate evaluate_release_gate(
    SoftwareCatalogDocument const& document,
    RuntimeCatalogLoad const& runtime,
    SoftwareCatalogPolicy const& policy,
    std::string_view identity) {
  return evaluate_release_gate_with_mode(
      document, runtime, policy, identity,
      SoftwareCatalogReleaseGateMode::formal);
}

SoftwareCatalogReleaseGate evaluate_beta_candidate_release_gate(
    SoftwareCatalogDocument const& document,
    RuntimeCatalogLoad const& runtime,
    SoftwareCatalogPolicy const& policy,
    std::string_view identity) {
  return evaluate_release_gate_with_mode(
      document, runtime, policy, identity,
      SoftwareCatalogReleaseGateMode::v0_1_0_beta_candidate);
}

std::string content_identity(std::string_view bytes) {
  static constexpr std::array<std::uint32_t, 64> round_constants{
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
      0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
      0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
      0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
      0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
      0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
      0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
  };
  std::array<std::uint32_t, 8> hash{
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
  };
  auto const padded_size = ((bytes.size() + 9U + 63U) / 64U) * 64U;
  auto const bit_size = static_cast<std::uint64_t>(bytes.size()) * 8U;
  for (std::size_t offset = 0; offset < padded_size; offset += 64U) {
    std::array<std::uint8_t, 64> block{};
    for (std::size_t index = 0; index < block.size(); ++index) {
      auto const position = offset + index;
      if (position < bytes.size()) {
        block[index] = static_cast<std::uint8_t>(bytes[position]);
      } else if (position == bytes.size()) {
        block[index] = 0x80U;
      } else if (position >= padded_size - 8U) {
        auto const shift = static_cast<unsigned>(
            (padded_size - 1U - position) * 8U);
        block[index] = static_cast<std::uint8_t>(bit_size >> shift);
      }
    }

    std::array<std::uint32_t, 64> schedule{};
    for (std::size_t index = 0; index < 16; ++index) {
      auto const start = index * 4U;
      schedule[index] =
          (static_cast<std::uint32_t>(block[start]) << 24U) |
          (static_cast<std::uint32_t>(block[start + 1U]) << 16U) |
          (static_cast<std::uint32_t>(block[start + 2U]) << 8U) |
          static_cast<std::uint32_t>(block[start + 3U]);
    }
    for (std::size_t index = 16; index < schedule.size(); ++index) {
      auto const sigma0 = std::rotr(schedule[index - 15U], 7) ^
                          std::rotr(schedule[index - 15U], 18) ^
                          (schedule[index - 15U] >> 3U);
      auto const sigma1 = std::rotr(schedule[index - 2U], 17) ^
                          std::rotr(schedule[index - 2U], 19) ^
                          (schedule[index - 2U] >> 10U);
      schedule[index] = schedule[index - 16U] + sigma0 +
                        schedule[index - 7U] + sigma1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < schedule.size(); ++index) {
      auto const sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^
                        std::rotr(e, 25);
      auto const choose = (e & f) ^ (~e & g);
      auto const temporary1 = h + sum1 + choose + round_constants[index] +
                              schedule[index];
      auto const sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^
                        std::rotr(a, 22);
      auto const majority = (a & b) ^ (a & c) ^ (b & c);
      auto const temporary2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (auto const part : hash) {
    stream << std::setw(8) << part;
  }
  return stream.str();
}

}  // namespace azzs::domain::software_catalog
