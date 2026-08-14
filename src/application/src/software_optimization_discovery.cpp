#include "azzs/application/software_optimization_discovery.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <utility>

#include "azzs/application/sogou_optimization.hpp"

namespace azzs::application::software_optimization_discovery {
namespace {

[[nodiscard]] bool is_sogou_target(catalog_domain::TargetSoftware const& target) {
  return target.install_detection.kind == catalog_domain::RuleKind::built_in_definition &&
         target.version_detection.kind == catalog_domain::RuleKind::built_in_definition &&
         target.install_detection.definition.value == "sogou.detect.installed.v1" &&
         target.version_detection.definition.value == "sogou.detect.version.v1";
}

[[nodiscard]] discovery_domain::TargetPresence presence_for(
    sogou_optimization::SogouOptimizationStatus status) noexcept {
  using Status = sogou_optimization::SogouOptimizationStatus;
  switch (status) {
    case Status::succeeded:
    case Status::already_effective:
    case Status::version_not_supported:
      return discovery_domain::TargetPresence::detected;
    case Status::not_installed:
      return discovery_domain::TargetPresence::absent;
    case Status::failed:
    case Status::pending_confirmation:
    case Status::invalid_request:
    case Status::unsupported:
    case Status::cancelled:
      return discovery_domain::TargetPresence::unknown;
  }
  return discovery_domain::TargetPresence::unknown;
}

[[nodiscard]] discovery_domain::OptionState option_state_for(
    sogou_optimization::SogouOptimizationStatus status) noexcept {
  using Status = sogou_optimization::SogouOptimizationStatus;
  switch (status) {
    case Status::already_effective:
      return discovery_domain::OptionState::optimized;
    case Status::succeeded:
      return discovery_domain::OptionState::needs_optimization;
    case Status::failed:
    case Status::pending_confirmation:
    case Status::not_installed:
    case Status::version_not_supported:
    case Status::invalid_request:
    case Status::unsupported:
    case Status::cancelled:
      return discovery_domain::OptionState::unknown;
  }
  return discovery_domain::OptionState::unknown;
}

[[nodiscard]] std::optional<domain::software_selection::ExternalHandoffRecord>
external_handoff_for(
    software_selection::SoftwareSelectionSnapshot const& snapshot,
    catalog_domain::TargetSoftware const& target) {
  if (!target.installation_item_id.has_value()) {
    return std::nullopt;
  }
  auto const found = std::ranges::find(
      snapshot.handoffs, target.installation_item_id->value,
      &domain::software_selection::ExternalHandoffRecord::software_id);
  return found == snapshot.handoffs.end() ? std::nullopt
                                          : std::optional{*found};
}

[[nodiscard]] bool is_executable(discovery_domain::SchemeState state) noexcept {
  return state == discovery_domain::SchemeState::can_optimize;
}

}  // namespace

SogouOptimizationDiscoveryObserver::SogouOptimizationDiscoveryObserver(
    sogou_optimization::SogouOptimizationService& service) noexcept
    : service_(service) {}

discovery_domain::TargetObservation
SogouOptimizationDiscoveryObserver::observe_target(
    catalog_domain::TargetSoftware const& target) {
  if (!is_sogou_target(target)) {
    return {.target_id = target.id,
            .presence = discovery_domain::TargetPresence::unknown,
            .detail = "no registered target observation capability"};
  }
  auto const observed = service_.detect_target();
  return {.target_id = target.id,
          .presence = presence_for(observed.status),
          .installed_version = observed.installed_version,
          .detail = observed.detail};
}

discovery_domain::OptionObservation
SogouOptimizationDiscoveryObserver::observe_option(
    catalog_domain::TargetSoftware const& target,
    catalog_domain::SoftwareOptimizationOption const& option) {
  if (!is_sogou_target(target)) {
    return {.option_id = option.id,
            .state = discovery_domain::OptionState::unknown,
            .detail = "no registered option observation capability"};
  }
  auto const observed = service_.detect_option(
      option, option.default_value.has_value()
                  ? std::optional<std::string_view>{*option.default_value}
                  : std::nullopt);
  return {.option_id = option.id,
          .state = option_state_for(observed.status),
          .detail = observed.detail};
}

SoftwareOptimizationDiscoveryService::SoftwareOptimizationDiscoveryService(
    SoftwareOptimizationCatalogLifecycle& catalogs,
    software_selection::SoftwareSelectionLifecycle const& selections,
    EmergencyWithdrawalService& withdrawals,
    SoftwareOptimizationObservationPort& observations) noexcept
    : catalogs_(catalogs),
      selections_(selections),
      withdrawals_(withdrawals),
      observations_(observations) {}

DiscoveryActionResult SoftwareOptimizationDiscoveryService::refresh() {
  auto snapshot = rebuild();
  return {.code = snapshot.has_current_catalog
                     ? DiscoveryActionCode::refreshed
                     : DiscoveryActionCode::no_current_catalog,
          .snapshot = std::move(snapshot),
          .message = snapshot_.error};
}

SoftwareOptimizationDiscoverySnapshot
SoftwareOptimizationDiscoveryService::snapshot() const {
  return snapshot_;
}

DiscoveryActionResult SoftwareOptimizationDiscoveryService::change_selection(
    discovery_domain::SelectionMutation mutation) {
  auto catalog_snapshot = catalogs_.snapshot();
  if (!catalog_snapshot.current.has_value()) {
    snapshot_ = {.error = catalog_snapshot.error};
    return {.code = DiscoveryActionCode::no_current_catalog,
            .snapshot = snapshot_,
            .message = snapshot_.error};
  }
  auto changed = discovery_domain::change_selection(*catalog_snapshot.current,
                                                     selection_, mutation);
  if (changed.code == discovery_domain::SelectionChangeCode::rejected) {
    return {.code = DiscoveryActionCode::selection_rejected,
            .snapshot = snapshot_,
            .adjustment = std::move(changed.adjustment),
            .message = std::move(changed.reason)};
  }
  if (changed.code == discovery_domain::SelectionChangeCode::confirmation_required) {
    return {.code = DiscoveryActionCode::adjustment_confirmation_required,
            .snapshot = snapshot_,
            .adjustment = std::move(changed.adjustment),
            .message = std::move(changed.reason)};
  }
  selection_ = std::move(changed.state);
  auto refreshed = rebuild();
  return {.code = DiscoveryActionCode::selection_changed,
          .snapshot = std::move(refreshed),
          .adjustment = std::move(changed.adjustment)};
}

DiscoveryActionResult SoftwareOptimizationDiscoveryService::prepare_submission() const {
  auto selected_options = executable_selected_options();
  if (selected_options.empty()) {
    return {.code = DiscoveryActionCode::no_executable_selection,
            .snapshot = snapshot_,
            .message = "no selected option is currently executable"};
  }
  return {.code = DiscoveryActionCode::selection_changed,
          .snapshot = snapshot_,
          .submission = SoftwareOptimizationSubmissionRequest{
              .catalog_revision = snapshot_.discovery.catalog_revision,
              .selected_options = std::move(selected_options)}};
}

SoftwareOptimizationDiscoverySnapshot
SoftwareOptimizationDiscoveryService::rebuild() {
  auto catalog_snapshot = catalogs_.snapshot();
  if (!catalog_snapshot.current.has_value()) {
    snapshot_ = {.has_current_catalog = false,
                 .defaults_initialized = defaults_initialized_,
                 .error = catalog_snapshot.error};
    return snapshot_;
  }
  if (!defaults_initialized_) {
    selection_ = discovery_domain::default_selection(*catalog_snapshot.current);
    defaults_initialized_ = true;
  }

  auto const selection_snapshot = selections_.snapshot();
  std::vector<discovery_domain::TargetObservation> targets;
  std::vector<discovery_domain::OptionObservation> options;
  std::vector<discovery_domain::WithdrawnOperation> withdrawn;
  for (auto const& target : catalog_snapshot.current->targets) {
    auto observed = observations_.observe_target(target);
    if (observed.target_id != target.id) {
      observed = {.target_id = target.id,
                  .presence = discovery_domain::TargetPresence::unknown,
                  .detail = "observation returned a mismatched target identity"};
    }
    if (auto const handoff = external_handoff_for(selection_snapshot, target);
        handoff.has_value()) {
      using domain::software_selection::ExternalHandoffStatus;
      if (handoff->status == ExternalHandoffStatus::externally_recognized) {
        observed.presence = discovery_domain::TargetPresence::externally_recognized;
      } else if (handoff->status == ExternalHandoffStatus::skipped &&
                 observed.presence == discovery_domain::TargetPresence::absent) {
        observed.presence = discovery_domain::TargetPresence::skipped;
      }
    }
    targets.push_back(observed);
    if (observed.presence != discovery_domain::TargetPresence::detected &&
        observed.presence != discovery_domain::TargetPresence::externally_recognized) {
      continue;
    }
    for (auto const& scheme : catalog_snapshot.current->schemes) {
      if (scheme.target_id != target.id) {
        continue;
      }
      auto const scheme_authorization = withdrawals_.authorize({
          .stable_id = scheme.id.value,
          .category = domain::emergency_withdrawal::OperationCategory::software_optimization,
          .version = observed.installed_version,
      });
      if (scheme_authorization.code == OperationAuthorizationCode::blocked) {
        withdrawn.push_back({.operation_id = scheme.id,
                             .reason = scheme_authorization.reason});
      }
      for (auto const& option : scheme.options) {
        options.push_back(observations_.observe_option(target, option));
        auto const option_authorization = withdrawals_.authorize({
            .stable_id = option.id.value,
            .category = domain::emergency_withdrawal::OperationCategory::software_optimization,
            .version = observed.installed_version,
        });
        if (option_authorization.code == OperationAuthorizationCode::blocked) {
          withdrawn.push_back({.operation_id = option.id,
                               .reason = option_authorization.reason});
        }
      }
    }
  }
  snapshot_ = {
      .has_current_catalog = true,
      .defaults_initialized = defaults_initialized_,
      .discovery = discovery_domain::discover({
          .catalog = *catalog_snapshot.current,
          .targets = targets,
          .options = options,
          .withdrawn_operations = withdrawn,
          .selection = selection_,
      }),
      .selection = selection_,
      .error = {},
  };
  return snapshot_;
}

std::vector<discovery_domain::SelectedOption>
SoftwareOptimizationDiscoveryService::executable_selected_options() const {
  std::vector<discovery_domain::SelectedOption> result;
  for (auto const& target : snapshot_.discovery.targets) {
    for (auto const& scheme : target.schemes) {
      if (!is_executable(scheme.state)) {
        continue;
      }
      for (auto const& option : scheme.options) {
        if (option.selected &&
            option.state == discovery_domain::OptionState::needs_optimization) {
          auto const selected = std::ranges::find_if(
              selection_.options, [&](auto const& entry) {
                return entry.scheme_id == scheme.scheme.id &&
                       entry.option_id == option.option.id;
              });
          if (selected != selection_.options.end()) {
            result.push_back(*selected);
          }
        }
      }
    }
  }
  return result;
}

char const* to_string(DiscoveryActionCode value) noexcept {
  switch (value) {
    case DiscoveryActionCode::refreshed: return "refreshed";
    case DiscoveryActionCode::no_current_catalog: return "no-current-catalog";
    case DiscoveryActionCode::selection_changed: return "selection-changed";
    case DiscoveryActionCode::adjustment_confirmation_required:
      return "adjustment-confirmation-required";
    case DiscoveryActionCode::selection_rejected: return "selection-rejected";
    case DiscoveryActionCode::no_executable_selection:
      return "no-executable-selection";
  }
  return "no-current-catalog";
}

}  // namespace azzs::application::software_optimization_discovery
