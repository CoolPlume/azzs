#include "azzs/domain/software_selection.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::domain::software_selection {
namespace {

[[nodiscard]] catalog::RuntimeSoftware const* find_software(
    catalog::RuntimeSoftwareCatalog const& runtime, std::string_view id) {
  auto const found = std::ranges::find(
      runtime.software, id, [](catalog::RuntimeSoftware const& software) {
        return software.definition.id;
      });
  return found == runtime.software.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] bool contains(std::vector<std::string> const& values,
                            std::string_view value) {
  return std::ranges::find(values, value) != values.end();
}

void sort_unique(std::vector<std::string>& values) {
  std::ranges::sort(values);
  auto const duplicate = std::ranges::unique(values);
  values.erase(duplicate.begin(), duplicate.end());
}

[[nodiscard]] bool dependencies_available(
    catalog::RuntimeSoftwareCatalog const& runtime,
    catalog::RuntimeSoftware const& software) {
  return std::ranges::all_of(
      software.definition.dependencies, [&](std::string const& dependency) {
        auto const* item = find_software(runtime, dependency);
        return item != nullptr &&
               item->availability == catalog::ItemAvailability::available;
      });
}

void append_with_dependencies(catalog::RuntimeSoftwareCatalog const& runtime,
                              std::string_view id,
                              std::vector<std::string>& selected,
                              std::vector<std::string>& closure) {
  auto const* software = find_software(runtime, id);
  if (software == nullptr) {
    return;
  }
  if (!contains(closure, id)) {
    closure.emplace_back(id);
  }
  if (!contains(selected, id)) {
    selected.emplace_back(id);
  }
  for (auto const& dependency : software->definition.dependencies) {
    if (!contains(closure, dependency)) {
      append_with_dependencies(runtime, dependency, selected, closure);
    }
  }
}

[[nodiscard]] bool depends_on(catalog::RuntimeSoftwareCatalog const& runtime,
                              std::string_view candidate,
                              std::string_view dependency,
                              std::set<std::string, std::less<>>& visited) {
  if (!visited.insert(std::string{candidate}).second) {
    return false;
  }
  auto const* software = find_software(runtime, candidate);
  if (software == nullptr) {
    return false;
  }
  for (auto const& direct : software->definition.dependencies) {
    if (direct == dependency ||
        depends_on(runtime, direct, dependency, visited)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string lower(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (auto character : value) {
    result.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(character))));
  }
  return result;
}

}  // namespace

bool ResolvedSourceSnapshot::stable_version() const noexcept {
  auto const normalized = lower(version);
  return !version.empty() && normalized.find("beta") == std::string::npos &&
         normalized.find("preview") == std::string::npos &&
         normalized.find("nightly") == std::string::npos &&
         normalized.find("alpha") == std::string::npos &&
         normalized.find("rc") == std::string::npos;
}

bool ResolvedSourceSnapshot::valid() const noexcept {
  if (software_id.empty() || declared_address.empty() || version.empty() ||
      actual_address.empty() || hosting_mechanism.empty() || branch.empty() ||
      capability_version.empty() || resolved_at_milliseconds <= 0 ||
      !stable_version() || packages.empty()) {
    return false;
  }
  return std::ranges::all_of(packages, [&](ResolvedPackage const& package) {
    return package.candidate.software_id == software_id &&
           package.candidate.version == version &&
           !package.candidate.identity.empty() &&
           package.candidate.architecture !=
               architecture::PackageArchitecture::unknown &&
           (package.package_type != PackageType::online_installer ||
            package.network_required || network_required) &&
           (package.package_type != PackageType::full_package ||
            package.complete_package);
  });
}

SelectionState default_selection(catalog::RuntimeSoftwareCatalog const& runtime) {
  SelectionState state{.initialized = true};
  std::vector<std::string> closure;
  for (auto const& software : runtime.software) {
    if (software.definition.enabled &&
        software.definition.tier == catalog::SoftwareTier::basic &&
        software.availability == catalog::ItemAvailability::available &&
        dependencies_available(runtime, software)) {
      append_with_dependencies(runtime, software.definition.id,
                               state.selected_software_ids, closure);
    }
  }
  sort_unique(state.selected_software_ids);
  return state;
}

SelectionChange change_selection(catalog::RuntimeSoftwareCatalog const& runtime,
                                 SelectionState state,
                                 std::string_view software_id,
                                 bool selected) {
  auto const* target = find_software(runtime, software_id);
  if (target == nullptr) {
    return {.state = std::move(state), .reason = "software is not in the current catalog"};
  }
  if (target->availability != catalog::ItemAvailability::available ||
      !dependencies_available(runtime, *target)) {
    return {.state = std::move(state),
            .reason = "software or one of its dependencies is unavailable"};
  }

  sort_unique(state.selected_software_ids);
  if (selected) {
    std::vector<std::string> closure;
    append_with_dependencies(runtime, software_id, state.selected_software_ids,
                             closure);
    sort_unique(state.selected_software_ids);
    sort_unique(closure);
    auto items = project_selection(runtime, state);
    return {.applied = true,
            .state = std::move(state),
            .dependency_closure = std::move(closure),
            .items = std::move(items)};
  }

  if (!contains(state.selected_software_ids, software_id)) {
    auto items = project_selection(runtime, state);
    return {.applied = true,
            .state = std::move(state),
            .items = std::move(items)};
  }

  for (auto const& selected_id : state.selected_software_ids) {
    if (selected_id == software_id) {
      continue;
    }
    std::set<std::string, std::less<>> visited;
    if (depends_on(runtime, selected_id, software_id, visited)) {
      return {.state = std::move(state),
              .reason = "selected software requires this dependency"};
    }
  }
  state.selected_software_ids.erase(
      std::ranges::find(state.selected_software_ids, software_id));
  auto items = project_selection(runtime, state);
  return {.applied = true,
          .state = std::move(state),
          .items = std::move(items)};
}

std::vector<SelectionItem> project_selection(
    catalog::RuntimeSoftwareCatalog const& runtime,
    SelectionState const& state, std::vector<std::string> const& removed_ids,
    std::vector<std::string> const& changed_ids,
    std::vector<std::string> const& disabled_ids) {
  std::vector<SelectionItem> result;
  result.reserve(runtime.software.size() + state.selected_software_ids.size());
  for (auto const& software : runtime.software) {
    auto const selected = contains(state.selected_software_ids,
                                   software.definition.id);
    auto blocker = SelectionBlocker::none;
    auto reason = std::string{};
    if (software.availability != catalog::ItemAvailability::available ||
        contains(disabled_ids, software.definition.id)) {
      blocker = SelectionBlocker::unavailable_in_current_catalog;
      reason = software.reasons.empty() ? "unavailable in current catalog"
                                        : software.reasons.front();
    } else if (contains(changed_ids, software.definition.id)) {
      blocker = SelectionBlocker::catalog_execution_changed;
      reason = "catalog execution semantics changed; reselect this software";
    }
    result.push_back({
        .software_id = software.definition.id,
        .selected = selected,
        .basic = software.definition.tier == catalog::SoftwareTier::basic,
        .available = blocker == SelectionBlocker::none,
        .requires_reselection = selected &&
                                blocker == SelectionBlocker::catalog_execution_changed,
        .blocker = blocker,
        .reason = std::move(reason),
    });
  }
  for (auto const& selected : state.selected_software_ids) {
    if (find_software(runtime, selected) != nullptr) {
      continue;
    }
    result.push_back({
        .software_id = selected,
        .selected = true,
        .available = false,
        .requires_reselection = true,
        .blocker = contains(removed_ids, selected)
                       ? SelectionBlocker::catalog_item_removed
                       : SelectionBlocker::unknown_software,
        .reason = "selection was retained, but the software is no longer usable",
    });
  }
  std::ranges::sort(result, {}, &SelectionItem::software_id);
  return result;
}

bool is_declared_source(catalog::RuntimeSoftwareCatalog const& runtime,
                        std::string_view software_id,
                        catalog::CatalogSource const& source) noexcept {
  auto const* software = find_software(runtime, software_id);
  if (software == nullptr) {
    return false;
  }
  return std::ranges::find(software->definition.sources, source) !=
         software->definition.sources.end();
}

char const* to_string(SelectionBlocker blocker) noexcept {
  switch (blocker) {
    case SelectionBlocker::none:
      return "none";
    case SelectionBlocker::unknown_software:
      return "unknown-software";
    case SelectionBlocker::unavailable_in_current_catalog:
      return "unavailable-in-current-catalog";
    case SelectionBlocker::required_by_selected_software:
      return "required-by-selected-software";
    case SelectionBlocker::catalog_item_removed:
      return "catalog-item-removed";
    case SelectionBlocker::catalog_execution_changed:
      return "catalog-execution-changed";
  }
  return "unknown";
}

char const* to_string(PackageType type) noexcept {
  switch (type) {
    case PackageType::full_package:
      return "full-package";
    case PackageType::online_installer:
      return "online-installer";
    case PackageType::external_handoff:
      return "external-handoff";
  }
  return "unknown";
}

char const* to_string(ExternalHandoffStatus status) noexcept {
  switch (status) {
    case ExternalHandoffStatus::none:
      return "none";
    case ExternalHandoffStatus::waiting_for_external_install:
      return "waiting-for-external-install";
    case ExternalHandoffStatus::externally_recognized:
      return "externally-recognized";
    case ExternalHandoffStatus::skipped:
      return "skipped";
  }
  return "unknown";
}

}  // namespace azzs::domain::software_selection
