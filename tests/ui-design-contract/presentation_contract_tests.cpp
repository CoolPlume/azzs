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
#include "guided_initialization_presentation.hpp"
#include "software_selection_presentation.hpp"
#include "Fixtures/design_system_fixture.hpp"
#include "azzs/application/advanced_view_preferences.hpp"

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
using azzs::application::offline_package_cache::CacheLocationState;
using azzs::application::offline_package_cache::OfflinePackageCacheSnapshot;
using azzs::domain::offline_package_cache::CacheLocationKind;

class InMemoryAdvancedViewPreferenceStore final
    : public azzs::application::AdvancedViewPreferenceStore {
 public:
  [[nodiscard]] azzs::application::AdvancedViewPreferenceRead
  read_advanced_view() override {
    return read;
  }

  [[nodiscard]] azzs::application::AdvancedViewPreferenceWriteStatus
  write_advanced_view(bool enabled) override {
    last_written = enabled;
    return write_status;
  }

  azzs::application::AdvancedViewPreferenceRead read{
      .status = azzs::application::AdvancedViewPreferenceReadStatus::loaded};
  azzs::application::AdvancedViewPreferenceWriteStatus write_status{
      azzs::application::AdvancedViewPreferenceWriteStatus::saved};
  bool last_written{false};
};

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

  auto const* force_attempt =
      fixture->find_component("fixture.system-settings-force-attempt");
  passed &= expect(
      force_attempt != nullptr && standard.visible(*force_attempt) &&
          advanced.visible(*force_attempt) &&
          force_attempt->state == PresentationState::pending_confirmation &&
          force_attempt->risk == RiskLevel::high,
      "out-of-range system-setting risk must remain visible in both views");
  passed &= expect(
      !standard
           .intent_for("fixture.system-settings-force-attempt", "force-attempt")
           .has_value(),
      "standard view must not emit a force-attempt intent");
  auto const force_attempt_intent = advanced.intent_for(
      "fixture.system-settings-force-attempt", "force-attempt");
  passed &= expect(
      force_attempt_intent.has_value() &&
          force_attempt_intent->kind == IntentKind::confirm_risk,
      "advanced view must emit only the typed confirmed force-attempt intent");

  return passed;
}

[[nodiscard]] bool verify_offline_package_cache_projection() {
  OfflinePackageCacheSnapshot source{
      .selected_root = {.kind = CacheLocationKind::system_directory,
                        .id = "program-data"},
      .location_state = CacheLocationState::available,
      .network_available = false,
  };
  auto const projected =
      azzs::ui::presentation::make_offline_package_cache_presentation(source);
  auto const* component = projected->find_component("offline-package-cache.status");
  bool passed = true;
  passed &= expect(component != nullptr,
                   "offline cache projection must expose one stable component");
  passed &= expect(component != nullptr &&
                       component->automation_id ==
                           "AzzsOfflinePackageCacheStatus" &&
                       !component->accessible_name.empty() &&
                       component->body.find("program-data") != std::string::npos,
                   "offline cache projection must preserve typed location and accessibility");

  auto empty_text =
      azzs::ui::presentation::OfflinePackageCachePresentationText{};
  empty_text.accessible_name.clear();
  empty_text.available_title.clear();
  empty_text.unavailable_title.clear();
  empty_text.available_body_prefix.clear();
  empty_text.unavailable_body_prefix.clear();
  empty_text.item_suffix.clear();
  empty_text.network_suffix.clear();
  auto const fallback =
      azzs::ui::presentation::make_offline_package_cache_presentation(
          source, std::move(empty_text));
  auto const* fallback_component =
      fallback->find_component("offline-package-cache.status");
  passed &= expect(
      fallback_component != nullptr &&
          fallback_component->accessible_name == "离线资源缓存状态" &&
          fallback_component->title == "离线资源缓存" &&
          fallback_component->body.find("受控缓存位置： program-data") == 0 &&
          fallback_component->body.find("0 个已缓存资源") != std::string::npos &&
          fallback_component->body.find("当前无法联网") != std::string::npos,
      "empty offline cache resources must use Chinese presentation defaults");

  source.location_state = CacheLocationState::unavailable;
  source.location_detail = "removable media is unavailable";
  auto const unavailable =
      azzs::ui::presentation::make_offline_package_cache_presentation(source);
  auto const* unavailable_component =
      unavailable->find_component("offline-package-cache.status");
  passed &= expect(unavailable_component != nullptr &&
                       unavailable_component->state ==
                           PresentationState::waiting_for_network &&
                       unavailable_component->body.find("removable media") !=
                           std::string::npos,
                   "offline cache location loss must remain a visible static state");
  return passed;
}

[[nodiscard]] bool verify_software_selection_empty_catalog_projection() {
  auto const source =
      azzs::application::software_selection::SoftwareSelectionSnapshot{
          .mode = azzs::application::software_selection::
              SelectionLifecycleMode::ready,
      };
  auto const snapshot =
      azzs::ui::presentation::make_software_selection_presentation(source);
  ReadOnlyPresentation const standard{snapshot, ViewMode::standard};
  ReadOnlyPresentation const advanced{snapshot, ViewMode::advanced};
  auto const* status = snapshot->find_component("software-selection.status");

  bool passed = true;
  passed &= expect(status != nullptr &&
                       status->state == PresentationState::waiting_for_network &&
                       status->accessible_name == "软件选择状态" &&
                       status->title == "当前有效目录尚未加载" &&
                       status->body.find("尚未加载当前有效目录") !=
                           std::string::npos,
                   "software installation fallback must remain Simplified Chinese");
  passed &= expect(std::addressof(standard.source()) ==
                       std::addressof(advanced.source()),
                   "software standard and advanced views must share one snapshot");
  passed &= expect(status != nullptr && standard.visible(*status) &&
                       advanced.visible(*status) &&
                       status->advanced_detail.find("当前页面不会") == 0,
                   "the absent-catalog warning must remain visible while advanced adds localized detail");

  auto available = source;
  available.has_current_catalog = true;
  available.mode = azzs::application::software_selection::SelectionLifecycleMode::ready;
  auto empty_text = azzs::ui::presentation::SoftwareSelectionPresentationText{};
  empty_text.accessible_name.clear();
  empty_text.available_title.clear();
  empty_text.available_body_prefix.clear();
  empty_text.available_body_suffix.clear();
  empty_text.absent_catalog_title.clear();
  empty_text.absent_catalog_body.clear();
  empty_text.not_restored_body.clear();
  empty_text.restore_failed_body.clear();
  empty_text.advanced_available.clear();
  empty_text.advanced_absent_catalog.clear();
  auto const fallback_snapshot =
      azzs::ui::presentation::make_software_selection_presentation(
          available, std::move(empty_text));
  auto const* fallback_status =
      fallback_snapshot->find_component("software-selection.status");
  passed &= expect(fallback_status != nullptr &&
                       fallback_status->accessible_name == "软件选择状态" &&
                       fallback_status->title == "软件选择" &&
                       fallback_status->body.find("已保留 0 个软件选择") == 0 &&
                       fallback_status->advanced_detail.find("标准与高级视图") == 0,
                   "empty resource strings must use Chinese presentation defaults");

  available.mode =
      azzs::application::software_selection::SelectionLifecycleMode::failed;
  available.has_current_catalog = false;
  available.error = "HRESULT 0x80070005";
  auto const failed_snapshot =
      azzs::ui::presentation::make_software_selection_presentation(available);
  auto const* failed_status =
      failed_snapshot->find_component("software-selection.status");
  passed &= expect(failed_status != nullptr &&
                       failed_status->body == "HRESULT 0x80070005",
                   "unknown software selection errors must retain their original text");
  return passed;
}

[[nodiscard]] bool verify_guided_initialization_projection() {
  namespace guided = azzs::application::guided_initialization;

  azzs::ui::presentation::GuidedInitializationPresentationText localized_text;
  localized_text.summary_title = "推荐初始化";
  localized_text.summary_prefix = "已完成：";
  localized_text.stage_completed_body = "已完成";
  localized_text.stage_partial_body = "部分完成";
  localized_text.stage_waiting_restart_body = "等待 Windows 重启";
  localized_text.stage_withdrawn_body = "已紧急撤回";
  localized_text.raw_detail_prefix = "原始系统信息：";

  guided::Snapshot source;
  source.mode = guided::LifecycleMode::ready;
  source.writable = true;
  source.active = guided::FlowRecord{
      .id = "guided-1",
      .state = guided::FlowState::awaiting_restart_continue,
      .current_stage = guided::Stage::system_optimization,
      .stages = {{
          {.stage = guided::Stage::drivers,
           .state = guided::StageState::completed},
          {.stage = guided::Stage::system_optimization,
           .state = guided::StageState::waiting_for_restart,
           .detail = "restart verification is complete"},
          {.stage = guided::Stage::software_installation},
          {.stage = guided::Stage::software_optimization},
      }},
  };
  source.evidence.restart_gate = guided::RestartGateState::awaiting_user_continue;

  auto projected = azzs::ui::presentation::make_guided_initialization_presentation(
      source, localized_text);
  auto const* localized_summary = projected->find_component("guided.summary");
  bool has_localized_projection =
      localized_summary != nullptr &&
      localized_summary->title == "推荐初始化" &&
      localized_summary->body.find("已完成：") != std::string::npos;
  auto const* restart_stage =
      projected->find_component("guided.stage.system-optimization");
  bool passed = expect(has_localized_projection,
                       "guided projection must use injected Simplified Chinese text");
  passed &= expect(restart_stage != nullptr,
                   "guided projection must expose the current restart stage");
  bool has_restart_continue = false;
  if (restart_stage != nullptr) {
    auto const restart_continue = std::ranges::find_if(
        restart_stage->commands, [](auto const& command) {
          return command.id == "continue";
        });
    has_restart_continue =
        restart_continue != restart_stage->commands.end() &&
        restart_continue->default_focus &&
        restart_continue->intent.kind == IntentKind::continue_workflow;
  }
  passed &= expect(
      has_restart_continue,
      "restart verification must expose an explicit focused continue command");

  source.active->state = guided::FlowState::active;
  source.active->current_stage = guided::Stage::drivers;
  source.active->stages[0].state = guided::StageState::failed;
  source.evidence.restart_gate = guided::RestartGateState::none;
  projected =
      azzs::ui::presentation::make_guided_initialization_presentation(
          source, localized_text);
  auto const* failed_stage = projected->find_component("guided.stage.drivers");
  bool has_retry = false;
  if (failed_stage != nullptr) {
    auto const retry = std::ranges::find_if(
        failed_stage->commands,
        [](auto const& command) { return command.id == "retry"; });
    has_retry = retry != failed_stage->commands.end() &&
                retry->intent.kind == IntentKind::retry;
  }
  passed &= expect(has_retry, "failed guided stages must expose a typed retry intent");

  auto const* summary = projected->find_component("guided.summary");
  bool has_history = false;
  if (summary != nullptr) {
    auto const history = std::ranges::find_if(
        summary->commands,
        [](auto const& command) { return command.id == "history"; });
    has_history = history != summary->commands.end() &&
                  history->intent.kind == IntentKind::open_details;
  }
  passed &= expect(has_history,
                   "guided summary must expose a typed history and logs entry");

  source.active->stages[0].state = guided::StageState::completed;
  source.active->stages[0].detail = "driver stage marked complete by the user";
  projected =
      azzs::ui::presentation::make_guided_initialization_presentation(
          source, localized_text);
  auto const* completed_stage =
      projected->find_component("guided.stage.drivers");
  passed &= expect(completed_stage != nullptr &&
                       completed_stage->body == "已完成",
                   "known guided stage details must be localized");

  source.active->stages[0].state = guided::StageState::partial;
  source.active->stages[0].detail =
      "external installation remains an explicitly recognized fact";
  projected =
      azzs::ui::presentation::make_guided_initialization_presentation(
          source, localized_text);
  auto const* partial_stage = projected->find_component("guided.stage.drivers");
  passed &= expect(partial_stage != nullptr &&
                       partial_stage->body == "部分完成",
                   "partial guided stages must use the localized state label");

  source.active->stages[0].state = guided::StageState::waiting_for_restart;
  source.active->stages[0].detail =
      "restart barrier is available only for read-only recovery";
  projected =
      azzs::ui::presentation::make_guided_initialization_presentation(
          source, localized_text);
  auto const* read_only_restart_stage =
      projected->find_component("guided.stage.drivers");
  passed &= expect(read_only_restart_stage != nullptr &&
                       read_only_restart_stage->body == "等待 Windows 重启",
                   "known read-only restart details must be localized");

  source.active->stages[0].state = guided::StageState::emergency_withdrawn;
  source.active->stages[0].detail = "controlled emergency withdrawal";
  projected =
      azzs::ui::presentation::make_guided_initialization_presentation(
          source, localized_text);
  auto const* withdrawn_stage = projected->find_component("guided.stage.drivers");
  passed &= expect(withdrawn_stage != nullptr &&
                       withdrawn_stage->body == "已紧急撤回",
                   "withdrawn guided stages must use the localized state label");

  source.active->stages[0].state = guided::StageState::failed;
  source.active->stages[0].detail = "vendor-specific failure 0x80070005";
  projected =
      azzs::ui::presentation::make_guided_initialization_presentation(
          source, localized_text);
  auto const* raw_stage = projected->find_component("guided.stage.drivers");
  passed &= expect(raw_stage != nullptr &&
                       raw_stage->body.find("原始系统信息：vendor-specific failure") ==
                           0,
                   "unknown guided stage details must retain a Chinese context");
  return passed;
}

[[nodiscard]] bool verify_advanced_view_preference_fallback() {
  bool passed = true;
  auto store = std::make_shared<InMemoryAdvancedViewPreferenceStore>();
  store->read.enabled = true;
  azzs::application::AdvancedViewPreferences preferences{store};
  passed &= expect(preferences.enabled(),
                   "loaded advanced-view preference must initialize application state");
  passed &= expect(preferences.set_enabled(false) == false &&
                       store->last_written == false,
                   "successful advanced-view write must update application state");

  store->write_status =
      azzs::application::AdvancedViewPreferenceWriteStatus::unavailable;
  passed &= expect(preferences.set_enabled(true) == false,
                   "failed advanced-view write must preserve application state");

  auto unavailable = std::make_shared<InMemoryAdvancedViewPreferenceStore>();
  unavailable->read.status =
      azzs::application::AdvancedViewPreferenceReadStatus::unavailable;
  unavailable->read.enabled = true;
  azzs::application::AdvancedViewPreferences fallback{unavailable};
  passed &= expect(!fallback.enabled(),
                   "unavailable advanced-view read must fall back to standard view");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_motion_contract();
  passed &= verify_fixture_contract();
  passed &= verify_offline_package_cache_projection();
  passed &= verify_software_selection_empty_catalog_projection();
  passed &= verify_guided_initialization_projection();
  passed &= verify_advanced_view_preference_fallback();
  if (!passed) {
    return EXIT_FAILURE;
  }

  std::cout << "UI presentation contract passed\n";
  return EXIT_SUCCESS;
}
