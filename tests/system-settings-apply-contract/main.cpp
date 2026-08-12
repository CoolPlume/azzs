#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/settings_catalog/settings_catalog.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"

namespace {

namespace catalog = azzs::domain::settings_catalog;
using azzs::application::ClassicContextMenuMode;
using azzs::application::ControlledSystemSetting;
using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::ExplorerPresentationMode;
using azzs::application::RecoveryStorageRead;
using azzs::application::RecoveryStorageStatus;
using azzs::application::RecoveryStorageWrite;
using azzs::application::SystemSettingApplyState;
using azzs::application::SystemSettingsAdapterResult;
using azzs::application::SystemSettingsAdapterStatus;
using azzs::application::SystemSettingsApplyRequest;
using azzs::application::SystemSettingsApplyService;
using azzs::application::SystemSettingsCatalogSnapshotSource;
using azzs::application::SystemSettingsPlatformAdapter;
using azzs::application::SystemSettingsRead;
using azzs::application::SystemSettingsRecoveryRecord;
using azzs::application::SystemSettingsRecoveryStore;
using azzs::application::SystemSettingsSnapshotStatus;
using azzs::application::WindowsSystemSettingValue;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::SequenceLeaseTokenSource;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "system settings apply contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] catalog::SupportedCapabilities capabilities() {
  return {.apply = {"windows.classic-context-menu.apply",
                    "windows.windows10-explorer.apply"},
          .detect = {"windows.classic-context-menu.detect",
                     "windows.windows10-explorer.detect"},
          .recover = {"windows.classic-context-menu.restore",
                      "windows.windows10-explorer.restore"}};
}

[[nodiscard]] catalog::WindowsVersion windows11_25h2() {
  return {.generation = catalog::WindowsGeneration::windows_11,
          .feature_update_year = 25,
          .feature_update_half = 2};
}

[[nodiscard]] catalog::WindowsVersion windows10_22h2() {
  return {.generation = catalog::WindowsGeneration::windows_10,
          .feature_update_year = 22,
          .feature_update_half = 2};
}

[[nodiscard]] catalog::SettingDefinition setting(
    std::string id, std::string name, std::string identity,
    std::string apply, std::string detect, std::string recover,
    bool default_selected) {
  return {
      .id = catalog::StableId{std::move(id)},
      .display_name = std::move(name),
      .description = "受控系统设置合同测试项。",
      .source_url = "https://example.invalid/settings-source",
      .known_windows_range = {.minimum = catalog::WindowsVersion{
                                  .generation =
                                      catalog::WindowsGeneration::windows_11,
                                  .feature_update_year = 21,
                                  .feature_update_half = 2},
                              .maximum = windows11_25h2()},
      .default_selected = default_selected,
      .risk = catalog::SettingRiskLevel::elevated,
      .force_attempt_rule =
          catalog::ForceAttemptRule::allowed_with_explicit_confirmation,
      .recovery_requirement =
          catalog::RecoveryRequirement::restore_record_required,
      .restart_requirement = catalog::RestartRequirement::explorer,
      .semantics = {.identity = std::move(identity),
                    .apply_capability = std::move(apply),
                    .detect_capability = std::move(detect),
                    .recover_capability = std::move(recover)}};
}

[[nodiscard]] catalog::SettingsCatalog make_catalog(bool explorer_depends) {
  auto classic =
      setting("setting.classic-context-menu", "经典右键菜单",
              "windows11.classic-context-menu",
              "windows.classic-context-menu.apply",
              "windows.classic-context-menu.detect",
              "windows.classic-context-menu.restore", false);
  auto explorer = setting("setting.windows10-explorer",
                          "Windows 10 风格资源管理器",
                          "windows11.windows10-explorer",
                          "windows.windows10-explorer.apply",
                          "windows.windows10-explorer.detect",
                          "windows.windows10-explorer.restore", false);
  if (explorer_depends) {
    explorer.depends_on = {catalog::StableId{"setting.classic-context-menu"}};
  }
  return {.revision = 42,
          .settings = {std::move(classic), std::move(explorer)},
          .plans = {catalog::OptimizationPlan{
              .id = catalog::StableId{"plan.recommended"},
              .display_name = "推荐总体优化",
              .description = "同一批系统设置的推荐组合。",
              .members = {{.setting_id = catalog::StableId{
                               "setting.classic-context-menu"},
                           .order = 10,
                           .default_selected = false},
                          {.setting_id = catalog::StableId{
                               "setting.windows10-explorer"},
                           .order = 20,
                           .default_selected = false}}}}};
}

class MemoryCatalogSource final : public SystemSettingsCatalogSnapshotSource {
 public:
  explicit MemoryCatalogSource(catalog::SettingsCatalog input) {
    auto validation = catalog::validate(std::move(input), capabilities());
    catalog_ = std::move(validation.validated);
  }

  [[nodiscard]] std::optional<catalog::ValidatedSettingsCatalog>
  current_settings_catalog() override {
    return catalog_;
  }

 private:
  std::optional<catalog::ValidatedSettingsCatalog> catalog_;
};

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"system-settings-contract"};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events.push_back(event);
    return {.persisted = true, .sequence = events.size()};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    return {.cleared = true};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true, .file_count = 1};
  }

  std::vector<ExecutionEvent> events;
};

class MemoryRecoveryStore final : public SystemSettingsRecoveryStore {
 public:
  explicit MemoryRecoveryStore(
      std::vector<std::string>& events,
      std::vector<SystemSettingsRecoveryRecord> initial_records = {})
      : records(std::move(initial_records)), events_(events) {}

  [[nodiscard]] RecoveryStorageRead read() override {
    return {.status = RecoveryStorageStatus::loaded, .records = records};
  }

  [[nodiscard]] RecoveryStorageWrite save(
      SystemSettingsRecoveryRecord record) override {
    events_.push_back("save:" + record.setting_id.value);
    auto found = std::ranges::find(records, record.record_id,
                                   &SystemSettingsRecoveryRecord::record_id);
    if (found == records.end()) {
      records.push_back(std::move(record));
    } else {
      *found = std::move(record);
    }
    return {.status = RecoveryStorageStatus::committed};
  }

  std::vector<SystemSettingsRecoveryRecord> records;

 private:
  std::vector<std::string>& events_;
};

class MemoryPlatform final : public SystemSettingsPlatformAdapter {
 public:
  explicit MemoryPlatform(std::vector<std::string>& events) : events_(events) {
    values_[ControlledSystemSetting::classic_context_menu] =
        ClassicContextMenuMode::windows11;
    values_[ControlledSystemSetting::windows10_explorer] =
        ExplorerPresentationMode::windows11;
  }

  [[nodiscard]] std::optional<catalog::WindowsVersion> windows_version()
      const override {
    return version;
  }

  [[nodiscard]] SystemSettingsRead read(
      ControlledSystemSetting setting) override {
    events_.push_back("read:" + name(setting));
    return {.status = SystemSettingsAdapterStatus::succeeded,
            .value = values_[setting]};
  }

  [[nodiscard]] SystemSettingsAdapterResult apply(
      ControlledSystemSetting setting) override {
    events_.push_back("apply:" + name(setting));
    if (fail_apply == setting) {
      return {.status = SystemSettingsAdapterStatus::apply_failed,
              .detail = "injected apply failure"};
    }
    switch (setting) {
      case ControlledSystemSetting::classic_context_menu:
        values_[setting] = ClassicContextMenuMode::classic;
        break;
      case ControlledSystemSetting::windows10_explorer:
        values_[setting] = ExplorerPresentationMode::windows10;
        break;
    }
    return {.status = SystemSettingsAdapterStatus::succeeded};
  }

  [[nodiscard]] SystemSettingsAdapterResult restore(
      ControlledSystemSetting setting, WindowsSystemSettingValue value) override {
    events_.push_back("restore:" + name(setting));
    values_[setting] = value;
    return {.status = SystemSettingsAdapterStatus::succeeded};
  }

  [[nodiscard]] SystemSettingsAdapterResult restart_explorer() override {
    events_.push_back("restart-explorer");
    restarted = true;
    return {.status = SystemSettingsAdapterStatus::succeeded};
  }

  [[nodiscard]] static std::string name(ControlledSystemSetting setting) {
    switch (setting) {
      case ControlledSystemSetting::classic_context_menu:
        return "classic";
      case ControlledSystemSetting::windows10_explorer:
        return "explorer";
    }
    return "unknown";
  }

  std::optional<catalog::WindowsVersion> version{windows11_25h2()};
  std::optional<ControlledSystemSetting> fail_apply;
  bool restarted{false};

 private:
  std::map<ControlledSystemSetting, WindowsSystemSettingValue> values_;
  std::vector<std::string>& events_;
};

class Harness final {
 public:
  explicit Harness(
      catalog::SettingsCatalog catalog,
      std::vector<SystemSettingsRecoveryRecord> recovery_records = {})
      : catalog_source(std::move(catalog)),
        platform(events),
        recovery(events, std::move(recovery_records)),
        occupancy(occupancy_storage, tokens),
        service(catalog_source, platform, recovery, occupancy, log) {}

  std::vector<std::string> events;
  MemoryCatalogSource catalog_source;
  MemoryPlatform platform;
  MemoryRecoveryStore recovery;
  InMemoryOperationOccupancyStorage occupancy_storage;
  SequenceLeaseTokenSource tokens{"lease-"};
  azzs::application::SharedOperationOccupancy occupancy;
  RecordingLog log;
  SystemSettingsApplyService service;
};

[[nodiscard]] SystemSettingApplyState state_for(
    std::vector<azzs::application::SystemSettingApplySnapshot> const& items,
    std::string_view id) {
  auto const found = std::ranges::find(items, id,
                                       [](auto const& item) {
                                         return item.id.value;
                                       });
  return found == items.end() ? SystemSettingApplyState::failed
                              : found->state;
}

[[nodiscard]] bool verify_selection_snapshot() {
  Harness harness{make_catalog(false)};
  bool passed = true;
  auto snapshot = harness.service.snapshot();
  passed &= expect(snapshot.catalog_revision == 42,
                   "snapshot must expose the active catalog revision");
  passed &= expect(snapshot.recommended_plan.has_value() &&
                       snapshot.recommended_plan->value == "plan.recommended",
                   "snapshot must expose the enabled recommended plan");
  passed &= expect(snapshot.settings.size() == 2,
                   "snapshot must project catalog settings");
  passed &= expect(snapshot.settings[0].source_url.has_value() &&
                       snapshot.settings[0].recovery_available,
                   "snapshot must project source and recovery facts");
  passed &= expect(harness.service.select_recommended_plan(
                       catalog::StableId{"plan.recommended"}),
                   "recommended plan must select through the same state");
  snapshot = harness.service.snapshot();
  passed &= expect(snapshot.selected_plan.has_value(),
                   "recommended plan identity must be visible");
  passed &= expect(!snapshot.can_apply,
                   "high impact plan members are not preselected");
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"},
                       true),
                   "single item selection must share plan state");
  passed &= expect(!harness.service.snapshot().selected_plan.has_value(),
                   "manual selection clears the plan identity without duplicating state");
  return passed;
}

[[nodiscard]] bool verify_serial_apply_and_recovery() {
  Harness harness{make_catalog(false)};
  bool passed = true;
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "classic context menu can be selected");
  auto result = harness.service.apply_selected();
  passed &= expect(result.status == SystemSettingsSnapshotStatus::completed,
                   "apply should complete when verification succeeds");
  passed &= expect(result.waiting_for_explorer_restart,
                   "explorer restart must remain pending");
  passed &= expect(harness.recovery.records.size() == 1,
                   "original value must be persisted");
  passed &= expect(harness.recovery.records[0].setting ==
                       ControlledSystemSetting::classic_context_menu,
                   "recovery record must use a typed controlled setting");
  passed &= expect(harness.recovery.records[0].status ==
                       azzs::application::RecoveryRecordStatus::
                           waiting_explorer_restart,
                   "explorer changes must persist a waiting restart record");
  std::vector<std::string> expected_prefix{
      "read:classic", "save:setting.classic-context-menu", "apply:classic",
      "read:classic", "save:setting.classic-context-menu"};
  passed &= expect(harness.events.size() >= expected_prefix.size(),
                   "observable events must include serial execution steps");
  for (std::size_t index = 0;
       index < expected_prefix.size() && index < harness.events.size();
       ++index) {
    passed &= expect(harness.events[index] == expected_prefix[index],
                     "read, persist, apply, verify and commit order must be fixed");
  }
  return passed;
}

[[nodiscard]] bool verify_already_effective_skips() {
  Harness harness{make_catalog(false)};
  bool passed = true;
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::classic_context_menu));
  harness.events.clear();
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "classic context menu can be selected");
  auto result = harness.service.apply_selected();
  passed &= expect(state_for(result.settings, "setting.classic-context-menu") ==
                       SystemSettingApplyState::already_effective,
                   "already effective item must be skipped");
  passed &= expect(harness.recovery.records.empty(),
                   "skipped item must not create a new recovery record");
  passed &= expect(std::ranges::none_of(harness.events, [](auto const& event) {
                     return event.starts_with("apply:");
                   }),
                   "skipped item must not call apply");
  return passed;
}

[[nodiscard]] bool verify_failure_only_blocks_dependencies() {
  Harness independent{make_catalog(false)};
  bool passed = true;
  independent.platform.fail_apply =
      ControlledSystemSetting::classic_context_menu;
  passed &= expect(independent.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "classic context menu can be selected");
  passed &= expect(independent.service.set_selected(
                       catalog::StableId{"setting.windows10-explorer"}, true),
                   "explorer can be selected");
  auto result = independent.service.apply_selected();
  passed &= expect(state_for(result.settings, "setting.classic-context-menu") ==
                       SystemSettingApplyState::failed,
                   "failed item must be reported individually");
  passed &= expect(state_for(result.settings, "setting.windows10-explorer") ==
                       SystemSettingApplyState::waiting_explorer_restart,
                   "independent item must continue after another failure");

  Harness dependent{make_catalog(true)};
  dependent.platform.fail_apply = ControlledSystemSetting::classic_context_menu;
  passed &= expect(dependent.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "classic dependency can be selected");
  passed &= expect(dependent.service.set_selected(
                       catalog::StableId{"setting.windows10-explorer"}, true),
                   "dependent explorer can be selected");
  result = dependent.service.apply_selected();
  passed &= expect(state_for(result.settings, "setting.windows10-explorer") ==
                       SystemSettingApplyState::blocked_by_dependency,
                   "dependent item must be blocked by dependency failure");
  return passed;
}

[[nodiscard]] bool verify_applicability_and_force_attempt() {
  Harness harness{make_catalog(false)};
  bool passed = true;
  harness.platform.version = windows10_22h2();
  auto snapshot = harness.service.refresh();
  passed &= expect(!snapshot.settings[0].applicable,
                   "standard path marks out-of-range setting not applicable");
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "out-of-range item can remain selected for force handling");
  auto result = harness.service.apply_selected();
  passed &= expect(state_for(result.settings, "setting.classic-context-menu") ==
                       SystemSettingApplyState::force_confirmation_required,
                   "recoverable out-of-range item requires explicit confirmation");
  result = harness.service.apply_selected({.force_attempt_confirmed = true});
  passed &= expect(state_for(result.settings, "setting.classic-context-menu") ==
                       SystemSettingApplyState::waiting_explorer_restart,
                   "advanced confirmed force attempt may execute");
  return passed;
}

[[nodiscard]] bool verify_explorer_restart_validation() {
  Harness harness{make_catalog(false)};
  bool passed = true;
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.windows10-explorer"}, true),
                   "explorer style can be selected");
  auto result = harness.service.apply_selected();
  passed &= expect(result.waiting_for_explorer_restart,
                   "apply can defer explorer restart");
  result = harness.service.restart_explorer_now();
  passed &= expect(result.explorer_restart_attempted,
                   "explicit restart command must reach the platform seam");
  passed &= expect(!result.waiting_for_explorer_restart,
                   "successful restart validation clears waiting state");
  passed &= expect(harness.platform.restarted,
                   "platform adapter restart was invoked");
  passed &= expect(harness.recovery.records[0].status ==
                       azzs::application::RecoveryRecordStatus::applied,
                   "restart validation must commit the recovery record");
  return passed;
}

[[nodiscard]] bool verify_reopened_explorer_wait_state() {
  SystemSettingsRecoveryRecord record{
      .record_id = 7,
      .setting_id = catalog::StableId{"setting.windows10-explorer"},
      .catalog_revision = 42,
      .setting = ControlledSystemSetting::windows10_explorer,
      .original_value = ExplorerPresentationMode::windows11,
      .restart_requirement = catalog::RestartRequirement::explorer,
      .status =
          azzs::application::RecoveryRecordStatus::waiting_explorer_restart,
  };
  Harness harness{make_catalog(false), {record}};
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::windows10_explorer));
  harness.events.clear();
  auto snapshot = harness.service.refresh();
  bool passed = true;
  passed &= expect(snapshot.waiting_for_explorer_restart,
                   "reopened service must retain pending explorer validation");
  passed &= expect(
      state_for(snapshot.settings, "setting.windows10-explorer") ==
          SystemSettingApplyState::waiting_explorer_restart,
      "reopened service must re-detect and project the waiting setting");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_selection_snapshot();
  passed &= verify_serial_apply_and_recovery();
  passed &= verify_already_effective_skips();
  passed &= verify_failure_only_blocks_dependencies();
  passed &= verify_applicability_and_force_attempt();
  passed &= verify_explorer_restart_validation();
  passed &= verify_reopened_explorer_wait_state();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "system settings apply contract passed\n";
  return EXIT_SUCCESS;
}
