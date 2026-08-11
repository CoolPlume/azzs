#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include "motion_contract.hpp"
#include "presentation_contract.hpp"
#include "Fixtures/design_system_fixture.hpp"

namespace {

using namespace std::chrono_literals;
using azzs::ui::presentation::CommandRole;
using azzs::ui::presentation::ComponentKind;
using azzs::ui::presentation::ComponentProjection;
using azzs::ui::presentation::InputModality;
using azzs::ui::presentation::IntentKind;
using azzs::ui::presentation::MotionSemantic;
using azzs::ui::presentation::ProgressKind;
using azzs::ui::presentation::PresentationState;
using azzs::ui::presentation::PresentationSnapshot;
using azzs::ui::presentation::ReadOnlyPresentation;
using azzs::ui::presentation::RiskLevel;
using azzs::ui::presentation::ViewMode;
using azzs::ui::presentation::WorkflowStage;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "UI presentation contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool verify_motion_contract() {
  bool passed = true;
  constexpr std::array allowed{0ms, 83ms, 167ms, 250ms};
  constexpr std::array semantics{
      MotionSemantic::immediate,
      MotionSemantic::keyboard_command,
      MotionSemantic::top_level_navigation,
      MotionSemantic::view_mode_switch,
      MotionSemantic::continuous_selection,
      MotionSemantic::progress_value_refresh,
      MotionSemantic::log_refresh,
      MotionSemantic::pointer_feedback,
      MotionSemantic::state_change,
      MotionSemantic::overlay_enter,
      MotionSemantic::overlay_exit,
  };
  constexpr std::array inputs{
      InputModality::mouse,
      InputModality::touch,
      InputModality::pen,
      InputModality::keyboard,
      InputModality::screen_reader,
  };

  for (auto const semantic : semantics) {
    for (auto const input : inputs) {
      auto const enabled =
          azzs::ui::presentation::motion_duration_for(semantic, input, true);
      passed &= expect(std::ranges::find(allowed, enabled) != allowed.end(),
                       "motion must use only 0/83/167/250ms");
      if (input == InputModality::keyboard ||
          input == InputModality::screen_reader ||
          semantic == MotionSemantic::immediate ||
          semantic == MotionSemantic::keyboard_command ||
          semantic == MotionSemantic::top_level_navigation ||
          semantic == MotionSemantic::view_mode_switch ||
          semantic == MotionSemantic::continuous_selection ||
          semantic == MotionSemantic::progress_value_refresh ||
          semantic == MotionSemantic::log_refresh) {
        passed &= expect(enabled == 0ms,
                         "keyboard, assistive, and immediate paths must be 0ms");
      }
      passed &= expect(
          azzs::ui::presentation::motion_duration_for(semantic, input, false) ==
              0ms,
          "the Windows animations switch must force every custom path to 0ms");
    }
  }

  passed &= expect(
      azzs::ui::presentation::motion_duration_for(
          MotionSemantic::pointer_feedback, InputModality::mouse, true) ==
          83ms,
      "pointer feedback must use the 83ms semantic duration");
  passed &= expect(
      azzs::ui::presentation::motion_duration_for(
          MotionSemantic::state_change, InputModality::touch, true) == 167ms,
      "small state changes must use the 167ms semantic duration");
  passed &= expect(
      azzs::ui::presentation::motion_duration_for(
          MotionSemantic::overlay_enter, InputModality::mouse, true) == 250ms,
      "occasional overlay entrance must not exceed 250ms");
  passed &= expect(
      azzs::ui::presentation::motion_duration_for(
          MotionSemantic::overlay_exit, InputModality::mouse, true) == 167ms,
      "overlay exit must be shorter than entrance");
  constexpr std::array immediate_semantics{
      MotionSemantic::keyboard_command,
      MotionSemantic::top_level_navigation,
      MotionSemantic::view_mode_switch,
      MotionSemantic::continuous_selection,
      MotionSemantic::progress_value_refresh,
      MotionSemantic::log_refresh,
  };
  for (auto const semantic : immediate_semantics) {
    passed &= expect(
        azzs::ui::presentation::motion_duration_for(
            semantic, InputModality::mouse, true) == 0ms,
        "high-frequency navigation, selection, progress, and logs must be 0ms");
  }
  return passed;
}

[[nodiscard]] bool verify_fixture_contract() {
  bool passed = true;
  auto const fixture = azzs::ui::presentation::make_design_system_fixture();
  ReadOnlyPresentation const standard{fixture, ViewMode::standard};
  ReadOnlyPresentation const advanced{fixture, ViewMode::advanced};

  bool rejected_duplicate_identity = false;
  try {
    std::vector<ComponentProjection> invalid{
        ComponentProjection{
            .id = "duplicate",
            .automation_id = "DuplicateAutomationId",
            .accessible_name = "重复一",
        },
        ComponentProjection{
            .id = "duplicate",
            .automation_id = "DuplicateAutomationId",
            .accessible_name = "重复二",
        },
    };
    static_cast<void>(PresentationSnapshot{std::move(invalid)});
  } catch (std::invalid_argument const&) {
    rejected_duplicate_identity = true;
  }
  passed &= expect(rejected_duplicate_identity,
                   "the immutable snapshot must reject duplicate identity");

  bool rejected_unknown_numeric_value = false;
  try {
    std::vector<ComponentProjection> invalid{
        ComponentProjection{
            .id = "invalid-progress",
            .automation_id = "InvalidProgress",
            .accessible_name = "Invalid progress",
            .kind = ComponentKind::progress,
            .progress = azzs::ui::presentation::ProgressProjection{
                .kind = ProgressKind::unknown,
                .completed = 1,
                .total = std::nullopt,
                .accessible_value = "Unknown progress",
            },
        },
    };
    static_cast<void>(PresentationSnapshot{std::move(invalid)});
  } catch (std::invalid_argument const&) {
    rejected_unknown_numeric_value = true;
  }
  passed &= expect(
      rejected_unknown_numeric_value,
      "unknown progress must reject fabricated numeric values");

  passed &= expect(std::addressof(standard.source()) ==
                       std::addressof(advanced.source()),
                   "standard and advanced views must share one source snapshot");

  std::set<std::string> ids;
  std::set<std::string> automation_ids;
  std::set<ComponentKind> kinds;
  std::array<std::optional<WorkflowStage>, 4> stages;
  std::size_t stage_index = 0;

  for (auto const& component : fixture->components()) {
    passed &= expect(!component.id.empty(), "every component needs a stable id");
    passed &= expect(!component.automation_id.empty(),
                     "every component needs a stable AutomationId");
    passed &= expect(!component.accessible_name.empty(),
                     "every component needs an accessible name");
    passed &= expect(ids.insert(component.id).second,
                     "component ids must be unique");
    passed &= expect(automation_ids.insert(component.automation_id).second,
                     "component AutomationIds must be unique");
    kinds.insert(component.kind);

    if (component.advanced_only) {
      passed &= expect(component.risk == RiskLevel::none ||
                           component.risk == RiskLevel::low,
                       "critical state cannot be hidden in advanced view");
      passed &= expect(component.state != PresentationState::failed &&
                           component.state !=
                               PresentationState::pending_confirmation &&
                           component.state != PresentationState::withdrawn,
                       "blocking states must remain visible in standard view");
    }

    if (component.stage.has_value()) {
      if (stage_index < stages.size()) {
        stages[stage_index] = component.stage;
      }
      ++stage_index;
    }

    std::set<std::string> command_ids;
    std::size_t default_focus_count = 0;
    for (auto const& command : component.commands) {
      passed &= expect(!command.id.empty() && !command.label.empty(),
                       "commands must remain discoverable by id and label");
      passed &= expect(command_ids.insert(command.id).second,
                       "command ids must be unique within a component");
      passed &= expect(command.intent.target_id == component.id,
                       "a typed intent must retain its source target");
      passed &= expect(command.intent.command_id == command.id,
                       "a typed intent must retain its command id");
      if (!command.enabled) {
        passed &= expect(!command.disabled_reason.empty(),
                         "disabled commands must include a complete reason");
      }
      if (command.role == CommandRole::danger) {
        passed &= expect(!command.default_focus,
                         "danger commands must never receive default focus");
      }
      if (command.default_focus) {
        ++default_focus_count;
        passed &= expect(command.enabled,
                         "a disabled command cannot receive default focus");
      }
    }
    passed &= expect(default_focus_count <= 1,
                     "a component can expose at most one default command");
  }

  constexpr std::array expected_kinds{
      ComponentKind::navigation,
      ComponentKind::group,
      ComponentKind::list,
      ComponentKind::detail,
      ComponentKind::summary,
      ComponentKind::status_band,
      ComponentKind::inline_error,
      ComponentKind::disabled_reason,
      ComponentKind::risk_confirmation,
      ComponentKind::progress,
      ComponentKind::waiting,
      ComponentKind::failure,
      ComponentKind::pending_confirmation,
      ComponentKind::emergency_withdrawal,
      ComponentKind::result_locator,
      ComponentKind::stage_summary,
      ComponentKind::settings_form,
      ComponentKind::catalog_editor,
  };
  for (auto const kind : expected_kinds) {
    passed &= expect(kinds.contains(kind),
                     "the fixture must cover every reusable component kind");
  }

  constexpr std::array expected_stages{
      WorkflowStage::drivers,
      WorkflowStage::system_optimization,
      WorkflowStage::software_installation,
      WorkflowStage::software_optimization,
  };
  passed &= expect(stage_index == expected_stages.size(),
                   "the fixture must include exactly four workflow stages");
  for (std::size_t index = 0; index < expected_stages.size(); ++index) {
    passed &= expect(stages[index] == expected_stages[index],
                     "workflow stages must remain in the frozen order");
  }

  auto const* long_chinese =
      fixture->find_component("fixture.long-chinese");
  passed &= expect(long_chinese != nullptr && long_chinese->body.size() > 180,
                   "the fixture must retain a long Chinese wrapping case");

  auto const* unknown_progress =
      fixture->find_component("fixture.unknown-progress");
  passed &= expect(
      unknown_progress != nullptr && unknown_progress->progress.has_value() &&
          unknown_progress->progress->kind == ProgressKind::unknown &&
          !unknown_progress->progress->completed.has_value() &&
          !unknown_progress->progress->total.has_value() &&
          !unknown_progress->progress->accessible_value.empty() &&
          unknown_progress->body.find("正在计算") != std::string::npos,
      "unknown progress must not invent a total or percentage");

  auto const* determinate_progress =
      fixture->find_component("fixture.determinate-progress");
  passed &= expect(
      determinate_progress != nullptr &&
          determinate_progress->progress.has_value() &&
          determinate_progress->progress->kind == ProgressKind::determinate &&
          determinate_progress->progress->completed == 37 &&
          determinate_progress->progress->total == 100 &&
          !determinate_progress->progress->accessible_value.empty(),
      "determinate progress must retain its real range and accessible text");

  constexpr std::array required_fixture_ids{
      "fixture.inline-error",
      "fixture.waiting-restart",
      "fixture.emergency-withdrawal",
      "fixture.pending-confirmation",
      "fixture.local-trial",
      "fixture.recovered-unsaved",
      "fixture.saved-not-applied",
      "fixture.source-handoff",
      "fixture.waiting-network",
      "fixture.catalog-editor",
  };
  for (auto const id : required_fixture_ids) {
    passed &= expect(fixture->find_component(id) != nullptr,
                     "the fixed fixture matrix is incomplete");
  }

  auto const result_intent =
      standard.intent_for("fixture.result-locator", "locate-result");
  passed &= expect(result_intent.has_value() &&
                       result_intent->kind == IntentKind::locate_result,
                   "enabled commands must emit a typed intent");
  passed &= expect(
      !standard
           .intent_for("fixture.disabled-reason", "retry-disabled")
           .has_value(),
      "disabled commands must not emit an intent");
  auto const selection_intent =
      standard.intent_for("fixture.long-chinese", "toggle-selection");
  passed &= expect(
      selection_intent.has_value() &&
          selection_intent->kind == IntentKind::toggle_item_selection,
      "per-item selection must be expressed as a typed consumer intent");
  passed &= expect(
      !standard
           .intent_for("fixture.advanced-detail", "open-details")
           .has_value(),
      "standard view must not expose advanced-only surface intents");
  auto const advanced_intent =
      advanced.intent_for("fixture.advanced-detail", "open-details");
  passed &= expect(advanced_intent.has_value() &&
                       advanced_intent->kind == IntentKind::open_details,
                   "advanced view must expose its typed low-frequency intent");
  passed &= expect(
      !standard.intent_for("fixture.shared-view", "open-source").has_value(),
      "standard view must hide low-frequency commands from the shared source");
  auto const shared_advanced_intent =
      advanced.intent_for("fixture.shared-view", "open-source");
  passed &= expect(
      shared_advanced_intent.has_value() &&
          shared_advanced_intent->kind == IntentKind::open_source,
      "advanced view must add only the typed low-frequency projection");

  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_motion_contract();
  passed &= verify_fixture_contract();
  if (!passed) {
    return EXIT_FAILURE;
  }

  std::cout << "UI presentation contract passed\n";
  return EXIT_SUCCESS;
}
