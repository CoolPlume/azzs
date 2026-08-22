#include <algorithm>
#include <chrono>
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
#include "azzs/adapters/infrastructure/system_settings_recovery_store.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/settings_catalog/initial_settings_catalog.hpp"
#include "azzs/settings_catalog/settings_catalog.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"
#include "azzs/testing/fixed_clock.hpp"

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
using azzs::application::RecoveryRecordDeleteStatus;
using azzs::application::RecoveryRecordOperation;
using azzs::application::RecoveryRecordStatus;
using azzs::application::RecoveryStorageRead;
using azzs::application::RecoveryStorageStatus;
using azzs::application::RecoveryStorageWrite;
using azzs::application::SystemSettingApplyState;
using azzs::application::SystemSettingsAdapterResult;
using azzs::application::SystemSettingsAdapterStatus;
using azzs::application::SystemSettingsApplyRequest;
using azzs::application::SystemSettingsApplyService;
using azzs::application::SystemSettingsCatalogSnapshotSource;
using azzs::application::SystemSettingsFactAvailability;
using azzs::application::SystemSettingsOperationHistory;
using azzs::application::SystemSettingsOperationKind;
using azzs::application::SystemSettingsOperationStatus;
using azzs::application::SystemSettingsPlatformAdapter;
using azzs::application::SystemSettingsRead;
using azzs::application::SystemSettingsRecoveryRecord;
using azzs::application::SystemSettingsRecoveryStore;
using azzs::application::SystemSettingsSnapshotStatus;
using azzs::application::SystemSettingsUndoStatus;
using azzs::application::SystemSettingsWindowsVersionFact;
using azzs::application::WindowsSystemSettingValue;
using InfrastructureRecoveryStore =
    azzs::adapters::infrastructure::SystemSettingsRecoveryStore;
using azzs::application::DeviceStateStore;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::InMemoryStateFileSystem;
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

[[nodiscard]] catalog::WindowsVersion windows11_26h1() {
  return {.generation = catalog::WindowsGeneration::windows_11,
          .feature_update_year = 26,
          .feature_update_half = 1};
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
    replace(std::move(input));
  }

  [[nodiscard]] std::optional<catalog::ValidatedSettingsCatalog>
  current_settings_catalog() override {
    return catalog_;
  }

  void replace(catalog::SettingsCatalog input) {
    auto validation = catalog::validate(std::move(input), capabilities());
    catalog_ = std::move(validation.validated);
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
    return {.status = RecoveryStorageStatus::loaded,
            .records = records,
            .operation_history = operation_history};
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

  [[nodiscard]] RecoveryStorageWrite append_operation_fact(
      azzs::application::SystemSettingsOperationFact fact) override {
    operation_history.facts.push_back(std::move(fact));
    return {.status = RecoveryStorageStatus::committed};
  }

  [[nodiscard]] RecoveryStorageWrite erase(std::uint64_t record_id) override {
    events_.push_back("erase:" + std::to_string(record_id));
    auto found = std::ranges::find(records, record_id,
                                   &SystemSettingsRecoveryRecord::record_id);
    if (found == records.end()) {
      return {.status = RecoveryStorageStatus::failed,
              .detail = "injected missing recovery record"};
    }
    records.erase(found);
    return {.status = RecoveryStorageStatus::committed};
  }

  std::vector<SystemSettingsRecoveryRecord> records;
  SystemSettingsOperationHistory operation_history;

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

  [[nodiscard]] std::optional<SystemSettingsWindowsVersionFact>
  windows_version_fact() const override {
    return windows_environment;
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
    if (fail_restore == setting) {
      return {.status = SystemSettingsAdapterStatus::restore_failed,
              .detail = "injected restore failure"};
    }
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
  std::optional<SystemSettingsWindowsVersionFact> windows_environment{
      SystemSettingsWindowsVersionFact{.display_version = "Windows 11 25H2",
                                       .internal_build = 26'200}};
  std::optional<ControlledSystemSetting> fail_apply;
  std::optional<ControlledSystemSetting> fail_restore;
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

[[nodiscard]] bool verify_initial_catalog_executes_through_service() {
  Harness harness{
      azzs::application::settings_catalog::initial_settings_catalog()};
  bool passed = true;
  auto snapshot = harness.service.snapshot();
  passed &= expect(snapshot.catalog_revision == 1 &&
                       snapshot.settings.size() == 2 &&
                       snapshot.recommended_plan.has_value(),
                   "initial catalog must be projected by the apply service");
  passed &= expect(harness.service.select_recommended_plan(
                       catalog::StableId{"plan.recommended"}),
                   "initial recommended plan must use the apply service seam");
  passed &= expect(!harness.service.snapshot().can_apply,
                   "initial recommended plan must remain opt-in");
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "initial classic menu item must be independently selectable");
  auto result = harness.service.apply_selected();
  passed &= expect(state_for(result.settings, "setting.classic-context-menu") ==
                       SystemSettingApplyState::waiting_explorer_restart,
                   "initial classic menu item must execute through the typed service");
  passed &= expect(harness.recovery.records.size() == 1 &&
                       harness.recovery.records[0].catalog_revision == 1 &&
                       harness.recovery.records[0].setting ==
                           ControlledSystemSetting::classic_context_menu,
                   "initial execution must persist a typed frozen recovery record");
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
  harness.platform.version = windows11_26h1();
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
  passed &= expect(
      state_for(result.settings, "setting.classic-context-menu") ==
          SystemSettingApplyState::waiting_explorer_restart,
      "advanced confirmed force attempt may execute");
  Harness windows10{make_catalog(false)};
  windows10.platform.version = windows10_22h2();
  passed &= expect(windows10.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "Windows 10 setting can be selected for an applicability result");
  auto windows10_result = windows10.service.apply_selected(
      {.force_attempt_confirmed = true});
  passed &= expect(
      state_for(windows10_result.settings, "setting.classic-context-menu") ==
          SystemSettingApplyState::not_applicable,
      "Windows 10 must not force-apply a Windows 11-only setting");
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

[[nodiscard]] bool verify_operation_history_is_immutable_and_append_only() {
  auto input = make_catalog(false);
  input.plans[0].members[0].default_selected = true;
  Harness harness{std::move(input)};
  bool passed = true;
  passed &= expect(harness.service.select_recommended_plan(
                       catalog::StableId{"plan.recommended"}),
                   "operation history test must select a frozen plan");
  auto applied = harness.service.apply_selected();
  auto initial_history = harness.service.operation_history();
  passed &= expect(applied.waiting_for_explorer_restart &&
                       initial_history.facts.size() == 1,
                   "apply must append one immutable operation fact");
  if (initial_history.facts.empty()) {
    return false;
  }
  auto const& fact = initial_history.facts.front();
  passed &= expect(fact.operation == SystemSettingsOperationKind::apply &&
                       fact.catalog_availability ==
                           SystemSettingsFactAvailability::obtained &&
                       fact.catalog_identity == "system-settings-catalog" &&
                       fact.catalog_revision == 42 &&
                       fact.selected_plan_id.has_value() &&
                       fact.selected_plan_id->value == "plan.recommended",
                   "apply history must freeze catalog identity, revision and plan");
  passed &= expect(
      fact.windows_environment.availability ==
          SystemSettingsFactAvailability::obtained &&
          fact.windows_environment.display_version == "Windows 11 25H2" &&
          fact.windows_environment.internal_build == 26'200 &&
          fact.settings.size() == 1 &&
          fact.settings.front().declared_range_availability ==
              SystemSettingsFactAvailability::obtained &&
          fact.settings.front().original_value ==
              WindowsSystemSettingValue{ClassicContextMenuMode::windows11} &&
          fact.settings.front().target_value ==
              WindowsSystemSettingValue{ClassicContextMenuMode::classic},
      "apply history must freeze Windows, declared range, original and target facts");
  passed &= expect(
      fact.explorer_restart_result ==
          azzs::application::SystemSettingsExplorerRestartResult::deferred &&
          fact.status ==
              SystemSettingsOperationStatus::waiting_explorer_restart &&
          fact.timeline.size() == 2,
      "deferred Explorer restart must be part of the immutable operation fact");

  auto changed_catalog = make_catalog(false);
  changed_catalog.revision = 99;
  changed_catalog.settings.front().display_name = "更新后的设置名称";
  changed_catalog.settings.front().known_windows_range.minimum = windows11_26h1();
  harness.catalog_source.replace(std::move(changed_catalog));
  harness.platform.version = windows11_26h1();
  harness.platform.windows_environment = SystemSettingsWindowsVersionFact{
      .display_version = "Windows 11 26H1", .internal_build = 27'000};
  static_cast<void>(harness.service.refresh());
  passed &= expect(harness.service.operation_history() == initial_history,
                   "later catalog and Windows changes must not backfill old facts");

  auto restarted = harness.service.restart_explorer_now();
  auto after_restart = harness.service.operation_history();
  passed &= expect(restarted.status == SystemSettingsSnapshotStatus::completed &&
                       after_restart.facts.size() == 2 &&
                       after_restart.facts[1].operation ==
                           SystemSettingsOperationKind::restart_explorer &&
                       after_restart.facts[1].explorer_restart_result ==
                           azzs::application::SystemSettingsExplorerRestartResult::
                               succeeded,
                   "Explorer restart verification must append its own result fact");
  passed &= expect(after_restart.facts.front() == initial_history.facts.front(),
                   "restart verification must not rewrite the apply fact");

  harness.platform.fail_restore = ControlledSystemSetting::classic_context_menu;
  auto failed_undo = harness.service.undo(
      catalog::StableId{"setting.classic-context-menu"});
  auto after_failed_undo = harness.service.operation_history();
  passed &= expect(failed_undo.status == SystemSettingsUndoStatus::failed &&
                       after_failed_undo.facts.size() == 3 &&
                       after_failed_undo.facts.back().operation ==
                           SystemSettingsOperationKind::restore &&
                       after_failed_undo.facts.back().status ==
                           SystemSettingsOperationStatus::failed,
                   "failed restore must append an immutable failure fact");

  harness.platform.fail_restore.reset();
  auto retried_undo = harness.service.undo(
      catalog::StableId{"setting.classic-context-menu"});
  auto after_retry = harness.service.operation_history();
  passed &= expect(
      retried_undo.status == SystemSettingsUndoStatus::waiting_explorer_restart &&
          after_retry.facts.size() == 4 &&
          after_retry.facts.back().status ==
              SystemSettingsOperationStatus::waiting_explorer_restart &&
          after_retry.facts[2] == after_failed_undo.facts.back(),
      "restore retry must append a new fact without changing the failed one");
  return passed;
}

[[nodiscard]] bool verify_legacy_recovery_projects_not_obtained_facts() {
  SystemSettingsRecoveryRecord record{
      .record_id = 77,
      .setting_id = catalog::StableId{"retired.classic-context-menu"},
      .display_name = "旧恢复记录",
      .catalog_revision = 7,
      .setting = ControlledSystemSetting::classic_context_menu,
      .original_value = ClassicContextMenuMode::windows11,
      .restart_requirement = catalog::RestartRequirement::none,
      .status = RecoveryRecordStatus::applied,
  };
  Harness harness{make_catalog(false), {record}};
  auto history = harness.service.operation_history();
  bool passed = true;
  passed &= expect(history.facts.size() == 1 &&
                       history.facts.front().fact_id != 0 &&
                       history.facts.front().catalog_availability ==
                           SystemSettingsFactAvailability::not_obtained &&
                       history.facts.front().catalog_identity == "NOT_OBTAINED" &&
                       history.facts.front().windows_environment.availability ==
                           SystemSettingsFactAvailability::not_obtained &&
                       history.facts.front().settings.size() == 1 &&
                       history.facts.front().settings.front().recovery_record_id ==
                           record.record_id,
                   "legacy recovery records must project missing history fields as NOT_OBTAINED");
  passed &= expect(
      history.facts.front().catalog_reason ==
          "NOT_OBTAINED: legacy recovery record predates immutable operation history" &&
          history.facts.front().settings.front().declared_range_reason ==
              "NOT_OBTAINED: legacy recovery record predates immutable operation history",
      "legacy NOT_OBTAINED facts must carry stable reasons");
  return passed;
}

[[nodiscard]] bool
verify_legacy_history_survives_new_operation_and_restart() {
  SystemSettingsRecoveryRecord legacy{
      .record_id = 77,
      .setting_id = catalog::StableId{"retired.classic-context-menu"},
      .display_name = "旧恢复记录",
      .catalog_revision = 7,
      .setting = ControlledSystemSetting::classic_context_menu,
      .original_value = ClassicContextMenuMode::windows11,
      .restart_requirement = catalog::RestartRequirement::none,
      .status = RecoveryRecordStatus::applied,
  };
  Harness harness{make_catalog(false), {legacy}};
  bool passed = true;
  passed &= expect(harness.service.set_selected(
                       catalog::StableId{"setting.classic-context-menu"}, true),
                   "legacy migration test must select a new operation");
  auto applied = harness.service.apply_selected();
  auto const persisted_history = harness.recovery.operation_history;
  auto const legacy_fact = std::ranges::find_if(
      persisted_history.facts,
      [&legacy](azzs::application::SystemSettingsOperationFact const& fact) {
        return std::ranges::any_of(
            fact.settings,
            [&legacy](azzs::application::SystemSettingsOperationSettingFact const&
                          setting) {
              return setting.recovery_record_id == legacy.record_id;
            });
      });
  passed &= expect(
      applied.status == SystemSettingsSnapshotStatus::completed &&
          persisted_history.facts.size() == 2 &&
          legacy_fact != persisted_history.facts.end() &&
          legacy_fact->catalog_availability ==
              SystemSettingsFactAvailability::not_obtained,
      "new operations must persist the projected legacy fact before appending their own fact");

  InMemoryOperationOccupancyStorage restarted_storage;
  SequenceLeaseTokenSource restarted_tokens{"restarted-lease-"};
  azzs::application::SharedOperationOccupancy restarted_occupancy{
      restarted_storage, restarted_tokens};
  RecordingLog restarted_log;
  SystemSettingsApplyService restarted{harness.catalog_source, harness.platform,
                                       harness.recovery, restarted_occupancy,
                                       restarted_log};
  passed &= expect(
      restarted.operation_history() == persisted_history,
      "restart must retain both the legacy projection and the later operation fact");
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

[[nodiscard]] bool verify_undo_restores_most_recent_original_value() {
  SystemSettingsRecoveryRecord first{
      .record_id = 1,
      .setting_id = catalog::StableId{"setting.classic-context-menu"},
      .display_name = "经典右键菜单",
      .catalog_revision = 11,
      .setting = ControlledSystemSetting::classic_context_menu,
      .original_value = ClassicContextMenuMode::windows11,
      .restart_requirement = catalog::RestartRequirement::none,
      .status = RecoveryRecordStatus::applied,
  };
  SystemSettingsRecoveryRecord most_recent = first;
  most_recent.record_id = 2;
  most_recent.catalog_revision = 12;
  most_recent.original_value = ClassicContextMenuMode::classic;
  Harness harness{make_catalog(false), {first, most_recent}};
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::classic_context_menu));
  harness.events.clear();
  auto result = harness.service.undo(
      catalog::StableId{"setting.classic-context-menu"});
  bool passed = true;
  passed &= expect(result.status == SystemSettingsUndoStatus::restored,
                   "undo must restore the newest saved original value");
  passed &= expect(result.record_id == 2,
                   "undo must target the latest recovery record");
  passed &= expect(harness.recovery.records[1].status ==
                       RecoveryRecordStatus::restored,
                   "verified undo must mark the target record restored");
  passed &= expect(std::ranges::find(harness.events, "restore:classic") !=
                       harness.events.end(),
                   "undo must call the typed platform restore seam");
  return passed;
}

[[nodiscard]] bool verify_undo_survives_retired_catalog_item() {
  SystemSettingsRecoveryRecord record{
      .record_id = 7,
      .setting_id = catalog::StableId{"retired.classic-context-menu"},
      .display_name = "已下架经典右键菜单",
      .catalog_revision = 7,
      .setting = ControlledSystemSetting::classic_context_menu,
      .original_value = ClassicContextMenuMode::windows11,
      .restart_requirement = catalog::RestartRequirement::none,
      .status = RecoveryRecordStatus::applied,
  };
  Harness harness{make_catalog(false), {record}};
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::classic_context_menu));
  auto result = harness.service.undo(record.setting_id);
  bool passed = true;
  passed &= expect(result.status == SystemSettingsUndoStatus::restored,
                   "retired settings with recovery records remain undoable");
  passed &= expect(harness.recovery.records[0].status ==
                       RecoveryRecordStatus::restored,
                   "retired setting undo must commit against its old snapshot");
  return passed;
}

[[nodiscard]] bool verify_failed_undo_keeps_retryable_record() {
  SystemSettingsRecoveryRecord record{
      .record_id = 8,
      .setting_id = catalog::StableId{"setting.windows10-explorer"},
      .display_name = "Windows 10 风格资源管理器",
      .catalog_revision = 42,
      .setting = ControlledSystemSetting::windows10_explorer,
      .original_value = ExplorerPresentationMode::windows11,
      .restart_requirement = catalog::RestartRequirement::explorer,
      .status = RecoveryRecordStatus::applied,
  };
  Harness harness{make_catalog(false), {record}};
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::windows10_explorer));
  harness.platform.fail_restore = ControlledSystemSetting::windows10_explorer;
  auto failed = harness.service.undo(record.setting_id);
  bool passed = true;
  passed &= expect(failed.status == SystemSettingsUndoStatus::failed,
                   "restore failure must be reported");
  passed &= expect(harness.recovery.records[0].status ==
                       RecoveryRecordStatus::restore_failed,
                   "failed undo must retain a retryable recovery record");
  harness.platform.fail_restore.reset();
  auto retried = harness.service.undo(record.setting_id);
  passed &= expect(retried.status ==
                       SystemSettingsUndoStatus::waiting_explorer_restart,
                   "failed undo must allow a later retry");
  passed &= expect(harness.recovery.records[0].status ==
                       RecoveryRecordStatus::waiting_explorer_restart,
                   "retry must retain the restart verification state");
  return passed;
}

[[nodiscard]] bool verify_undo_restart_validation_and_delete_protection() {
  SystemSettingsRecoveryRecord record{
      .record_id = 9,
      .setting_id = catalog::StableId{"setting.windows10-explorer"},
      .display_name = "Windows 10 风格资源管理器",
      .catalog_revision = 42,
      .setting = ControlledSystemSetting::windows10_explorer,
      .original_value = ExplorerPresentationMode::windows11,
      .restart_requirement = catalog::RestartRequirement::explorer,
      .status = RecoveryRecordStatus::applied,
  };
  Harness harness{make_catalog(false), {record}};
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::windows10_explorer));
  auto undo = harness.service.undo(record.setting_id);
  bool passed = true;
  passed &= expect(undo.status ==
                       SystemSettingsUndoStatus::waiting_explorer_restart,
                   "explorer undo must wait for restart verification");
  passed &= expect(harness.service.delete_recovery_record(record.record_id, true)
                       .status == RecoveryRecordDeleteStatus::blocked,
                   "pending restart verification protects recovery records");
  auto restarted = harness.service.restart_explorer_now();
  passed &= expect(restarted.status == SystemSettingsSnapshotStatus::completed,
                   "restart verifies the frozen undo snapshot");
  passed &= expect(harness.recovery.records[0].status ==
                       RecoveryRecordStatus::restored,
                   "record remains until undo restart verification succeeds");
  auto confirmation =
      harness.service.delete_recovery_record(record.record_id, false);
  passed &= expect(confirmation.status ==
                       RecoveryRecordDeleteStatus::confirmation_required,
                   "recovery deletion requires a separate confirmation");
  passed &= expect(harness.service.delete_recovery_record(record.record_id, true)
                       .status == RecoveryRecordDeleteStatus::deleted,
                   "confirmed deletion removes the persisted recovery record");
  passed &= expect(harness.recovery.records.empty(),
                   "confirmed deletion must remove the in-memory projection");
  return passed;
}

[[nodiscard]] bool verify_windows11_default_saves_current_value() {
  Harness harness{make_catalog(false)};
  static_cast<void>(harness.service.set_selected(
      catalog::StableId{"setting.windows10-explorer"}, true));
  static_cast<void>(
      harness.platform.apply(ControlledSystemSetting::classic_context_menu));
  harness.events.clear();
  auto result = harness.service.restore_windows11_default(
      catalog::StableId{"setting.classic-context-menu"});
  bool passed = true;
  passed &= expect(result.status == SystemSettingsSnapshotStatus::completed,
                   "Windows 11 default target must execute through the service");
  passed &= expect(harness.recovery.records.size() == 1 &&
                       harness.recovery.records[0].original_value ==
                           WindowsSystemSettingValue{ClassicContextMenuMode::classic},
                   "Windows 11 default must save the current value first");
  passed &= expect(harness.recovery.records[0].operation ==
                       RecoveryRecordOperation::windows11_default,
                   "Windows 11 default must remain distinct from historical undo");
  passed &= expect(harness.service.snapshot().settings[1].selected,
                   "Windows 11 default must preserve existing user selection");
  return passed;
}

[[nodiscard]] bool verify_recovery_store_persists_undo_snapshot_and_erase() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'508'800'000}}};
  DeviceStateStore first_states{files, clock};
  InfrastructureRecoveryStore first{first_states};
  SystemSettingsRecoveryRecord record{
      .record_id = 22,
      .setting_id = catalog::StableId{"retired.classic-context-menu"},
      .display_name = "已下架经典右键菜单",
      .catalog_revision = 7,
      .setting = ControlledSystemSetting::classic_context_menu,
      .original_value = ClassicContextMenuMode::windows11,
      .restart_requirement = catalog::RestartRequirement::explorer,
      .status = RecoveryRecordStatus::waiting_explorer_restart,
      .operation = RecoveryRecordOperation::restore,
  };
  bool passed = true;
  passed &= expect(first.save(record).status == RecoveryStorageStatus::committed,
                   "infrastructure recovery store must save an undo snapshot");
  azzs::application::SystemSettingsOperationFact fact{
      .fact_id = 88,
      .operation = SystemSettingsOperationKind::restore,
      .catalog_availability = SystemSettingsFactAvailability::not_obtained,
      .catalog_identity = "NOT_OBTAINED",
      .catalog_revision = record.catalog_revision,
      .catalog_reason = "NOT_OBTAINED: frozen recovery record has no catalog identity",
      .windows_environment = {
          .availability = SystemSettingsFactAvailability::obtained,
          .display_version = "Windows 11 25H2",
          .internal_build = 26'200,
          .reason = {}},
      .explorer_restart_requested = true,
      .explorer_restart_result =
          azzs::application::SystemSettingsExplorerRestartResult::deferred,
      .status = SystemSettingsOperationStatus::waiting_explorer_restart,
      .reason = "等待资源管理器重启后重新验证",
      .settings = {{
          .setting_id = record.setting_id,
          .display_name = record.display_name,
          .controlled_identity = "NOT_OBTAINED",
          .catalog_revision = record.catalog_revision,
          .declared_range_availability =
              SystemSettingsFactAvailability::not_obtained,
          .declared_range_reason =
              "NOT_OBTAINED: frozen recovery record has no declared Windows range",
          .original_value = record.original_value,
          .target_value = record.original_value,
          .recovery_record_id = record.record_id,
          .restart_requirement = record.restart_requirement,
          .status = SystemSettingsOperationStatus::waiting_explorer_restart,
          .reason = "等待资源管理器重启后重新验证"}},
      .timeline = {{
          .ordinal = 1,
          .stage = "restore-finished",
          .status = SystemSettingsOperationStatus::waiting_explorer_restart,
          .reason = "等待资源管理器重启后重新验证"}},
  };
  passed &= expect(first.append_operation_fact(fact).status ==
                       RecoveryStorageStatus::committed,
                   "infrastructure recovery store must append immutable facts");
  DeviceStateStore restarted_states{files, clock};
  InfrastructureRecoveryStore restarted{restarted_states};
  auto loaded = restarted.read();
  passed &= expect(loaded.status == RecoveryStorageStatus::loaded &&
                       loaded.records.size() == 1 &&
                       loaded.records[0].display_name == record.display_name &&
                       loaded.records[0].operation ==
                           RecoveryRecordOperation::restore &&
                       loaded.operation_history.facts.size() == 1 &&
                       loaded.operation_history.facts.front() == fact,
                   "restart must retain frozen recovery and operation-history facts");
  passed &= expect(restarted.erase(record.record_id).status ==
                       RecoveryStorageStatus::committed,
                   "infrastructure recovery store must erase a confirmed record");
  DeviceStateStore erased_states{files, clock};
  InfrastructureRecoveryStore erased{erased_states};
  auto empty = erased.read();
  passed &= expect(empty.status == RecoveryStorageStatus::loaded &&
                       empty.records.empty() &&
                       empty.operation_history.facts.size() == 1,
                   "recovery deletion must not remove immutable operation history");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_initial_catalog_executes_through_service();
  passed &= verify_selection_snapshot();
  passed &= verify_serial_apply_and_recovery();
  passed &= verify_already_effective_skips();
  passed &= verify_failure_only_blocks_dependencies();
  passed &= verify_applicability_and_force_attempt();
  passed &= verify_explorer_restart_validation();
  passed &= verify_operation_history_is_immutable_and_append_only();
  passed &= verify_legacy_recovery_projects_not_obtained_facts();
  passed &= verify_legacy_history_survives_new_operation_and_restart();
  passed &= verify_reopened_explorer_wait_state();
  passed &= verify_undo_restores_most_recent_original_value();
  passed &= verify_undo_survives_retired_catalog_item();
  passed &= verify_failed_undo_keeps_retryable_record();
  passed &= verify_undo_restart_validation_and_delete_protection();
  passed &= verify_windows11_default_saves_current_value();
  passed &= verify_recovery_store_persists_undo_snapshot_and_erase();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "system settings apply contract passed\n";
  return EXIT_SUCCESS;
}
