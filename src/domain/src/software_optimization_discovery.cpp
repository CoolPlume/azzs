#include "azzs/domain/software_optimization_discovery.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace azzs::domain::software_optimization_discovery {
namespace {

[[nodiscard]] bool installed(TargetPresence value) noexcept {
  return value == TargetPresence::detected ||
         value == TargetPresence::externally_recognized;
}

[[nodiscard]] std::optional<std::vector<std::uint64_t>> parse_version(
    std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }

  std::vector<std::uint64_t> parts;
  while (!value.empty()) {
    auto const separator = value.find('.');
    auto const part = value.substr(0, separator);
    if (part.empty() || parts.size() == 4) {
      return std::nullopt;
    }
    std::uint64_t parsed{};
    auto const [end, error] =
        std::from_chars(part.data(), part.data() + part.size(), parsed);
    if (error != std::errc{} || end != part.data() + part.size()) {
      return std::nullopt;
    }
    parts.push_back(parsed);
    if (separator == std::string_view::npos) {
      break;
    }
    value.remove_prefix(separator + 1);
  }
  return parts;
}

[[nodiscard]] int compare_versions(std::string_view left,
                                   std::string_view right) noexcept {
  auto const lhs = parse_version(left);
  auto const rhs = parse_version(right);
  if (!lhs.has_value() || !rhs.has_value()) {
    return 1;
  }
  auto const count = std::max(lhs->size(), rhs->size());
  for (std::size_t index = 0; index < count; ++index) {
    auto const lhs_part = index < lhs->size() ? (*lhs)[index] : 0;
    auto const rhs_part = index < rhs->size() ? (*rhs)[index] : 0;
    if (lhs_part < rhs_part) {
      return -1;
    }
    if (lhs_part > rhs_part) {
      return 1;
    }
  }
  return 0;
}

[[nodiscard]] bool version_matches(catalog::VersionRange const& range,
                                   std::string_view version) noexcept {
  return parse_version(version).has_value() && !range.minimum.empty() &&
         !range.maximum.empty() &&
         compare_versions(version, range.minimum) >= 0 &&
         compare_versions(version, range.maximum) <= 0;
}

[[nodiscard]] TargetObservation const* find_target_observation(
    std::span<TargetObservation const> observations,
    std::string_view target_id) {
  auto const found = std::ranges::find_if(observations,
      [&](TargetObservation const& observation) {
        return observation.target_id.value == target_id;
      });
  return found == observations.end() ? nullptr : &*found;
}

[[nodiscard]] OptionObservation const* find_option_observation(
    std::span<OptionObservation const> observations,
    std::string_view option_id) {
  auto const found = std::ranges::find_if(
      observations, [&](OptionObservation const& observation) {
        return observation.option_id.value == option_id;
      });
  return found == observations.end() ? nullptr : &*found;
}

[[nodiscard]] WithdrawnOperation const* find_withdrawal(
    std::span<WithdrawnOperation const> operations, std::string_view id) {
  auto const found = std::ranges::find_if(
      operations, [&](WithdrawnOperation const& operation) {
        return operation.operation_id.value == id;
      });
  return found == operations.end() ? nullptr : &*found;
}

[[nodiscard]] bool selected(SelectionState const& state,
                            std::string_view scheme_id,
                            std::string_view option_id) {
  return std::ranges::any_of(state.options, [&](auto const& entry) {
    return entry.scheme_id.value == scheme_id &&
           entry.option_id.value == option_id;
  });
}

[[nodiscard]] catalog::SoftwareOptimizationOption const* find_option(
    catalog::SoftwareOptimizationCatalog const& catalog_value,
    std::string_view scheme_id, std::string_view option_id) {
  auto const* scheme = catalog_value.find_scheme(scheme_id);
  if (scheme == nullptr) {
    return nullptr;
  }
  auto const found = std::ranges::find_if(
      scheme->options, [&](catalog::SoftwareOptimizationOption const& option) {
        return option.id.value == option_id;
      });
  return found == scheme->options.end() ? nullptr : &*found;
}

void add_selection(SelectionState& state, SelectedOption entry) {
  if (!selected(state, entry.scheme_id.value, entry.option_id.value)) {
    state.options.push_back(std::move(entry));
  }
}

void remove_selection(SelectionState& state, std::string_view scheme_id,
                      std::string_view option_id) {
  std::erase_if(state.options, [&](SelectedOption const& entry) {
    return entry.scheme_id.value == scheme_id &&
           entry.option_id.value == option_id;
  });
}

[[nodiscard]] OptionState option_state_for(
    std::span<OptionObservation const> observations,
    catalog::SoftwareOptimizationOption const& option, std::string& detail) {
  auto const* observed = find_option_observation(observations, option.id.value);
  if (observed == nullptr) {
    detail = "option state has not been reliably detected";
    return OptionState::unknown;
  }
  detail = observed->detail;
  return observed->state;
}

}  // namespace

DiscoverySnapshot discover(DiscoveryInput const& input) {
  DiscoverySnapshot snapshot{.catalog_revision = input.catalog.revision};
  for (auto const& target : input.catalog.targets) {
    auto const* observed = find_target_observation(input.targets, target.id.value);
    auto const presence = observed == nullptr ? TargetPresence::unknown
                                              : observed->presence;
    if (!installed(presence)) {
      continue;
    }

    TargetDiscovery target_view{
        .target = target,
        .presence = presence,
        .installed_version = observed == nullptr ? std::nullopt
                                                 : observed->installed_version,
        .detail = observed == nullptr ? "target has not been reliably detected"
                                      : observed->detail,
    };
    bool has_usable_scheme = false;
    bool has_supported_first_release_scheme = false;

    for (auto const& scheme : input.catalog.schemes) {
      if (scheme.target_id != target.id) {
        continue;
      }
      SchemeDiscovery scheme_view{
          .scheme = scheme,
          .target_presence = presence,
          .installed_version = target_view.installed_version,
      };
      auto const supported_version =
          target_view.installed_version.has_value() &&
          version_matches(target.supported_versions,
                          *target_view.installed_version) &&
          version_matches(scheme.supported_versions,
                          *target_view.installed_version);
      has_supported_first_release_scheme =
          has_supported_first_release_scheme ||
          (scheme.required_first_release &&
           scheme.automation == catalog::AutomationSupport::controlled &&
           scheme.availability == catalog::SchemeAvailability::available &&
           supported_version);

      if (scheme.availability == catalog::SchemeAvailability::configuration_error) {
        scheme_view.state = SchemeState::configuration_error;
        scheme_view.detail = "directory configuration error";
      } else if (auto const* scheme_withdrawal =
                     find_withdrawal(input.withdrawn_operations, scheme.id.value);
                 scheme_withdrawal != nullptr) {
        scheme_view.state = SchemeState::emergency_withdrawn;
        scheme_view.detail = scheme_withdrawal->reason;
      } else if (!supported_version) {
        scheme_view.state = SchemeState::version_not_applicable;
        scheme_view.detail = "installed version is outside the declared support range";
      } else if (scheme.automation == catalog::AutomationSupport::manual_only ||
                 scheme.availability == catalog::SchemeAvailability::manual_only) {
        scheme_view.state = SchemeState::manual_only;
        scheme_view.detail = "automatic handling is not supported";
      } else {
        bool any_unknown = false;
        bool any_applicable_option = false;
        bool all_applicable_options_optimized = true;
        bool withdrawn_option = false;
        for (auto const& option : scheme.options) {
          OptionDiscovery option_view{.option = option,
                                      .selected = selected(input.selection,
                                                           scheme.id.value,
                                                           option.id.value)};
          if (auto const* option_withdrawal =
                  find_withdrawal(input.withdrawn_operations, option.id.value);
              option_withdrawal != nullptr) {
            withdrawn_option = true;
            option_view.state = OptionState::unknown;
            option_view.detail = option_withdrawal->reason;
          } else if (!version_matches(option.supported_versions,
                                      *target_view.installed_version)) {
            option_view.state = OptionState::version_not_applicable;
            option_view.detail =
                "installed version is outside this option's declared support range";
          } else {
            any_applicable_option = true;
            option_view.state = option_state_for(input.options, option,
                                                  option_view.detail);
            any_unknown = any_unknown || option_view.state == OptionState::unknown;
            all_applicable_options_optimized =
                all_applicable_options_optimized &&
                option_view.state == OptionState::optimized;
          }
          scheme_view.options.push_back(std::move(option_view));
        }
        if (withdrawn_option) {
          scheme_view.state = SchemeState::emergency_withdrawn;
          scheme_view.detail = "one or more options were emergency withdrawn";
        } else if (any_unknown) {
          scheme_view.state = SchemeState::needs_attention;
          scheme_view.detail = "one or more option states need confirmation";
        } else if (!any_applicable_option) {
          scheme_view.state = SchemeState::version_not_applicable;
          scheme_view.detail =
              "no option applies to the installed version within this scheme";
        } else if (all_applicable_options_optimized) {
          scheme_view.state = SchemeState::optimized;
          scheme_view.detail = "all options already match their targets";
        } else {
          scheme_view.state = SchemeState::can_optimize;
          scheme_view.detail = "selected options can be submitted for confirmation";
          has_usable_scheme = true;
        }
      }
      target_view.schemes.push_back(std::move(scheme_view));
    }

    target_view.no_available_optimization = !has_usable_scheme;
    target_view.first_release_implementation_error =
        target.required_first_release && target.support_mode == catalog::SupportMode::supported &&
        target_view.installed_version.has_value() &&
        version_matches(target.supported_versions, *target_view.installed_version) &&
        !has_supported_first_release_scheme;
    snapshot.targets.push_back(std::move(target_view));
  }
  return snapshot;
}

SelectionState default_selection(catalog::SoftwareOptimizationCatalog const& catalog_value) {
  SelectionState state;
  for (auto const& scheme : catalog_value.schemes) {
    if (scheme.availability != catalog::SchemeAvailability::available ||
        scheme.automation != catalog::AutomationSupport::controlled) {
      continue;
    }
    for (auto const& option : scheme.options) {
      if (option.automation != catalog::AutomationSupport::controlled ||
          (!option.default_selected && !option.required)) {
        continue;
      }
      add_selection(state, {.scheme_id = scheme.id,
                            .option_id = option.id,
                            .value = option.default_value});
    }
  }
  return state;
}

std::vector<SelectedOption> executable_selected_options(
    DiscoverySnapshot const& snapshot, SelectionState const& selection) {
  std::vector<SelectedOption> result;
  for (auto const& target : snapshot.targets) {
    for (auto const& scheme : target.schemes) {
      if (scheme.state != SchemeState::can_optimize) {
        continue;
      }
      for (auto const& option : scheme.options) {
        if (!option.selected ||
            option.state != OptionState::needs_optimization) {
          continue;
        }
        auto const selected_entry = std::ranges::find_if(
            selection.options, [&](SelectedOption const& entry) {
              return entry.scheme_id == scheme.scheme.id &&
                     entry.option_id == option.option.id;
            });
        if (selected_entry != selection.options.end()) {
          result.push_back(*selected_entry);
        }
      }
    }
  }
  return result;
}

SelectionChange change_selection(catalog::SoftwareOptimizationCatalog const& catalog_value,
                                 SelectionState current,
                                 SelectionMutation const& mutation) {
  auto const* option = find_option(catalog_value, mutation.scheme_id.value,
                                   mutation.option_id.value);
  if (option == nullptr) {
    return {.reason = "the option is not part of the current catalog"};
  }
  if (option->required && !mutation.selected) {
    return {.state = std::move(current),
            .reason = "a required option cannot be deselected"};
  }

  SelectionState proposed = current;
  SelectionAdjustment adjustment;
  if (!mutation.selected) {
    remove_selection(proposed, mutation.scheme_id.value, mutation.option_id.value);
  } else {
    add_selection(proposed, {.scheme_id = mutation.scheme_id,
                             .option_id = mutation.option_id,
                             .value = option->default_value});
    for (auto const& required_id : option->required_option_ids) {
      auto const* required = find_option(catalog_value, mutation.scheme_id.value,
                                         required_id.value);
      if (required == nullptr) {
        return {.state = std::move(current),
                .reason = "the catalog references a missing required option"};
      }
      if (!selected(proposed, mutation.scheme_id.value, required_id.value)) {
        SelectedOption entry{.scheme_id = mutation.scheme_id,
                             .option_id = required->id,
                             .value = required->default_value};
        adjustment.added_required.push_back(entry);
        add_selection(proposed, std::move(entry));
      }
    }
    for (auto const& conflict_id : option->conflicting_option_ids) {
      if (selected(proposed, mutation.scheme_id.value, conflict_id.value)) {
        auto const* conflict = find_option(catalog_value, mutation.scheme_id.value,
                                           conflict_id.value);
        if (conflict == nullptr || conflict->required) {
          return {.state = std::move(current),
                  .reason = "the requested option conflicts with a required option"};
        }
        auto const found = std::ranges::find_if(
            proposed.options, [&](SelectedOption const& entry) {
              return entry.scheme_id == mutation.scheme_id &&
                     entry.option_id == conflict_id;
            });
        if (found != proposed.options.end()) {
          adjustment.removed_conflicting.push_back(*found);
        }
        remove_selection(proposed, mutation.scheme_id.value, conflict_id.value);
      }
    }
  }

  if (!adjustment.empty() && !mutation.accept_adjustments) {
    return {.code = SelectionChangeCode::confirmation_required,
            .applied = false,
            .state = std::move(proposed),
            .adjustment = std::move(adjustment),
            .reason = "the requested selection changes required or conflicting options"};
  }
  return {.code = SelectionChangeCode::applied,
          .applied = true,
          .state = std::move(proposed),
          .adjustment = std::move(adjustment)};
}

char const* to_string(TargetPresence value) noexcept {
  switch (value) {
    case TargetPresence::detected: return "detected";
    case TargetPresence::externally_recognized: return "externally-recognized";
    case TargetPresence::absent: return "absent";
    case TargetPresence::unknown: return "unknown";
    case TargetPresence::skipped: return "skipped";
  }
  return "unknown";
}

char const* to_string(OptionState value) noexcept {
  switch (value) {
    case OptionState::needs_optimization: return "needs-optimization";
    case OptionState::optimized: return "optimized";
    case OptionState::version_not_applicable: return "version-not-applicable";
    case OptionState::unknown: return "unknown";
  }
  return "unknown";
}

char const* to_string(SchemeState value) noexcept {
  switch (value) {
    case SchemeState::can_optimize: return "can-optimize";
    case SchemeState::needs_attention: return "needs-attention";
    case SchemeState::optimized: return "optimized";
    case SchemeState::version_not_applicable: return "version-not-applicable";
    case SchemeState::emergency_withdrawn: return "emergency-withdrawn";
    case SchemeState::configuration_error: return "configuration-error";
    case SchemeState::manual_only: return "manual-only";
  }
  return "needs-attention";
}

}  // namespace azzs::domain::software_optimization_discovery
