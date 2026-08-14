#include "azzs/domain/installation_batch.hpp"

#include <algorithm>
#include <functional>
#include <ranges>

namespace azzs::domain::installation_batch {
namespace {

[[nodiscard]] bool nonempty_and_bounded(std::string const& value,
                                        std::size_t maximum = 256) noexcept {
  return !value.empty() && value.size() <= maximum;
}

template <typename Value, typename Projection>
[[nodiscard]] bool unique_by(std::vector<Value> const& values,
                             Projection projection) noexcept {
  for (auto outer = values.begin(); outer != values.end(); ++outer) {
    if (std::ranges::find(std::next(outer), values.end(), std::invoke(projection, *outer),
                          projection) != values.end()) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool contains_package(
    selection::ResolvedSourceSnapshot const& source,
    selection::ResolvedPackage const& package) noexcept {
  return std::ranges::find(source.packages, package) != source.packages.end();
}

}  // namespace

bool FrozenPreferenceChoice::valid() const noexcept {
  return nonempty_and_bounded(preference_id);
}

bool FrozenExecutionProfile::valid() const noexcept {
  auto const execution_valid =
      execution == catalog::WindowsExecutionReadiness::declaration_only ||
      execution == catalog::WindowsExecutionReadiness::project_executor_registered;
  auto const boundary_valid =
      completion_boundary ==
          catalog::InstallationCompletionBoundary::post_install_then_result_detection ||
      completion_boundary ==
          catalog::InstallationCompletionBoundary::post_install_then_restart_verification;
  auto const post_valid = post_install == catalog::PostInstallBehavior::none ||
                          post_install ==
                              catalog::PostInstallBehavior::controlled_preferences;
  auto const restart_valid = restart == catalog::RestartVerification::not_required ||
                             restart ==
                                 catalog::RestartVerification::required_after_restart;
  auto const detection_valid =
      result_detection == catalog::ResultDetectionStrategy::project_owned_presence_probe ||
      result_detection == catalog::ResultDetectionStrategy::user_confirmation_only;
  auto const scope_valid =
      interaction_scope ==
          catalog::InstallerInteractionScope::non_identity_preferences_only ||
      interaction_scope == catalog::InstallerInteractionScope::official_identity_required;
  auto const disposition_valid =
      interaction_disposition == catalog::InteractionDisposition::controlled_automatic ||
      interaction_disposition == catalog::InteractionDisposition::workbench_confirmation ||
      interaction_disposition == catalog::InteractionDisposition::official_installer;
  return executor == catalog::ControlledWindowsExecutionKind::project_owned_windows_executor &&
         execution_valid && boundary_valid && post_valid && restart_valid &&
         detection_valid && scope_valid && disposition_valid &&
         ((completion_boundary ==
               catalog::InstallationCompletionBoundary::post_install_then_restart_verification) ==
          (restart == catalog::RestartVerification::required_after_restart)) &&
         nonempty_and_bounded(profile_id) &&
         nonempty_and_bounded(baseline.id) &&
         nonempty_and_bounded(baseline.version) && choices.size() <= 64 &&
         std::ranges::all_of(choices, &FrozenPreferenceChoice::valid) &&
         unique_by(choices, &FrozenPreferenceChoice::preference_id);
}

bool FrozenCatalogSnapshot::valid() const noexcept {
  return !raw_catalog_bytes.empty() && raw_catalog_bytes.size() <= 1024U * 1024U &&
         nonempty_and_bounded(content_identity) &&
         nonempty_and_bounded(application_id) && schema_version > 0 && revision > 0;
}

bool FrozenInstallationItem::is_external_handoff() const noexcept {
  return selected_package.package_type == selection::PackageType::external_handoff;
}

bool FrozenInstallationItem::valid() const noexcept {
  return nonempty_and_bounded(item_id) && dependencies.size() <= 128 &&
         std::ranges::all_of(dependencies, [](std::string const& dependency) {
           return nonempty_and_bounded(dependency);
         }) && unique_by(dependencies, [](std::string const& value) -> std::string const& {
           return value;
         }) && source.valid() && source.software_id == item_id &&
         contains_package(source, selected_package) && execution_profile.valid() &&
         !is_external_handoff() && cache_asset.valid() && cache_root.valid();
}

bool FrozenBatchPlan::valid() const noexcept {
  if (!nonempty_and_bounded(batch_id) || !nonempty_and_bounded(correlation_id) ||
      (retry_of_batch_id.has_value() && !nonempty_and_bounded(*retry_of_batch_id)) ||
      !catalog.valid() || items.empty() || items.size() > 128 ||
      !std::ranges::all_of(items, &FrozenInstallationItem::valid) ||
      !unique_by(items, &FrozenInstallationItem::item_id)) {
    return false;
  }
  for (auto const& item : items) {
    for (auto const& dependency : item.dependencies) {
      if (dependency == item.item_id ||
          std::ranges::find(items, dependency,
                            &FrozenInstallationItem::item_id) == items.end()) {
        return false;
      }
    }
  }
  return true;
}

bool InstallationItemProgress::valid() const noexcept {
  return nonempty_and_bounded(item_id) && attempt <= 1024 &&
         (!opaque_installer_handle.has_value() ||
          nonempty_and_bounded(*opaque_installer_handle));
}

bool DurableLeaseBinding::valid() const noexcept {
  return nonempty_and_bounded(kind) && nonempty_and_bounded(operation_id) &&
         nonempty_and_bounded(correlation_id) &&
         nonempty_and_bounded(lease_token) && occupancy_revision > 0;
}

bool LastDurableTransition::valid() const noexcept {
  return generation > 0 && nonempty_and_bounded(item_id);
}

bool InstallationBatchRecord::valid() const noexcept {
  if (!plan.valid() || items.size() != plan.items.size() || generation == 0 ||
      !std::ranges::all_of(items, &InstallationItemProgress::valid) ||
      !unique_by(items, &InstallationItemProgress::item_id) ||
      (active_lease.has_value() && !active_lease->valid()) ||
      !last_transition.valid()) {
    return false;
  }
  for (auto const& item : plan.items) {
    auto const progress = std::ranges::find(items, item.item_id,
                                            &InstallationItemProgress::item_id);
    if (progress == items.end()) {
      return false;
    }
  }
  return true;
}

bool InstallationBatchHistory::valid() const noexcept {
  return plan.valid() && items.size() == plan.items.size() &&
         std::ranges::all_of(items, &InstallationItemProgress::valid) &&
         nonempty_and_bounded(reason);
}

bool is_terminal(InstallationItemState state) noexcept {
  return state == InstallationItemState::source_invalid ||
         state == InstallationItemState::failed ||
         state == InstallationItemState::skipped_installed ||
         state == InstallationItemState::dependency_blocked ||
         state == InstallationItemState::succeeded ||
         state == InstallationItemState::stop_pending;
}

bool blocks_batch(InstallationItemState state) noexcept {
  return state == InstallationItemState::installer_interaction_pending ||
         state == InstallationItemState::result_confirmation_pending ||
         state == InstallationItemState::waiting_restart;
}

bool requires_fresh_retry_snapshot(InstallationItemState state) noexcept {
  return state == InstallationItemState::source_invalid ||
         state == InstallationItemState::failed;
}

bool command_allowed(InstallationItemState state,
                     InstallationItemCommand command) noexcept {
  switch (state) {
    case InstallationItemState::pending:
    case InstallationItemState::waiting_network:
      return command == InstallationItemCommand::start ||
             command == InstallationItemCommand::stop;
    case InstallationItemState::source_invalid:
    case InstallationItemState::failed:
      return command == InstallationItemCommand::retry ||
             command == InstallationItemCommand::stop;
    case InstallationItemState::installer_interaction_pending:
      return command == InstallationItemCommand::user_complete_installer_interaction ||
             command == InstallationItemCommand::stop ||
             command == InstallationItemCommand::read_only_verify;
    case InstallationItemState::result_confirmation_pending:
      return command == InstallationItemCommand::user_complete_confirmation ||
             command == InstallationItemCommand::read_only_verify;
    case InstallationItemState::waiting_restart:
      return command == InstallationItemCommand::read_only_verify;
    case InstallationItemState::downloading:
      return command == InstallationItemCommand::stop ||
             command == InstallationItemCommand::read_only_verify;
    case InstallationItemState::installer_running:
      return command == InstallationItemCommand::read_only_verify;
    case InstallationItemState::skipped_installed:
    case InstallationItemState::dependency_blocked:
    case InstallationItemState::succeeded:
    case InstallationItemState::stop_pending:
      return false;
  }
  return false;
}

char const* to_string(FrozenResourceKind value) noexcept {
  switch (value) {
    case FrozenResourceKind::cached_package:
      return "cached_package";
    case FrozenResourceKind::controlled_download:
      return "controlled_download";
    case FrozenResourceKind::managed_source:
      return "managed_source";
  }
  return "unknown";
}

char const* to_string(InstallationItemState value) noexcept {
  switch (value) {
    case InstallationItemState::pending:
      return "pending";
    case InstallationItemState::downloading:
      return "downloading";
    case InstallationItemState::installer_running:
      return "installer_running";
    case InstallationItemState::waiting_network:
      return "waiting_network";
    case InstallationItemState::source_invalid:
      return "source_invalid";
    case InstallationItemState::installer_interaction_pending:
      return "installer_interaction_pending";
    case InstallationItemState::result_confirmation_pending:
      return "result_confirmation_pending";
    case InstallationItemState::waiting_restart:
      return "waiting_restart";
    case InstallationItemState::failed:
      return "failed";
    case InstallationItemState::skipped_installed:
      return "skipped_installed";
    case InstallationItemState::dependency_blocked:
      return "dependency_blocked";
    case InstallationItemState::succeeded:
      return "succeeded";
    case InstallationItemState::stop_pending:
      return "stop_pending";
  }
  return "unknown";
}

char const* to_string(InstallationBatchState value) noexcept {
  switch (value) {
    case InstallationBatchState::ready:
      return "ready";
    case InstallationBatchState::running:
      return "running";
    case InstallationBatchState::awaiting_user:
      return "awaiting_user";
    case InstallationBatchState::waiting_restart:
      return "waiting_restart";
    case InstallationBatchState::closing:
      return "closing";
    case InstallationBatchState::stopped:
      return "stopped";
    case InstallationBatchState::completed:
      return "completed";
    case InstallationBatchState::recovery_required:
      return "recovery_required";
    case InstallationBatchState::failed_closed:
      return "failed_closed";
  }
  return "unknown";
}

}  // namespace azzs::domain::installation_batch
