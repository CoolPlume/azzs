#include "pch.h"

#include "DesignSystemFixturePage.xaml.h"

#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.xaml.h"

#if __has_include("DesignSystem/Fixtures/DesignSystemFixturePage.g.cpp")
#include "DesignSystem/Fixtures/DesignSystemFixturePage.g.cpp"
#endif

namespace {

using azzs::ui::presentation::ComponentProjection;
using azzs::ui::presentation::PresentationSnapshot;
using azzs::ui::presentation::ViewMode;
using ProjectedSurface =
    winrt::Azzs::Ui::DesignSystem::Controls::ReadOnlyPresentationSurface;
using SurfaceImplementation = winrt::Azzs::Ui::DesignSystem::Controls::
    implementation::ReadOnlyPresentationSurface;
using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
using winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationLiveSetting;
using winrt::Microsoft::UI::Xaml::Controls::Border;
using winrt::Microsoft::UI::Xaml::Controls::TextBlock;

[[nodiscard]] ComponentProjection const& required_component(
    PresentationSnapshot const& snapshot,
    std::string_view component_id) {
  auto const* component = snapshot.find_component(component_id);
  if (component == nullptr) {
    throw std::logic_error{"design-system fixture is incomplete"};
  }
  return *component;
}

void project_surface(
    ProjectedSurface const& target,
    std::shared_ptr<PresentationSnapshot const> const& source,
    std::string_view component_id,
    ViewMode mode,
    SurfaceImplementation::IntentHandler const& intent_handler,
    std::int32_t tab_index_start,
    std::string automation_suffix = {}) {
  winrt::get_self<SurfaceImplementation>(target)->project(
      source, component_id, mode, intent_handler, tab_index_start,
      std::move(automation_suffix));
}

void project_stage(Border const& surface,
                   TextBlock const& text,
                   ComponentProjection const& component,
                   std::size_t order) {
  AutomationProperties::SetAutomationId(
      surface, winrt::to_hstring(component.automation_id));
  AutomationProperties::SetName(
      surface, winrt::to_hstring(component.accessible_name));
  text.Text(winrt::to_hstring(std::to_string(order) + "  " + component.title));
}

}  // namespace

namespace winrt::Azzs::Ui::DesignSystem::Fixtures::implementation {

DesignSystemFixturePage::DesignSystemFixturePage() {
  InitializeComponent();
  project_fixture(azzs::ui::presentation::make_design_system_fixture());
}

void DesignSystemFixturePage::project_fixture(
    std::shared_ptr<azzs::ui::presentation::PresentationSnapshot const> const&
        snapshot) {
  auto weak_this = get_weak();
  SurfaceImplementation::IntentHandler const intent_handler =
      [weak_this](azzs::ui::presentation::PresentationIntent const& intent) {
        if (auto self = weak_this.get()) {
          self->project_intent(intent);
        }
      };

  project_surface(SummarySurface(), snapshot, "fixture.summary",
                  ViewMode::standard, intent_handler, 11, "Summary");
  project_surface(LongChineseSurface(), snapshot, "fixture.long-chinese",
                  ViewMode::standard, intent_handler, 20, "List");
  project_surface(StandardSharedSurface(), snapshot, "fixture.shared-view",
                  ViewMode::standard, intent_handler, 30, "Standard");
  project_surface(AdvancedSharedSurface(), snapshot, "fixture.shared-view",
                  ViewMode::advanced, intent_handler, 40, "Advanced");

  struct StatusTarget final {
    ProjectedSurface surface;
    char const* component_id;
  };
  std::array<StatusTarget, 10> const status_targets{
      StatusTarget{LocalTrialStatus(), "fixture.local-trial"},
      StatusTarget{RecoveredUnsavedStatus(), "fixture.recovered-unsaved"},
      StatusTarget{SavedNotAppliedStatus(), "fixture.saved-not-applied"},
      StatusTarget{SourceHandoffStatus(), "fixture.source-handoff"},
      StatusTarget{WaitingNetworkStatus(), "fixture.waiting-network"},
      StatusTarget{InlineErrorStatus(), "fixture.inline-error"},
      StatusTarget{WaitingRestartStatus(), "fixture.waiting-restart"},
      StatusTarget{FailureStatus(), "fixture.failure"},
      StatusTarget{PendingConfirmationStatus(),
                   "fixture.pending-confirmation"},
      StatusTarget{EmergencyWithdrawalStatus(),
                   "fixture.emergency-withdrawal"},
  };
  auto status_tab_index = 60;
  for (auto const& target : status_targets) {
    project_surface(target.surface, snapshot, target.component_id,
                    ViewMode::standard, intent_handler, status_tab_index,
                    "Status");
    status_tab_index += 10;
  }

  project_surface(DisabledReasonSurface(), snapshot,
                  "fixture.disabled-reason", ViewMode::standard,
                  intent_handler, 170, "DisabledReason");
  project_surface(RiskConfirmationSurface(), snapshot,
                  "fixture.risk-confirmation", ViewMode::standard,
                  intent_handler, 180, "RiskConfirmation");
  project_surface(ResultLocatorSurface(), snapshot, "fixture.result-locator",
                  ViewMode::standard, intent_handler, 190,
                  "ResultLocator");

  struct StageTarget final {
    Border surface;
    TextBlock text;
    char const* component_id;
  };
  std::array<StageTarget, 4> const stage_targets{
      StageTarget{StageDriversSurface(), StageDriversText(),
                  "fixture.stage.drivers"},
      StageTarget{StageSystemOptimizationSurface(),
                  StageSystemOptimizationText(),
                  "fixture.stage.system-optimization"},
      StageTarget{StageSoftwareInstallationSurface(),
                  StageSoftwareInstallationText(),
                  "fixture.stage.software-installation"},
      StageTarget{StageSoftwareOptimizationSurface(),
                  StageSoftwareOptimizationText(),
                  "fixture.stage.software-optimization"},
  };
  for (std::size_t index = 0; index < stage_targets.size(); ++index) {
    auto const& target = stage_targets[index];
    project_stage(target.surface, target.text,
                  required_component(*snapshot, target.component_id), index + 1);
  }

  auto const& progress =
      required_component(*snapshot, "fixture.unknown-progress");
  AutomationProperties::SetAutomationId(
      UnknownProgressSurface(), winrt::to_hstring(progress.automation_id));
  AutomationProperties::SetName(
      UnknownProgressSurface(), winrt::to_hstring(progress.accessible_name));
  AutomationProperties::SetLiveSetting(
      UnknownProgressSurface(), AutomationLiveSetting::Polite);
  UnknownProgressTitle().Text(winrt::to_hstring(progress.title));
  UnknownProgressValue().Text(winrt::to_hstring(progress.body));
  if (progress.progress.has_value()) {
    AutomationProperties::SetAutomationId(
        UnknownProgressBar(),
        winrt::to_hstring(progress.automation_id + ".ProgressBar"));
    AutomationProperties::SetName(
        UnknownProgressBar(),
        winrt::to_hstring(progress.progress->accessible_value));
  }

  auto const& settings =
      required_component(*snapshot, "fixture.settings-form");
  AutomationProperties::SetAutomationId(
      SettingsFormGrid(), winrt::to_hstring(settings.automation_id));
  AutomationProperties::SetName(
      SettingsFormGrid(), winrt::to_hstring(settings.accessible_name));
  SettingsFieldLabel().Text(winrt::to_hstring(settings.title));
  FieldHelpText().Text(winrt::to_hstring(settings.body));

  auto const& catalog =
      required_component(*snapshot, "fixture.catalog-editor");
  AutomationProperties::SetAutomationId(
      CatalogEditorSurface(), winrt::to_hstring(catalog.automation_id));
  AutomationProperties::SetName(
      CatalogEditorSurface(), winrt::to_hstring(catalog.accessible_name));
  CatalogEditorText().Text(winrt::to_hstring(catalog.body));
}

void DesignSystemFixturePage::project_intent(
    azzs::ui::presentation::PresentationIntent const& intent) {
  std::wstring message{L"已发出类型化意图："};
  auto const command_id = winrt::to_hstring(intent.command_id);
  message.append(command_id.c_str(), command_id.size());
  message.append(L" -> ");
  auto const target_id = winrt::to_hstring(intent.target_id);
  message.append(target_id.c_str(), target_id.size());
  LastIntentText().Text(winrt::hstring{message});
}

}  // namespace winrt::Azzs::Ui::DesignSystem::Fixtures::implementation
