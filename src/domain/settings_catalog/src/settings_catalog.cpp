#include "azzs/settings_catalog/settings_catalog.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
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

[[nodiscard]] bool plan_has_cycle(OptimizationPlan const& plan) {
  std::map<std::string, std::vector<std::string>> dependencies;
  for (auto const& member : plan.members) {
    auto& values = dependencies[member.setting_id.value];
    for (auto const& dependency : member.depends_on) {
      values.push_back(dependency.value);
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
    for (auto const& dependency : member.depends_on) {
      if (!dependency.valid() || dependency == member.setting_id ||
          !member_ids.contains(dependency.value) ||
          !dependencies.insert(dependency.value).second) {
        local_problem(CatalogProblemCode::invalid_plan_dependency,
                      "optimization plan has an invalid dependency for: " +
                          member.setting_id.value);
      }
    }
  }

  if (plan_has_cycle(plan)) {
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

}  // namespace

bool StableId::valid() const noexcept {
  return safe_identifier(value, 128);
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
    auto const valid_minimum =
        !setting.known_windows_range.minimum.has_value() ||
        present(*setting.known_windows_range.minimum, 64);
    auto const valid_maximum =
        !setting.known_windows_range.maximum.has_value() ||
        present(*setting.known_windows_range.maximum, 64);
    if (!valid_minimum || !valid_maximum) {
      add_problem(result.problems, CatalogProblemCode::invalid_required_field,
                  setting.id,
                  "known Windows version range contains invalid text");
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
  std::vector<CatalogProblem> problems;
  auto const previous = settings_by_id(current.catalog);
  for (auto const& setting : candidate.catalog.settings) {
    auto const found = previous.find(setting.id.value);
    if (found != previous.end() &&
        found->second->semantics.identity != setting.semantics.identity) {
      add_problem(problems, CatalogProblemCode::stable_identity_reused,
                  setting.id,
                  "stable identifier cannot be reassigned to another Windows target");
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
