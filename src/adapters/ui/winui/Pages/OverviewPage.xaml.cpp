#include "pch.h"

#include "OverviewPage.xaml.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

#include "../DesignSystem/Controls/ReadOnlyPresentationSurface.xaml.h"
#include "../DesignSystem/guided_initialization_presentation.hpp"
#include "../native_resource_fallback.hpp"
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

using PresentationText =
    azzs::ui::presentation::GuidedInitializationPresentationText;
using PresentationMember = std::string PresentationText::*;

struct PresentationResource final {
  wchar_t const* key;
  PresentationMember member;
};

constexpr std::array kPresentationResources{
    PresentationResource{L"OverviewGuidedSummaryAccessibleName",
                          &PresentationText::summary_accessible_name},
    PresentationResource{L"OverviewGuidedSummaryTitle",
                         &PresentationText::summary_title},
    PresentationResource{L"OverviewGuidedSummaryCompletedPrefix",
                         &PresentationText::summary_prefix},
    PresentationResource{L"OverviewGuidedSummaryExternalPrefix",
                         &PresentationText::summary_external_prefix},
    PresentationResource{L"OverviewGuidedSummaryPartialPrefix",
                         &PresentationText::summary_partial_prefix},
    PresentationResource{L"OverviewGuidedSummaryFailedPrefix",
                         &PresentationText::summary_failed_prefix},
    PresentationResource{L"OverviewGuidedSummarySkippedPrefix",
                         &PresentationText::summary_skipped_prefix},
    PresentationResource{L"OverviewGuidedSummaryNoApplicablePrefix",
                         &PresentationText::summary_no_applicable_prefix},
    PresentationResource{L"OverviewGuidedSummaryNotExecutedPrefix",
                         &PresentationText::summary_not_executed_prefix},
    PresentationResource{L"OverviewGuidedSummaryConfirmationPrefix",
                         &PresentationText::summary_confirmation_prefix},
    PresentationResource{L"OverviewGuidedSummaryExplorerRestartPrefix",
                         &PresentationText::summary_explorer_restart_prefix},
    PresentationResource{L"OverviewGuidedSummaryRestartPrefix",
                         &PresentationText::summary_restart_prefix},
    PresentationResource{L"OverviewGuidedSummaryWithdrawnPrefix",
                         &PresentationText::summary_withdrawn_prefix},
    PresentationResource{L"OverviewGuidedSummaryErrorSuffix",
                         &PresentationText::summary_error_suffix},
    PresentationResource{L"OverviewGuidedStartCommand",
                         &PresentationText::start_command},
    PresentationResource{L"OverviewGuidedRefreshCommand",
                         &PresentationText::refresh_command},
    PresentationResource{L"OverviewGuidedCancelCommand",
                         &PresentationText::cancel_command},
    PresentationResource{L"OverviewGuidedHistoryCommand",
                         &PresentationText::history_command},
    PresentationResource{L"OverviewGuidedSkipCommand",
                         &PresentationText::skip_command},
    PresentationResource{L"OverviewGuidedContinueCommand",
                         &PresentationText::continue_command},
    PresentationResource{L"OverviewGuidedRetryCommand",
                         &PresentationText::retry_command},
    PresentationResource{L"OverviewGuidedOpenCommand",
                         &PresentationText::open_command},
    PresentationResource{L"OverviewGuidedLocalTrialAccessibleName",
                         &PresentationText::local_trial_accessible_name},
    PresentationResource{L"OverviewGuidedLocalTrialTitle",
                         &PresentationText::local_trial_title},
    PresentationResource{L"OverviewGuidedLocalTrialBody",
                         &PresentationText::local_trial_body},
    PresentationResource{L"OverviewGuidedHandoffAccessibleName",
                         &PresentationText::handoff_accessible_name},
    PresentationResource{L"OverviewGuidedHandoffTitle",
                         &PresentationText::handoff_title},
    PresentationResource{L"OverviewGuidedHandoffWaitingBody",
                         &PresentationText::handoff_waiting_body},
    PresentationResource{L"OverviewGuidedHandoffRecognizedBody",
                         &PresentationText::handoff_recognized_body},
    PresentationResource{L"OverviewGuidedHandoffContinueCommand",
                         &PresentationText::handoff_continue_command},
    PresentationResource{L"OverviewGuidedReadOnlyAccessibleName",
                         &PresentationText::read_only_accessible_name},
    PresentationResource{L"OverviewGuidedReadOnlyTitle",
                         &PresentationText::read_only_title},
    PresentationResource{L"OverviewGuidedReadOnlyBody",
                         &PresentationText::read_only_body},
    PresentationResource{L"OverviewGuidedReadOnlyDisabledReason",
                         &PresentationText::read_only_disabled_reason},
    PresentationResource{L"OverviewGuidedStageEmptyBody",
                         &PresentationText::stage_empty_body},
    PresentationResource{L"OverviewGuidedRawDetailPrefix",
                         &PresentationText::raw_detail_prefix},
    PresentationResource{L"OverviewGuidedRawErrorPrefix",
                         &PresentationText::raw_error_prefix},
    PresentationResource{L"OverviewGuidedDriversStageTitle",
                         &PresentationText::drivers_stage_title},
    PresentationResource{L"OverviewGuidedSystemOptimizationStageTitle",
                         &PresentationText::system_optimization_stage_title},
    PresentationResource{L"OverviewGuidedSoftwareInstallationStageTitle",
                         &PresentationText::software_installation_stage_title},
    PresentationResource{L"OverviewGuidedSoftwareOptimizationStageTitle",
                         &PresentationText::software_optimization_stage_title},
    PresentationResource{L"OverviewGuidedUnknownStageTitle",
                         &PresentationText::unknown_stage_title},
    PresentationResource{L"OverviewGuidedStagePendingBody",
                         &PresentationText::stage_pending_body},
    PresentationResource{L"OverviewGuidedStageActiveBody",
                         &PresentationText::stage_active_body},
    PresentationResource{L"OverviewGuidedStageCompletedBody",
                         &PresentationText::stage_completed_body},
    PresentationResource{L"OverviewGuidedStageSkippedBody",
                         &PresentationText::stage_skipped_body},
    PresentationResource{L"OverviewGuidedStageNoApplicableBody",
                         &PresentationText::stage_no_applicable_body},
    PresentationResource{L"OverviewGuidedStagePartialBody",
                         &PresentationText::stage_partial_body},
    PresentationResource{L"OverviewGuidedStageFailedBody",
                         &PresentationText::stage_failed_body},
    PresentationResource{L"OverviewGuidedStageConfirmationBody",
                         &PresentationText::stage_confirmation_body},
    PresentationResource{L"OverviewGuidedStageWaitingExplorerBody",
                         &PresentationText::stage_waiting_explorer_body},
    PresentationResource{L"OverviewGuidedStageWaitingRestartBody",
                         &PresentationText::stage_waiting_restart_body},
    PresentationResource{L"OverviewGuidedStageWithdrawnBody",
                         &PresentationText::stage_withdrawn_body},
    PresentationResource{L"OverviewGuidedStageExternalHandoffBody",
                         &PresentationText::stage_external_handoff_body},
    PresentationResource{L"OverviewGuidedStageNotExecutedBody",
                         &PresentationText::stage_not_executed_body},
};

// Keep the native pipe resource and the resource-loader table in lockstep.
static_assert(kPresentationResources.size() == 55);

[[nodiscard]] std::string localized_string(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const& resources,
    wchar_t const* key, std::string const& fallback) {
  try {
    auto const value = resources.GetString(key);
    return value.empty() ? fallback : winrt::to_string(value);
  } catch (...) {
    // Resource projection is optional at this boundary; keep the native
    // presentation fallback when a catalog lookup fails.
    return fallback;
  }
}

[[nodiscard]] bool load_native_presentation_text(PresentationText& text) {
  try {
    auto const packed = azzs::ui::winui::native_resources::load_string(
        AZZS_NATIVE_STRING_OVERVIEW_GUIDED_PRESENTATION);
    if (packed.empty()) {
      return false;
    }

    auto candidate = text;
    auto const packed_view = std::wstring_view{packed};
    std::size_t offset = 0;
    for (std::size_t index = 0; index < kPresentationResources.size(); ++index) {
      auto const delimiter = packed_view.find(L'|', offset);
      auto const end = delimiter == std::wstring_view::npos
                           ? packed_view.size()
                           : delimiter;
      if (end <= offset) {
        return false;
      }
      if (delimiter == std::wstring_view::npos &&
          index + 1 != kPresentationResources.size()) {
        // A packed fallback is valid only when every field is present in the
        // frozen order; do not silently accept a truncated resource string.
        return false;
      }
      if (delimiter != std::wstring_view::npos &&
          index + 1 == kPresentationResources.size()) {
        // The final field must terminate at the end of the resource string;
        // a trailing separator would encode an additional empty field.
        return false;
      }
      candidate.*kPresentationResources[index].member = winrt::to_string(
          winrt::hstring{packed_view.substr(offset, end - offset)});
      if (delimiter == std::wstring_view::npos) {
        offset = packed_view.size();
        break;
      }
      offset = delimiter + 1;
    }
    if (offset != packed_view.size()) {
      return false;
    }
    text = std::move(candidate);
    return true;
  } catch (...) {
    // A malformed or unavailable native fallback must leave the page alive;
    // the presentation's technical defaults remain available below.
    return false;
  }
}

[[nodiscard]] azzs::ui::presentation::GuidedInitializationPresentationText
load_presentation_text() {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  azzs::ui::presentation::GuidedInitializationPresentationText text;
  static_cast<void>(load_native_presentation_text(text));
  try {
    auto const resources = ResourceLoader{};
    for (auto const& resource : kPresentationResources) {
      text.*resource.member = localized_string(
          resources, resource.key, text.*resource.member);
    }
  } catch (...) {
    // Keep the native fallback when the packaged catalog is unavailable.
  }
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
