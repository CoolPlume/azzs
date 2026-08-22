#include "pch.h"

#include "OverviewPage.xaml.h"

#include <optional>
#include <string_view>
#include <utility>

#include "../DesignSystem/Controls/ReadOnlyPresentationSurface.xaml.h"
#include "../DesignSystem/guided_initialization_presentation.hpp"
#include "azzs/application/guided_initialization.hpp"
#include "azzs/application/workbench_services.hpp"

#if __has_include("Pages/OverviewPage.g.cpp")
#include "Pages/OverviewPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {

namespace {

using azzs::application::PageId;
using azzs::ui::presentation::IntentKind;
using SurfaceImplementation =
    winrt::Azzs::Ui::DesignSystem::Controls::implementation::
        ReadOnlyPresentationSurface;

[[nodiscard]] std::string localized_string(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const& resources,
    wchar_t const* key, std::string fallback) {
  auto const value = resources.GetString(key);
  return value.empty() ? std::move(fallback) : winrt::to_string(value);
}

[[nodiscard]] azzs::ui::presentation::GuidedInitializationPresentationText
load_presentation_text() {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  auto const resources = ResourceLoader{};
  azzs::ui::presentation::GuidedInitializationPresentationText text;
  text.summary_accessible_name = localized_string(resources,
      L"OverviewGuidedSummaryAccessibleName", std::move(text.summary_accessible_name));
  text.summary_title = localized_string(resources,
      L"OverviewGuidedSummaryTitle", std::move(text.summary_title));
  text.summary_prefix = localized_string(resources,
      L"OverviewGuidedSummaryCompletedPrefix", std::move(text.summary_prefix));
  text.summary_external_prefix = localized_string(resources,
      L"OverviewGuidedSummaryExternalPrefix", std::move(text.summary_external_prefix));
  text.summary_partial_prefix = localized_string(resources,
      L"OverviewGuidedSummaryPartialPrefix", std::move(text.summary_partial_prefix));
  text.summary_failed_prefix = localized_string(resources,
      L"OverviewGuidedSummaryFailedPrefix", std::move(text.summary_failed_prefix));
  text.summary_skipped_prefix = localized_string(resources,
      L"OverviewGuidedSummarySkippedPrefix", std::move(text.summary_skipped_prefix));
  text.summary_no_applicable_prefix = localized_string(resources,
      L"OverviewGuidedSummaryNoApplicablePrefix", std::move(text.summary_no_applicable_prefix));
  text.summary_not_executed_prefix = localized_string(resources,
      L"OverviewGuidedSummaryNotExecutedPrefix", std::move(text.summary_not_executed_prefix));
  text.summary_confirmation_prefix = localized_string(resources,
      L"OverviewGuidedSummaryConfirmationPrefix", std::move(text.summary_confirmation_prefix));
  text.summary_explorer_restart_prefix = localized_string(resources,
      L"OverviewGuidedSummaryExplorerRestartPrefix", std::move(text.summary_explorer_restart_prefix));
  text.summary_restart_prefix = localized_string(resources,
      L"OverviewGuidedSummaryRestartPrefix", std::move(text.summary_restart_prefix));
  text.summary_withdrawn_prefix = localized_string(resources,
      L"OverviewGuidedSummaryWithdrawnPrefix", std::move(text.summary_withdrawn_prefix));
  text.summary_error_suffix = localized_string(resources,
      L"OverviewGuidedSummaryErrorSuffix", std::move(text.summary_error_suffix));
  text.start_command = localized_string(resources, L"OverviewGuidedStartCommand", std::move(text.start_command));
  text.refresh_command = localized_string(resources, L"OverviewGuidedRefreshCommand", std::move(text.refresh_command));
  text.cancel_command = localized_string(resources, L"OverviewGuidedCancelCommand", std::move(text.cancel_command));
  text.history_command = localized_string(resources, L"OverviewGuidedHistoryCommand", std::move(text.history_command));
  text.skip_command = localized_string(resources, L"OverviewGuidedSkipCommand", std::move(text.skip_command));
  text.continue_command = localized_string(resources, L"OverviewGuidedContinueCommand", std::move(text.continue_command));
  text.retry_command = localized_string(resources, L"OverviewGuidedRetryCommand", std::move(text.retry_command));
  text.open_command = localized_string(resources, L"OverviewGuidedOpenCommand", std::move(text.open_command));
  text.local_trial_accessible_name = localized_string(resources, L"OverviewGuidedLocalTrialAccessibleName", std::move(text.local_trial_accessible_name));
  text.local_trial_title = localized_string(resources, L"OverviewGuidedLocalTrialTitle", std::move(text.local_trial_title));
  text.local_trial_body = localized_string(resources, L"OverviewGuidedLocalTrialBody", std::move(text.local_trial_body));
  text.handoff_accessible_name = localized_string(resources, L"OverviewGuidedHandoffAccessibleName", std::move(text.handoff_accessible_name));
  text.handoff_title = localized_string(resources, L"OverviewGuidedHandoffTitle", std::move(text.handoff_title));
  text.handoff_waiting_body = localized_string(resources, L"OverviewGuidedHandoffWaitingBody", std::move(text.handoff_waiting_body));
  text.handoff_recognized_body = localized_string(resources, L"OverviewGuidedHandoffRecognizedBody", std::move(text.handoff_recognized_body));
  text.handoff_continue_command = localized_string(resources, L"OverviewGuidedHandoffContinueCommand", std::move(text.handoff_continue_command));
  text.read_only_accessible_name = localized_string(resources, L"OverviewGuidedReadOnlyAccessibleName", std::move(text.read_only_accessible_name));
  text.read_only_title = localized_string(resources, L"OverviewGuidedReadOnlyTitle", std::move(text.read_only_title));
  text.read_only_body = localized_string(resources, L"OverviewGuidedReadOnlyBody", std::move(text.read_only_body));
  text.read_only_disabled_reason = localized_string(resources, L"OverviewGuidedReadOnlyDisabledReason", std::move(text.read_only_disabled_reason));
  text.stage_empty_body = localized_string(resources, L"OverviewGuidedStageEmptyBody", std::move(text.stage_empty_body));
  text.raw_detail_prefix = localized_string(resources, L"OverviewGuidedRawDetailPrefix", std::move(text.raw_detail_prefix));
  text.raw_error_prefix = localized_string(resources, L"OverviewGuidedRawErrorPrefix", std::move(text.raw_error_prefix));
  text.drivers_stage_title = localized_string(resources, L"OverviewGuidedDriversStageTitle", std::move(text.drivers_stage_title));
  text.system_optimization_stage_title = localized_string(resources, L"OverviewGuidedSystemOptimizationStageTitle", std::move(text.system_optimization_stage_title));
  text.software_installation_stage_title = localized_string(resources, L"OverviewGuidedSoftwareInstallationStageTitle", std::move(text.software_installation_stage_title));
  text.software_optimization_stage_title = localized_string(resources, L"OverviewGuidedSoftwareOptimizationStageTitle", std::move(text.software_optimization_stage_title));
  text.unknown_stage_title = localized_string(resources, L"OverviewGuidedUnknownStageTitle", std::move(text.unknown_stage_title));
  text.stage_pending_body = localized_string(resources, L"OverviewGuidedStagePendingBody", std::move(text.stage_pending_body));
  text.stage_active_body = localized_string(resources, L"OverviewGuidedStageActiveBody", std::move(text.stage_active_body));
  text.stage_completed_body = localized_string(resources, L"OverviewGuidedStageCompletedBody", std::move(text.stage_completed_body));
  text.stage_skipped_body = localized_string(resources, L"OverviewGuidedStageSkippedBody", std::move(text.stage_skipped_body));
  text.stage_no_applicable_body = localized_string(resources, L"OverviewGuidedStageNoApplicableBody", std::move(text.stage_no_applicable_body));
  text.stage_partial_body = localized_string(resources, L"OverviewGuidedStagePartialBody", std::move(text.stage_partial_body));
  text.stage_failed_body = localized_string(resources, L"OverviewGuidedStageFailedBody", std::move(text.stage_failed_body));
  text.stage_confirmation_body = localized_string(resources, L"OverviewGuidedStageConfirmationBody", std::move(text.stage_confirmation_body));
  text.stage_waiting_explorer_body = localized_string(resources, L"OverviewGuidedStageWaitingExplorerBody", std::move(text.stage_waiting_explorer_body));
  text.stage_waiting_restart_body = localized_string(resources, L"OverviewGuidedStageWaitingRestartBody", std::move(text.stage_waiting_restart_body));
  text.stage_withdrawn_body = localized_string(resources, L"OverviewGuidedStageWithdrawnBody", std::move(text.stage_withdrawn_body));
  text.stage_external_handoff_body = localized_string(resources, L"OverviewGuidedStageExternalHandoffBody", std::move(text.stage_external_handoff_body));
  text.stage_not_executed_body = localized_string(resources, L"OverviewGuidedStageNotExecutedBody", std::move(text.stage_not_executed_body));
  return text;
}

[[nodiscard]] std::optional<PageId> page_for_stage(
    azzs::application::guided_initialization::Stage stage) noexcept {
  using Stage = azzs::application::guided_initialization::Stage;
  switch (stage) {
    case Stage::drivers:
      return PageId::drivers;
    case Stage::system_optimization:
      return PageId::system_optimization;
    case Stage::software_installation:
      return PageId::software_installation;
    case Stage::software_optimization:
      return PageId::software_optimization;
  }
  return std::nullopt;
}

}  // namespace

OverviewPage::OverviewPage() {
  InitializeComponent();
}

void OverviewPage::bind(
    std::shared_ptr<azzs::application::WorkbenchServices> services,
    bool advanced_view,
    std::function<void(azzs::application::PageId)> navigate) {
  services_ = std::move(services);
  advanced_view_ = advanced_view;
  navigate_ = std::move(navigate);
  project();
}

void OverviewPage::project() {
  if (!services_) {
    return;
  }
  static_cast<void>(services_->guided_initialization().refresh());
  auto const snapshot = services_->guided_initialization().snapshot();
  presentation_ = azzs::ui::presentation::make_guided_initialization_presentation(
      snapshot, load_presentation_text());
  auto const mode = advanced_view_ ? azzs::ui::presentation::ViewMode::advanced
                                   : azzs::ui::presentation::ViewMode::standard;
  auto weak_this = get_weak();
  SurfaceImplementation::IntentHandler intent_handler =
      [weak_this](auto const& intent) {
        if (auto self = weak_this.get()) {
          self->handle_intent(intent);
        }
      };
  winrt::get_self<SurfaceImplementation>(SummarySurface())->project(
      presentation_, "guided.summary", mode, intent_handler, 10, "Summary");
  winrt::get_self<SurfaceImplementation>(LocalTrialSurface())->project(
      presentation_, "guided.local-trial", mode, intent_handler, 20,
      "LocalTrial");
  winrt::get_self<SurfaceImplementation>(ErrorSurface())->project(
      presentation_, "guided.error", mode, intent_handler, 30, "Error");
  winrt::get_self<SurfaceImplementation>(DriversStageSurface())->project(
      presentation_, "guided.stage.drivers", mode, intent_handler, 40,
      "Drivers");
  winrt::get_self<SurfaceImplementation>(SystemOptimizationStageSurface())
      ->project(
      presentation_, "guided.stage.system-optimization", mode, intent_handler,
      50, "SystemOptimization");
  winrt::get_self<SurfaceImplementation>(SoftwareInstallationStageSurface())
      ->project(
      presentation_, "guided.stage.software-installation", mode,
      intent_handler, 60, "SoftwareInstallation");
  winrt::get_self<SurfaceImplementation>(SoftwareOptimizationStageSurface())
      ->project(
      presentation_, "guided.stage.software-optimization", mode,
      intent_handler, 70, "SoftwareOptimization");
}

void OverviewPage::handle_intent(
    azzs::ui::presentation::PresentationIntent const& intent) {
  if (!services_ || !presentation_) {
    return;
  }
  auto& service = services_->guided_initialization();
  namespace guided = azzs::application::guided_initialization;

  if (intent.kind == IntentKind::open_details) {
    if (intent.target_id == "guided.summary" &&
        intent.command_id == "history") {
      if (navigate_) {
        navigate_(PageId::history_and_logs);
      }
      return;
    }
    auto const* component = presentation_->find_component(intent.target_id);
    if (component && component->stage.has_value()) {
      if (auto const page = page_for_stage(
              static_cast<guided::Stage>(*component->stage));
          page.has_value() && navigate_) {
        navigate_(*page);
      }
    }
    return;
  }

  if (intent.target_id.starts_with("guided.handoff.")) {
    auto const software_id = intent.target_id.substr(
        std::string_view{"guided.handoff."}.size());
    static_cast<void>(service.continue_external_handoff(software_id));
    project();
    return;
  }

  if (intent.target_id == "guided.summary") {
    if (intent.command_id == "start") {
      static_cast<void>(service.start());
    } else if (intent.command_id == "refresh") {
      static_cast<void>(service.refresh());
    } else if (intent.command_id == "cancel") {
      static_cast<void>(service.cancel());
    }
    project();
    return;
  }

  auto const* component = presentation_->find_component(intent.target_id);
  auto const current_snapshot = service.snapshot();
  if (!component || !component->stage.has_value() ||
      !current_snapshot.active.has_value()) {
    return;
  }
  auto const stage = static_cast<guided::Stage>(*component->stage);
  if (stage != current_snapshot.active->current_stage) {
    return;
  }
  if (intent.command_id == "skip") {
    static_cast<void>(service.skip_current_stage());
  } else if (intent.command_id == "retry") {
    auto const retry = service.retry_current_stage();
    if (retry.succeeded()) {
      if (auto const page = page_for_stage(stage);
          page.has_value() && navigate_) {
        navigate_(*page);
        return;
      }
    }
  } else if (intent.command_id == "continue") {
    if (current_snapshot.active->state ==
        guided::FlowState::awaiting_restart_continue) {
      static_cast<void>(service.continue_after_restart());
    } else if (stage == guided::Stage::drivers &&
               current_snapshot.active->stages[static_cast<std::size_t>(stage)]
                       .state ==
                   guided::StageState::result_confirmation_pending) {
      static_cast<void>(service.mark_driver_completed());
    } else {
      static_cast<void>(service.continue_current_stage());
    }
  }
  project();
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
