#include "azzs/settings_catalog/settings_catalog.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>

namespace azzs::domain::settings_catalog {
namespace {

[[nodiscard]] bool ascii_lower(char value) noexcept {
  return value >= 'a' && value <= 'z';
}

[[nodiscard]] bool ascii_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] bool safe_identifier(std::string_view value,
                                   std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum ||
      !(ascii_lower(value.front()) || ascii_digit(value.front()))) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    return ascii_lower(character) || ascii_digit(character) ||
           character == '-' || character == '_' || character == '.' ||
           character == ':';
  });
}

[[nodiscard]] bool present(std::string_view value,
                           std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
         std::ranges::all_of(value, [](unsigned char byte) {
           return byte >= 0x20 && byte != 0x7f;
         });
}

[[nodiscard]] bool supported(std::vector<std::string> const& values,
                             std::string_view candidate) {
  return std::ranges::find(values, candidate) != values.end();
}

void add_problem(std::vector<CatalogProblem>& problems,
                 CatalogProblemCode code, std::optional<StableId> id,
                 std::string detail) {
  problems.push_back(
      CatalogProblem{.code = code,
                     .item_id = std::move(id),
                     .detail = std::move(detail)});
}

[[nodiscard]] std::map<std::string, SettingDefinition const*> settings_by_id(
    SettingsCatalog const& catalog) {
  std::map<std::string, SettingDefinition const*> result;
  for (auto const& setting : catalog.settings) {
    result.emplace(setting.id.value, &setting);
  }
  return result;
}

[[nodiscard]] bool plan_has_cycle(
    OptimizationPlan const& plan,
    std::map<std::string, SettingDefinition const*> const& settings) {
  std::map<std::string, std::vector<std::string>> dependencies;
  for (auto const& member : plan.members) {
    auto& values = dependencies[member.setting_id.value];
    auto const found = settings.find(member.setting_id.value);
    if (found != settings.end()) {
      for (auto const& dependency : found->second->depends_on) {
        values.push_back(dependency.value);
      }
    }
  }

  enum class Visit { unseen, active, complete };
  std::map<std::string, Visit> visits;
  auto visit = [&](auto const& self, std::string const& id) -> bool {
    auto& state = visits[id];
    if (state == Visit::active) {
      return true;
    }
    if (state == Visit::complete) {
      return false;
    }
    state = Visit::active;
    auto const found = dependencies.find(id);
    if (found != dependencies.end()) {
      for (auto const& dependency : found->second) {
        if (self(self, dependency)) {
          return true;
        }
      }
    }
    state = Visit::complete;
    return false;
  };

  for (auto const& [id, ignored] : dependencies) {
    static_cast<void>(ignored);
    if (visit(visit, id)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] PlanAvailability validate_plan(
    OptimizationPlan const& plan,
    std::map<std::string, SettingDefinition const*> const& settings) {
  PlanAvailability result{.plan_id = plan.id};
  auto local_problem = [&](CatalogProblemCode code, std::string detail) {
    result.enabled = false;
    add_problem(result.problems, code, plan.id, std::move(detail));
  };

  if (plan.members.empty()) {
    local_problem(CatalogProblemCode::empty_plan,
                  "optimization plan has no setting members");
    return result;
  }

  std::set<std::string> member_ids;
  std::set<std::uint32_t> orders;
  for (auto const& member : plan.members) {
    if (!member.setting_id.valid() ||
        !settings.contains(member.setting_id.value)) {
      local_problem(CatalogProblemCode::missing_plan_member,
                    "optimization plan references an unavailable setting: " +
                        member.setting_id.value);
      continue;
    }
    if (!member_ids.insert(member.setting_id.value).second) {
      local_problem(CatalogProblemCode::duplicate_plan_member,
                    "optimization plan repeats setting: " +
                        member.setting_id.value);
    }
    if (!orders.insert(member.order).second) {
      local_problem(CatalogProblemCode::duplicate_plan_order,
                    "optimization plan repeats execution order: " +
                        std::to_string(member.order));
    }
  }

  for (auto const& member : plan.members) {
    std::set<std::string> dependencies;
    auto const found = settings.find(member.setting_id.value);
    if (found == settings.end()) {
      continue;
    }
    for (auto const& dependency : found->second->depends_on) {
      if (!dependency.valid() || dependency == member.setting_id ||
          !settings.contains(dependency.value) ||
          !member_ids.contains(dependency.value) ||
          !dependencies.insert(dependency.value).second) {
        local_problem(CatalogProblemCode::invalid_plan_dependency,
                      "optimization plan has an invalid dependency for: " +
                          member.setting_id.value);
      }
    }
  }

  if (plan_has_cycle(plan, settings)) {
    local_problem(CatalogProblemCode::cyclic_plan_dependencies,
                  "optimization plan member dependencies form a cycle");
  }
  return result;
}

template <typename Item>
void append_changes(std::vector<Item> const& current,
                    std::vector<Item> const& candidate,
                    CatalogItemKind item_kind,
                    CatalogChangePreview& preview) {
  std::map<std::string, Item const*> old_items;
  std::map<std::string, Item const*> new_items;
  for (auto const& item : current) {
    old_items.emplace(item.id.value, &item);
  }
  for (auto const& item : candidate) {
    new_items.emplace(item.id.value, &item);
  }
  for (auto const& [id, item] : new_items) {
    auto const previous = old_items.find(id);
    if (previous == old_items.end()) {
      preview.added.push_back({.change = CatalogChangeKind::added,
                               .kind = item_kind,
                               .id = item->id,
                               .display_name = item->display_name});
    } else if (*previous->second != *item) {
      preview.changed.push_back({.change = CatalogChangeKind::changed,
                                 .kind = item_kind,
                                 .id = item->id,
                                 .display_name = item->display_name});
    }
  }
  for (auto const& [id, item] : old_items) {
    if (!new_items.contains(id)) {
      preview.retired.push_back({.change = CatalogChangeKind::retired,
                                 .kind = item_kind,
                                 .id = item->id,
                                 .display_name = item->display_name});
    }
  }
}

void sort_changes(std::vector<CatalogItemChange>& changes) {
  std::ranges::sort(changes, {}, [](CatalogItemChange const& change) {
    return std::pair{change.kind, change.id.value};
  });
}

void append_fingerprint_field(std::string& fingerprint,
                              std::string_view value) {
  fingerprint += std::to_string(value.size());
  fingerprint += ':';
  fingerprint.append(value);
}

template <typename Value>
void append_fingerprint_number(std::string& fingerprint, Value value) {
  append_fingerprint_field(
      fingerprint,
      std::to_string(static_cast<std::uint64_t>(value)));
}

[[nodiscard]] std::string setting_fingerprint(
    SettingDefinition const& setting) {
  std::string fingerprint{"settings-setting-v2|"};
  append_fingerprint_field(fingerprint, setting.semantics.identity);
  append_fingerprint_field(fingerprint, setting.semantics.apply_capability);
  append_fingerprint_field(fingerprint, setting.semantics.detect_capability);
  append_fingerprint_field(
      fingerprint, setting.semantics.recover_capability.value_or(""));
  append_fingerprint_number(fingerprint, setting.recovery_requirement);
  append_fingerprint_number(fingerprint, setting.restart_requirement);
  append_fingerprint_number(fingerprint, setting.risk);
  append_fingerprint_number(fingerprint, setting.force_attempt_rule);
  auto dependencies = setting.depends_on;
  std::ranges::sort(dependencies, {}, &StableId::value);
  append_fingerprint_number(fingerprint, dependencies.size());
  for (auto const& dependency : dependencies) {
    append_fingerprint_field(fingerprint, dependency.value);
  }
  auto append_version = [&](std::optional<WindowsVersion> const& version) {
    append_fingerprint_number(fingerprint, version.has_value() ? 1 : 0);
    if (version.has_value()) {
      append_fingerprint_number(fingerprint, version->generation);
      append_fingerprint_number(fingerprint, version->feature_update_year);
      append_fingerprint_number(fingerprint, version->feature_update_half);
    }
  };
  append_version(setting.known_windows_range.minimum);
  append_version(setting.known_windows_range.maximum);
  return fingerprint;
}

[[nodiscard]] std::string plan_fingerprint(OptimizationPlan const& plan) {
  auto members = plan.members;
  std::ranges::sort(members, [](PlanMember const& left,
                               PlanMember const& right) {
    return std::pair{left.order, left.setting_id.value} <
           std::pair{right.order, right.setting_id.value};
  });

  std::string fingerprint{"settings-plan-v2|"};
  append_fingerprint_number(fingerprint, members.size());
  for (auto const& member : members) {
    append_fingerprint_field(fingerprint, member.setting_id.value);
    append_fingerprint_number(fingerprint, member.order);
    append_fingerprint_number(fingerprint, member.default_selected ? 1 : 0);
  }
  return fingerprint;
}

[[nodiscard]] bool same_identity(CatalogIdentityTombstone const& left,
                                 CatalogIdentityTombstone const& right) {
  return left.kind == right.kind && left.id == right.id;
}

}  // namespace

bool StableId::valid() const noexcept {
  return safe_identifier(value, 128);
}

bool WindowsVersion::valid() const noexcept {
  auto const generation_value = static_cast<std::uint8_t>(generation);
  return (generation_value == 10 || generation_value == 11) &&
         feature_update_year >= 15 && feature_update_year <= 99 &&
         (feature_update_half == 1 || feature_update_half == 2);
}

bool WindowsVersionRange::valid() const noexcept {
  if ((minimum.has_value() && !minimum->valid()) ||
      (maximum.has_value() && !maximum->valid())) {
    return false;
  }
  return !minimum.has_value() || !maximum.has_value() ||
         *minimum <= *maximum;
}

bool WindowsVersionRange::contains(WindowsVersion const& version) const
    noexcept {
  return valid() && version.valid() &&
         (!minimum.has_value() || version >= *minimum) &&
         (!maximum.has_value() || version <= *maximum);
}

CatalogValidationResult validate(
    SettingsCatalog catalog, SupportedCapabilities const& capabilities) {
  CatalogValidationResult result;
  if (catalog.schema_version != supported_schema_version) {
    add_problem(result.problems, CatalogProblemCode::unsupported_schema,
                std::nullopt,
                "settings catalog schema is not supported by this workbench");
  }
  if (catalog.revision == 0) {
    add_problem(result.problems, CatalogProblemCode::invalid_revision,
                std::nullopt, "settings catalog revision must be positive");
  }
  for (auto const& field : catalog.unknown_semantic_fields) {
    add_problem(result.problems,
                CatalogProblemCode::unknown_execution_semantics,
                std::nullopt,
                "unknown settings catalog operation semantic: " + field);
  }

  std::set<std::string> setting_ids;
  std::set<std::string> target_identities;
  for (auto const& setting : catalog.settings) {
    if (!setting.id.valid()) {
      add_problem(result.problems, CatalogProblemCode::invalid_stable_id,
                  setting.id, "system setting stable identifier is invalid");
    } else if (!setting_ids.insert(setting.id.value).second) {
      add_problem(result.problems, CatalogProblemCode::duplicate_stable_id,
                  setting.id,
                  "system setting stable identifier is duplicated");
    }
    if (!present(setting.display_name, 512) ||
        !present(setting.description, 4096) ||
        !safe_identifier(setting.semantics.identity, 256)) {
      add_problem(result.problems, CatalogProblemCode::invalid_required_field,
                  setting.id,
                  "system setting name, description, or target identity is invalid");
    } else if (!target_identities.insert(setting.semantics.identity).second) {
      add_problem(result.problems,
                  CatalogProblemCode::duplicate_target_identity, setting.id,
                  "multiple stable identifiers name the same system setting target");
    }
    if (setting.source_url.has_value() &&
        (!present(*setting.source_url, 2048) ||
         !(setting.source_url->starts_with("https://") ||
           setting.source_url->starts_with("http://")))) {
      add_problem(result.problems, CatalogProblemCode::invalid_source_url,
                  setting.id,
                  "system setting source must be an HTTP or HTTPS URL");
    }
    if (!setting.known_windows_range.valid()) {
      add_problem(result.problems, CatalogProblemCode::invalid_required_field,
                  setting.id,
                  "known Windows version range is invalid or reversed");
    }
    std::set<std::string> dependencies;
    for (auto const& dependency : setting.depends_on) {
      if (!dependency.valid() || dependency == setting.id ||
          !dependencies.insert(dependency.value).second) {
        add_problem(result.problems,
                    CatalogProblemCode::invalid_required_field, setting.id,
                    "system setting dependency is invalid or duplicated");
      }
    }
    auto const valid_apply =
        safe_identifier(setting.semantics.apply_capability, 128) &&
        supported(capabilities.apply, setting.semantics.apply_capability);
    auto const valid_detect =
        safe_identifier(setting.semantics.detect_capability, 128) &&
        supported(capabilities.detect, setting.semantics.detect_capability);
    bool valid_recover = true;
    if (setting.recovery_requirement ==
        RecoveryRequirement::restore_record_required) {
      valid_recover = setting.semantics.recover_capability.has_value() &&
                      safe_identifier(
                          *setting.semantics.recover_capability, 128) &&
                      supported(capabilities.recover,
                                *setting.semantics.recover_capability);
    } else if (setting.semantics.recover_capability.has_value()) {
      valid_recover = false;
    }
    if (!valid_apply || !valid_detect || !valid_recover) {
      add_problem(result.problems,
                  CatalogProblemCode::unknown_execution_semantics,
                  setting.id,
                  "system setting references an unknown controlled capability");
    }
    if (setting.force_attempt_rule ==
            ForceAttemptRule::allowed_with_explicit_confirmation &&
        setting.recovery_requirement !=
            RecoveryRequirement::restore_record_required) {
      add_problem(result.problems, CatalogProblemCode::invalid_required_field,
                  setting.id,
                  "force attempt requires explicit restore-record semantics");
    }
  }

  for (auto const& setting : catalog.settings) {
    for (auto const& dependency : setting.depends_on) {
      if (!setting_ids.contains(dependency.value)) {
        add_problem(result.problems,
                    CatalogProblemCode::invalid_required_field, setting.id,
                    "system setting dependency references an unavailable setting");
      }
    }
  }

  std::set<std::string> plan_ids;
  for (auto const& plan : catalog.plans) {
    if (!plan.id.valid()) {
      add_problem(result.problems, CatalogProblemCode::invalid_stable_id,
                  plan.id,
                  "system optimization plan stable identifier is invalid");
    } else if (!plan_ids.insert(plan.id.value).second) {
      add_problem(result.problems, CatalogProblemCode::duplicate_stable_id,
                  plan.id,
                  "system optimization plan stable identifier is duplicated");
    }
    if (!present(plan.display_name, 512) ||
        !present(plan.description, 4096)) {
      add_problem(result.problems, CatalogProblemCode::invalid_required_field,
                  plan.id,
                  "system optimization plan name or description is invalid");
    }
  }

  if (!result.problems.empty()) {
    return result;
  }

  ValidatedSettingsCatalog validated{.catalog = std::move(catalog)};
  auto const settings = settings_by_id(validated.catalog);
  validated.plans.reserve(validated.catalog.plans.size());
  for (auto const& plan : validated.catalog.plans) {
    validated.plans.push_back(validate_plan(plan, settings));
  }
  result.validated = std::move(validated);
  return result;
}

std::vector<CatalogProblem> validate_transition(
    ValidatedSettingsCatalog const& current,
    ValidatedSettingsCatalog const& candidate) {
  return validate_transition(identity_tombstones(current.catalog), candidate);
}

std::vector<CatalogIdentityTombstone> identity_tombstones(
    SettingsCatalog const& catalog) {
  std::vector<CatalogIdentityTombstone> result;
  result.reserve(catalog.settings.size() + catalog.plans.size());
  for (auto const& setting : catalog.settings) {
    result.push_back({.kind = CatalogItemKind::setting,
                      .id = setting.id,
                      .semantic_fingerprint = setting_fingerprint(setting)});
  }
  for (auto const& plan : catalog.plans) {
    result.push_back(
        {.kind = CatalogItemKind::optimization_plan,
         .id = plan.id,
         .semantic_fingerprint = plan_fingerprint(plan)});
  }
  std::ranges::sort(result, [](CatalogIdentityTombstone const& left,
                              CatalogIdentityTombstone const& right) {
    return std::pair{left.kind, left.id.value} <
           std::pair{right.kind, right.id.value};
  });
  return result;
}

std::vector<CatalogIdentityTombstone> merge_identity_tombstones(
    std::vector<CatalogIdentityTombstone> history,
    SettingsCatalog const& catalog) {
  for (auto& candidate : identity_tombstones(catalog)) {
    auto const found = std::ranges::find_if(
        history, [&](CatalogIdentityTombstone const& existing) {
          return same_identity(existing, candidate);
        });
    if (found == history.end()) {
      history.push_back(std::move(candidate));
    }
  }
  std::ranges::sort(history, [](CatalogIdentityTombstone const& left,
                               CatalogIdentityTombstone const& right) {
    return std::pair{left.kind, left.id.value} <
           std::pair{right.kind, right.id.value};
  });
  return history;
}

std::vector<CatalogProblem> validate_transition(
    std::vector<CatalogIdentityTombstone> const& history,
    ValidatedSettingsCatalog const& candidate) {
  std::vector<CatalogProblem> problems;
  std::map<std::pair<CatalogItemKind, std::string>, std::string>
      fingerprints;
  for (auto const& previous : history) {
    auto const key = std::pair{previous.kind, previous.id.value};
    auto const [found, inserted] =
        fingerprints.emplace(key, previous.semantic_fingerprint);
    if (!previous.id.valid() || previous.semantic_fingerprint.empty() ||
        (!inserted && found->second != previous.semantic_fingerprint)) {
      add_problem(problems, CatalogProblemCode::stable_identity_reused,
                  previous.id,
                  "persisted settings catalog identity history is inconsistent");
    }
  }
  for (auto const& identity : identity_tombstones(candidate.catalog)) {
    auto const found =
        fingerprints.find(std::pair{identity.kind, identity.id.value});
    if (found != fingerprints.end() &&
        found->second != identity.semantic_fingerprint) {
      add_problem(
          problems, CatalogProblemCode::stable_identity_reused, identity.id,
          identity.kind == CatalogItemKind::setting
              ? "stable setting identifier cannot change or reuse operation semantics"
              : "stable optimization plan identifier cannot change or reuse membership semantics");
    }
  }
  return problems;
}

SettingDefinition const* find_setting(
    ValidatedSettingsCatalog const& catalog, StableId const& id) noexcept {
  auto const found = std::ranges::find(catalog.catalog.settings, id,
                                       &SettingDefinition::id);
  return found == catalog.catalog.settings.end() ? nullptr : &*found;
}

PlanAvailability const* find_plan_availability(
    ValidatedSettingsCatalog const& catalog, StableId const& id) noexcept {
  auto const found =
      std::ranges::find(catalog.plans, id, &PlanAvailability::plan_id);
  return found == catalog.plans.end() ? nullptr : &*found;
}

CatalogChangePreview preview_changes(
    ValidatedSettingsCatalog const& current,
    ValidatedSettingsCatalog const& candidate) {
  CatalogChangePreview preview{
      .current_revision = current.catalog.revision,
      .candidate_revision = candidate.catalog.revision,
      .downgrade = candidate.catalog.revision < current.catalog.revision,
  };
  append_changes(current.catalog.settings, candidate.catalog.settings,
                 CatalogItemKind::setting, preview);
  append_changes(current.catalog.plans, candidate.catalog.plans,
                 CatalogItemKind::optimization_plan, preview);
  sort_changes(preview.added);
  sort_changes(preview.changed);
  sort_changes(preview.retired);
  return preview;
}

}  // namespace azzs::domain::settings_catalog
