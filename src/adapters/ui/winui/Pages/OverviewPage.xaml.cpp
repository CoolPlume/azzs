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
      snapshot);
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
