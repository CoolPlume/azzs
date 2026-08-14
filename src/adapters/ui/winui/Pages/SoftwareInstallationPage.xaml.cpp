#include "pch.h"

#include "SoftwareInstallationPage.xaml.h"

#include <algorithm>
#include <string>
#include <utility>

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.xaml.h"
#include "DesignSystem/presentation_contract.hpp"
#include "DesignSystem/software_selection_presentation.hpp"

#if __has_include("Pages/SoftwareInstallationPage.g.cpp")
#include "Pages/SoftwareInstallationPage.g.cpp"
#endif

namespace {

namespace batch = azzs::domain::installation_batch;
namespace presentation = azzs::ui::presentation;

void add_batch_command(presentation::ComponentProjection& component,
                       std::string id,
                       std::string label,
                       presentation::CommandRole role,
                       presentation::IntentKind kind,
                       bool default_focus = false) {
  auto command_id = id;
  component.commands.push_back({
      .id = std::move(id),
      .label = std::move(label),
      .role = role,
      .default_focus = default_focus,
      .intent = {.kind = kind,
                 .target_id = component.id,
                 .command_id = std::move(command_id)},
  });
}

[[nodiscard]] std::shared_ptr<presentation::PresentationSnapshot const>
make_installation_batch_presentation(
    batch::InstallationBatchSnapshot const& snapshot,
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const& resources) {
  auto const text = [&resources](wchar_t const* key) {
    return winrt::to_string(resources.GetString(key));
  };
  auto component = presentation::ComponentProjection{
      .id = "installation-batch.status",
      .automation_id = "AzzsInstallationBatchStatus",
      .accessible_name = text(L"InstallationBatchStatusAccessibleName"),
      .kind = presentation::ComponentKind::progress,
      .state = presentation::PresentationState::neutral,
      .title = text(L"InstallationBatchStatusTitle"),
      .body = text(L"InstallationBatchNoActiveBody"),
  };
  if (!snapshot.writable) {
    component.kind = presentation::ComponentKind::disabled_reason;
    component.state = presentation::PresentationState::disabled;
    component.body = text(L"InstallationBatchReadOnlyBody");
    return std::make_shared<presentation::PresentationSnapshot>(
        std::vector<presentation::ComponentProjection>{std::move(component)});
  }
  if (!snapshot.active.has_value()) {
    return std::make_shared<presentation::PresentationSnapshot>(
        std::vector<presentation::ComponentProjection>{std::move(component)});
  }

  auto const& active = *snapshot.active;
  auto const current = std::ranges::find_if(
      active.items, [](batch::InstallationItemProgress const& item) {
        return !batch::is_terminal(item.state);
      });
  auto const* current_item =
      current == active.items.end() ? nullptr : std::addressof(*current);
  std::uint64_t processed{};
  for (auto const& item : active.items) {
    if (batch::is_terminal(item.state)) {
      ++processed;
    }
  }
  component.progress = presentation::ProgressProjection{
      .kind = presentation::ProgressKind::determinate,
      .completed = processed,
      .total = static_cast<std::uint64_t>(active.items.size()),
      .accessible_value = text(L"InstallationBatchProgressPrefix") + " " +
                          std::to_string(processed) + " / " +
                          std::to_string(active.items.size()),
  };
  component.advanced_detail = text(L"InstallationBatchProgressPrefix") + " " +
                              std::to_string(processed) + " / " +
                              std::to_string(active.items.size());
  switch (active.state) {
    case batch::InstallationBatchState::ready:
    case batch::InstallationBatchState::running:
      component.state = presentation::PresentationState::in_progress;
      component.body = text(L"InstallationBatchRunningBody");
      if (current_item != nullptr &&
          current_item->state == batch::InstallationItemState::downloading) {
        add_batch_command(component, "pause-download",
                          text(L"InstallationBatchPauseDownloadCommand"),
                          presentation::CommandRole::secondary,
                          presentation::IntentKind::continue_workflow);
        add_batch_command(component, "stop-batch",
                          text(L"InstallationBatchStopCommand"),
                          presentation::CommandRole::danger,
                          presentation::IntentKind::stop_safely);
      } else if (current_item != nullptr &&
                 current_item->state ==
                     batch::InstallationItemState::installer_running) {
        add_batch_command(component, "stop-batch",
                          text(L"InstallationBatchStopCommand"),
                          presentation::CommandRole::primary,
                          presentation::IntentKind::stop_safely, true);
        add_batch_command(component, "request-force-termination",
                          text(L"InstallationBatchRequestForceTerminationCommand"),
                          presentation::CommandRole::danger,
                          presentation::IntentKind::confirm_risk);
      }
      break;
    case batch::InstallationBatchState::download_paused:
      component.state = presentation::PresentationState::pending_confirmation;
      component.body = text(L"InstallationBatchDownloadPausedBody");
      add_batch_command(component, "resume-download",
                        text(L"InstallationBatchResumeDownloadCommand"),
                        presentation::CommandRole::primary,
                        presentation::IntentKind::continue_workflow, true);
      add_batch_command(component, "stop-batch",
                        text(L"InstallationBatchStopCommand"),
                        presentation::CommandRole::danger,
                        presentation::IntentKind::stop_safely);
      break;
    case batch::InstallationBatchState::stopping:
      component.state = presentation::PresentationState::in_progress;
      component.body = text(L"InstallationBatchStoppingBody");
      break;
    case batch::InstallationBatchState::awaiting_user:
      component.kind = presentation::ComponentKind::pending_confirmation;
      component.state = presentation::PresentationState::pending_confirmation;
      component.announcement = presentation::AnnouncementMode::polite;
      component.body = text(L"InstallationBatchAwaitingUserBody");
      if (current_item != nullptr &&
          current_item->state ==
              batch::InstallationItemState::force_termination_confirmation_pending) {
        component.kind = presentation::ComponentKind::risk_confirmation;
        component.risk = presentation::RiskLevel::high;
        component.body = text(L"InstallationBatchForceTerminationRiskBody");
        add_batch_command(component, "cancel-force-termination",
                          text(L"InstallationBatchCancelForceTerminationCommand"),
                          presentation::CommandRole::primary,
                          presentation::IntentKind::continue_workflow, true);
        add_batch_command(component, "confirm-force-termination",
                          text(L"InstallationBatchConfirmForceTerminationCommand"),
                          presentation::CommandRole::danger,
                          presentation::IntentKind::confirm_risk);
      } else if (current_item != nullptr &&
                 current_item->state ==
                     batch::InstallationItemState::result_confirmation_pending) {
        add_batch_command(component, "confirm-current-complete",
                          text(L"InstallationBatchConfirmCurrentCompleteCommand"),
                          presentation::CommandRole::primary,
                          presentation::IntentKind::continue_workflow, true);
      }
      break;
    case batch::InstallationBatchState::waiting_restart:
      component.kind = presentation::ComponentKind::waiting;
      component.state = presentation::PresentationState::waiting_for_restart;
      component.body = text(L"InstallationBatchWaitingRestartBody");
      break;
    case batch::InstallationBatchState::closing:
      component.kind = presentation::ComponentKind::waiting;
      component.state = presentation::PresentationState::in_progress;
      component.body = text(L"InstallationBatchClosingBody");
      break;
    case batch::InstallationBatchState::stopped:
      component.state = presentation::PresentationState::completed;
      component.body = text(L"InstallationBatchStoppedBody");
      break;
    case batch::InstallationBatchState::completed:
      component.state = presentation::PresentationState::completed;
      component.body = text(L"InstallationBatchCompletedBody");
      break;
    case batch::InstallationBatchState::recovery_required:
      component.kind = presentation::ComponentKind::pending_confirmation;
      component.state = presentation::PresentationState::pending_confirmation;
      component.announcement = presentation::AnnouncementMode::polite;
      component.body = text(L"InstallationBatchRecoveryBody");
      if (current_item != nullptr &&
          (current_item->state == batch::InstallationItemState::installer_running ||
           batch::blocks_batch(current_item->state))) {
        add_batch_command(component, "recover-read-only",
                          text(L"InstallationBatchRecoverReadOnlyCommand"),
                          presentation::CommandRole::primary,
                          presentation::IntentKind::continue_workflow, true);
      } else {
        add_batch_command(component, "continue-after-recovery",
                          text(L"InstallationBatchContinueAfterRecoveryCommand"),
                          presentation::CommandRole::primary,
                          presentation::IntentKind::continue_workflow, true);
      }
      break;
    case batch::InstallationBatchState::failed_closed:
      component.kind = presentation::ComponentKind::failure;
      component.state = presentation::PresentationState::failed;
      component.announcement = presentation::AnnouncementMode::assertive;
      component.body = text(L"InstallationBatchFailedClosedBody");
      break;
  }
  return std::make_shared<presentation::PresentationSnapshot>(
      std::vector<presentation::ComponentProjection>{std::move(component)});
}

}  // namespace

namespace winrt::Azzs::Ui::Pages::implementation {

SoftwareInstallationPage::SoftwareInstallationPage() {
  InitializeComponent();
  project({
      .mode = azzs::application::software_selection::SelectionLifecycleMode::
          not_restored,
  }, {}, {});
}

void SoftwareInstallationPage::bind(
    std::shared_ptr<azzs::application::WorkbenchServices> services) {
  services_ = std::move(services);
  refresh();
}

void SoftwareInstallationPage::refresh() {
  if (!services_) {
    project({.mode = azzs::application::software_selection::SelectionLifecycleMode::not_restored},
            {}, {});
    return;
  }
  project(services_->software_selection().snapshot(),
          services_->offline_package_cache().snapshot(),
          services_->installation_batches().snapshot());
}

void SoftwareInstallationPage::project(
    azzs::application::software_selection::SoftwareSelectionSnapshot const& snapshot,
    azzs::application::offline_package_cache::OfflinePackageCacheSnapshot const& cache,
    azzs::domain::installation_batch::InstallationBatchSnapshot const& batch) {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  auto const resources = ResourceLoader{};
  auto text = azzs::ui::presentation::SoftwareSelectionPresentationText{
      .accessible_name = winrt::to_string(
          resources.GetString(L"SoftwareSelectionStatusAccessibleName")),
      .available_title = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAvailableTitle")),
      .available_body_prefix = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAvailableBodyPrefix")),
      .available_body_suffix = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAvailableBodySuffix")),
      .absent_catalog_title = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAbsentCatalogTitle")),
      .absent_catalog_body = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAbsentCatalogBody")),
      .not_restored_body = winrt::to_string(
          resources.GetString(L"SoftwareSelectionNotRestoredBody")),
      .restore_failed_body = winrt::to_string(
          resources.GetString(L"SoftwareSelectionRestoreFailedBody")),
      .advanced_available = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAdvancedAvailable")),
      .advanced_absent_catalog = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAdvancedAbsentCatalog")),
  };
  using SurfaceImplementation = winrt::Azzs::Ui::DesignSystem::Controls::
      implementation::ReadOnlyPresentationSurface;
  winrt::get_self<SurfaceImplementation>(SoftwareSelectionStatus())->project(
      azzs::ui::presentation::make_software_selection_presentation(
          snapshot, std::move(text)),
      "software-selection.status", azzs::ui::presentation::ViewMode::standard,
      {}, 0, "SoftwareInstallation");
  auto cache_text = azzs::ui::presentation::OfflinePackageCachePresentationText{
      .accessible_name = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheStatusAccessibleName")),
      .available_title = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheAvailableTitle")),
      .unavailable_title = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheUnavailableTitle")),
      .available_body_prefix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheAvailableBodyPrefix")),
      .unavailable_body_prefix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheUnavailableBodyPrefix")),
      .item_suffix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheItemSuffix")),
      .network_suffix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheNetworkSuffix")),
  };
  winrt::get_self<SurfaceImplementation>(OfflinePackageCacheStatus())->project(
      azzs::ui::presentation::make_offline_package_cache_presentation(
          cache, std::move(cache_text)),
      "offline-package-cache.status", azzs::ui::presentation::ViewMode::standard,
      {}, 0, "SoftwareInstallation");
  winrt::get_self<SurfaceImplementation>(InstallationBatchStatus())->project(
      make_installation_batch_presentation(batch, resources),
      "installation-batch.status", azzs::ui::presentation::ViewMode::standard,
      [weak_this = get_weak()](presentation::PresentationIntent const& intent) {
        if (auto self = weak_this.get()) {
          self->handle_installation_batch_intent(intent);
        }
      },
      0, "SoftwareInstallation");
}

void SoftwareInstallationPage::handle_installation_batch_intent(
    presentation::PresentationIntent const& intent) {
  if (!services_ || intent.target_id != "installation-batch.status") {
    return;
  }
  auto& batches = services_->installation_batches();
  if (intent.command_id == "pause-download") {
    static_cast<void>(batches.pause_current_download());
  } else if (intent.command_id == "resume-download") {
    static_cast<void>(batches.resume_current_download());
  } else if (intent.command_id == "stop-batch") {
    static_cast<void>(batches.stop_current());
  } else if (intent.command_id == "request-force-termination") {
    static_cast<void>(batches.request_force_termination());
  } else if (intent.command_id == "confirm-force-termination") {
    static_cast<void>(batches.confirm_force_termination());
  } else if (intent.command_id == "cancel-force-termination") {
    static_cast<void>(batches.cancel_force_termination());
  } else if (intent.command_id == "confirm-current-complete") {
    static_cast<void>(batches.confirm_current_complete());
  } else if (intent.command_id == "recover-read-only") {
    static_cast<void>(batches.recover_read_only());
  } else if (intent.command_id == "continue-after-recovery") {
    static_cast<void>(batches.continue_after_recovery());
  }
  refresh();
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
