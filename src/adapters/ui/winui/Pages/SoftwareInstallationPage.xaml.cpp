#include "pch.h"

#include "SoftwareInstallationPage.xaml.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.xaml.h"
#include "DesignSystem/presentation_contract.hpp"
#include "DesignSystem/software_selection_presentation.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"
#include "azzs/application/restart_resume.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#if __has_include("Pages/SoftwareInstallationPage.g.cpp")
#include "Pages/SoftwareInstallationPage.g.cpp"
#endif

namespace {

namespace batch = azzs::domain::installation_batch;
namespace presentation = azzs::ui::presentation;

using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
using winrt::Microsoft::UI::Xaml::Controls::Border;
using winrt::Microsoft::UI::Xaml::Controls::CheckBox;
using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

std::atomic<std::uint64_t> batch_sequence{0};

[[nodiscard]] winrt::hstring resource_string(wchar_t const* key) {
  return ResourceLoader{}.GetString(key);
}

[[nodiscard]] winrt::hstring software_name(
    azzs::domain::software_catalog::RuntimeSoftwareCatalog const& catalog,
    std::string_view software_id) {
  auto const found = std::ranges::find_if(
      catalog.software, [software_id](
                           azzs::domain::software_catalog::RuntimeSoftware const& item) {
        return item.definition.id == software_id;
      });
  if (found == catalog.software.end() || found->definition.name.empty()) {
    return winrt::to_hstring(std::string{software_id});
  }
  return winrt::to_hstring(found->definition.name);
}

[[nodiscard]] std::int64_t now_milliseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::string next_batch_identity(std::int64_t now) {
  auto const sequence = batch_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  return "installation-batch-" + std::to_string(now) + "-" +
         std::to_string(sequence);
}

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
  auto const resources = ResourceLoader{};
  projecting_ = true;
  struct ProjectionGuard final {
    bool& value;
    ~ProjectionGuard() { value = false; }
  } projection_guard{projecting_};
  LocalTrialInfoBar().IsOpen(
      snapshot.active_catalog.has_value() &&
      snapshot.active_catalog->identity ==
          azzs::application::software_catalog::EffectiveCatalogIdentity::
              local_trial);
  BasicSoftwareItems().Children().Clear();
  NormalSoftwareItems().Children().Clear();

  auto const catalog_snapshot =
      services_ ? services_->software_catalog().snapshot()
                : azzs::application::software_catalog::SoftwareCatalogLifecycleSnapshot{};
  if (!catalog_snapshot.current_catalog.has_value()) {
    set_catalog_status(
        snapshot.error.empty()
            ? resource_string(L"SoftwareInstallationNoCurrentCatalog")
            : winrt::to_hstring(snapshot.error),
        InfoBarSeverity::Warning);
  } else {
    SoftwareCatalogStatusInfoBar().IsOpen(false);
  }

  auto const add_item = [&](StackPanel target,
                            azzs::domain::software_selection::SelectionItem const& item) {
    auto card = Border{};
    card.BorderThickness({1, 1, 1, 1});
    card.CornerRadius({4, 4, 4, 4});
    card.Padding({12, 12, 12, 12});
    auto content = StackPanel{};
    content.Spacing(6);
    auto const name = software_name(*catalog_snapshot.current_catalog, item.software_id);
    auto check_box = CheckBox{};
    check_box.Content(winrt::box_value(name));
    check_box.Tag(winrt::box_value(winrt::to_hstring(item.software_id)));
    check_box.IsChecked(item.selected);
    auto const enabled = item.available && !item.requires_reselection &&
                         snapshot.subject_writable &&
                         snapshot.mode ==
                             azzs::application::software_selection::SelectionLifecycleMode::
                                 ready;
    check_box.IsEnabled(enabled);
    AutomationProperties::SetName(check_box, name);
    check_box.Checked({this, &SoftwareInstallationPage::OnSoftwareSelectionChanged});
    check_box.Unchecked({this, &SoftwareInstallationPage::OnSoftwareSelectionChanged});
    content.Children().Append(check_box);
    if (!enabled) {
      auto reason = TextBlock{};
      reason.Text(item.reason.empty()
                      ? resource_string(L"SoftwareInstallationUnavailableReason")
                      : winrt::to_hstring(item.reason));
      reason.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
      content.Children().Append(reason);
    }
    card.Child(content);
    target.Children().Append(card);
  };

  if (catalog_snapshot.current_catalog.has_value()) {
    for (auto const& item : snapshot.items) {
      if (item.basic) {
        add_item(BasicSoftwareItems(), item);
      } else {
        add_item(NormalSoftwareItems(), item);
      }
    }
  }
  BasicSoftwareGroup().Visibility(
      BasicSoftwareItems().Children().Size() == 0
          ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
          : winrt::Microsoft::UI::Xaml::Visibility::Visible);
  NormalSoftwareGroup().Visibility(
      NormalSoftwareItems().Children().Size() == 0
          ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
          : winrt::Microsoft::UI::Xaml::Visibility::Visible);
  auto const selected_count = std::ranges::count_if(
      snapshot.items, [](auto const& item) { return item.selected; });
  CreateBatchButton().IsEnabled(catalog_snapshot.current_catalog.has_value() &&
                                snapshot.mode ==
                                    azzs::application::software_selection::SelectionLifecycleMode::
                                        ready &&
                                selected_count != 0);
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

void SoftwareInstallationPage::set_catalog_status(
    winrt::hstring const& message, InfoBarSeverity severity) {
  SoftwareCatalogStatusInfoBar().Title(
      resource_string(L"SoftwareInstallationCatalogStatusTitle"));
  SoftwareCatalogStatusInfoBar().Message(message);
  SoftwareCatalogStatusInfoBar().Severity(severity);
  SoftwareCatalogStatusInfoBar().IsOpen(!message.empty());
}

void SoftwareInstallationPage::OnSoftwareSelectionChanged(
    winrt::Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (projecting_ || !services_) {
    return;
  }
  try {
    auto const check_box = sender.try_as<CheckBox>();
    if (!check_box || !check_box.Tag()) {
      return;
    }
    auto const software_id =
        winrt::to_string(winrt::unbox_value<winrt::hstring>(check_box.Tag()));
    auto const selected = check_box.IsChecked() && check_box.IsChecked().Value();
    auto const result = services_->software_selection().select(software_id, selected);
    refresh();
    if (!result.succeeded()) {
      set_catalog_status(
          result.message.empty()
              ? resource_string(L"SoftwareInstallationSelectionRejected")
              : winrt::to_hstring(result.message),
          InfoBarSeverity::Warning);
    }
  } catch (...) {
    set_catalog_status(resource_string(L"SoftwareInstallationSelectionRejected"),
                      InfoBarSeverity::Error);
  }
}

void SoftwareInstallationPage::OnCreateBatch(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!services_) {
    return;
  }
  auto const selection = services_->software_selection().snapshot();
  auto request =
      azzs::application::installation_batch::InstallationBatchCreateRequest{};
  auto const frozen_at = now_milliseconds();
  request.batch_id = next_batch_identity(frozen_at);
  request.correlation_id = request.batch_id;
  request.frozen_at_milliseconds = frozen_at > 0 ? frozen_at : 1;
  for (auto const& item : selection.items) {
    if (!item.selected) {
      continue;
    }
    azzs::application::installation_batch::InstallationPackageChoice choice{
        .software_id = item.software_id};
    auto const source = std::ranges::find_if(
        selection.sources, [&](auto const& candidate) {
          return candidate.software_id == item.software_id &&
                 candidate.declared_purpose ==
                     azzs::domain::software_catalog::SourcePurpose::primary;
        });
    if (source != selection.sources.end()) {
      choice.declared_purpose = source->declared_purpose;
      choice.declared_address = source->declared_address;
      if (!source->packages.empty()) {
        choice.package_identity = source->packages.front().candidate.identity;
      }
    }
    request.packages.push_back(std::move(choice));
  }
  auto const result = services_->installation_batch_creation().create(request);
  refresh();
  if (result.assessment.ready() && result.batch.succeeded()) {
    set_catalog_status(resource_string(L"SoftwareInstallationBatchCreated"),
                      InfoBarSeverity::Success);
  } else {
    auto const detail = result.assessment.detail.empty()
                            ? resource_string(L"SoftwareInstallationBatchCreateFailed")
                            : winrt::to_hstring(result.assessment.detail);
    set_catalog_status(detail, InfoBarSeverity::Warning);
  }
}

void SoftwareInstallationPage::handle_installation_batch_intent(
    presentation::PresentationIntent const& intent) {
  if (!services_ || intent.target_id != "installation-batch.status") {
    return;
  }
  auto& batches = services_->installation_batches();
  auto& restart_resume = services_->restart_resume();
  if (intent.command_id == "pause-download") {
    static_cast<void>(batches.pause_current_download());
  } else if (intent.command_id == "resume-download") {
    static_cast<void>(batches.resume_current_download());
  } else if (intent.command_id == "stop-batch") {
    if (restart_resume.snapshot().state !=
            azzs::application::restart_resume::RestartResumeState::awaiting_user_decision ||
        restart_resume.cancel().succeeded()) {
      static_cast<void>(batches.stop_current());
    }
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
    if (restart_resume.snapshot().state !=
            azzs::application::restart_resume::RestartResumeState::awaiting_user_decision ||
        restart_resume.confirm_continue().succeeded()) {
      static_cast<void>(batches.continue_after_recovery());
    }
  }
  refresh();
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
