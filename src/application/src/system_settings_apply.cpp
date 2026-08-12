#include "azzs/application/system_settings_apply.hpp"

#include <algorithm>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace azzs::application {
namespace {

constexpr std::string_view kClassicContextMenuIdentity{
    "windows11.classic-context-menu"};
constexpr std::string_view kWindows10ExplorerIdentity{
    "windows11.windows10-explorer"};
constexpr std::string_view kClassicContextMenuApply{
    "windows.classic-context-menu.apply"};
constexpr std::string_view kClassicContextMenuDetect{
    "windows.classic-context-menu.detect"};
constexpr std::string_view kClassicContextMenuRecover{
    "windows.classic-context-menu.restore"};
constexpr std::string_view kWindows10ExplorerApply{
    "windows.windows10-explorer.apply"};
constexpr std::string_view kWindows10ExplorerDetect{
    "windows.windows10-explorer.detect"};
constexpr std::string_view kWindows10ExplorerRecover{
    "windows.windows10-explorer.restore"};

[[nodiscard]] bool is_success(SystemSettingApplyState state) noexcept {
  return state == SystemSettingApplyState::already_effective ||
         state == SystemSettingApplyState::applied ||
         state == SystemSettingApplyState::waiting_explorer_restart;
}

[[nodiscard]] std::string_view state_name(
    SystemSettingApplyState state) noexcept {
  switch (state) {
    case SystemSettingApplyState::not_selected:
      return "not-selected";
    case SystemSettingApplyState::already_effective:
      return "already-effective";
    case SystemSettingApplyState::applied:
      return "applied";
    case SystemSettingApplyState::waiting_explorer_restart:
      return "waiting-explorer-restart";
    case SystemSettingApplyState::not_applicable:
      return "not-applicable";
    case SystemSettingApplyState::force_confirmation_required:
      return "force-confirmation-required";
    case SystemSettingApplyState::blocked_by_dependency:
      return "blocked-by-dependency";
    case SystemSettingApplyState::failed:
      return "failed";
  }
  return "failed";
}

[[nodiscard]] bool same_value(WindowsSystemSettingValue const& left,
                              WindowsSystemSettingValue const& right) {
  return left == right;
}

}  // namespace

SystemSettingsApplyService::SystemSettingsApplyService(
    SystemSettingsCatalogSnapshotSource& catalog,
    SystemSettingsPlatformAdapter& platform,
    SystemSettingsRecoveryStore& recovery_store,
    SharedOperationOccupancy& occupancy, ExecutionLog& log) noexcept
    : catalog_(catalog),
      platform_(platform),
      recovery_store_(recovery_store),
      occupancy_(occupancy),
      log_(log) {
  auto stored = recovery_store_.read();
  if (stored.status == RecoveryStorageStatus::loaded) {
    recovery_records_ = std::move(stored.records);
    for (auto const& record : recovery_records_) {
      next_recovery_record_id_ =
          std::max(next_recovery_record_id_, record.record_id + 1);
    }
  }
  static_cast<void>(refresh());
}

SystemSettingsApplySnapshot SystemSettingsApplyService::snapshot() const {
  return snapshot_;
}

SystemSettingsApplySnapshot SystemSettingsApplyService::refresh() {
  auto previous = snapshot_;
  snapshot_ = {};
  snapshot_.recovery_records = recovery_records_;
  auto current = catalog_.current_settings_catalog();
  if (!current.has_value()) {
    snapshot_.status = SystemSettingsSnapshotStatus::unavailable;
    snapshot_.detail = "当前有效系统设置目录不可用";
    return snapshot_;
  }

  snapshot_.status = SystemSettingsSnapshotStatus::ready;
  snapshot_.catalog_revision = current->catalog.revision;
  for (auto const& plan : current->plans) {
    if (plan.enabled) {
      snapshot_.recommended_plan = plan.plan_id;
      break;
    }
  }
  auto const version = platform_.windows_version();
  for (auto const& setting : current->catalog.settings) {
    auto const kind = map_setting(setting);
    auto const applicable =
        version.has_value() && setting.known_windows_range.contains(*version);
    auto item = SystemSettingApplySnapshot{
        .id = setting.id,
        .display_name = setting.display_name,
        .description = setting.description,
        .source_url = setting.source_url,
        .known_windows_range = setting.known_windows_range,
        .selected = setting.default_selected,
        .applicable = applicable,
        .can_force_attempt =
            kind.has_value() &&
            setting.force_attempt_rule ==
                settings_domain::ForceAttemptRule::
                    allowed_with_explicit_confirmation &&
            setting.recovery_requirement ==
                settings_domain::RecoveryRequirement::restore_record_required,
        .recovery_available =
            setting.recovery_requirement ==
            settings_domain::RecoveryRequirement::restore_record_required,
        .restart_requirement = setting.restart_requirement,
        .state = applicable ? SystemSettingApplyState::not_selected
                            : SystemSettingApplyState::not_applicable,
        .detail = applicable ? "可执行" : "可能不适用",
    };
    auto const existing = std::ranges::find(
        previous.settings, setting.id, &SystemSettingApplySnapshot::id);
    if (existing != previous.settings.end()) {
      item.selected = existing->selected;
      item.state = existing->state;
      item.detail = existing->detail;
    }
    if (!kind.has_value()) {
      item.applicable = false;
      item.can_force_attempt = false;
      item.state = SystemSettingApplyState::failed;
      item.detail = "目录设置未映射到受控 Windows 能力";
    }
    snapshot_.settings.push_back(std::move(item));
  }
  if (previous.catalog_revision == snapshot_.catalog_revision) {
    snapshot_.selected_plan = std::move(previous.selected_plan);
    snapshot_.plan_name = std::move(previous.plan_name);
    snapshot_.plan_description = std::move(previous.plan_description);
    snapshot_.status = previous.status == SystemSettingsSnapshotStatus::applying
                           ? SystemSettingsSnapshotStatus::ready
                           : previous.status;
  }
  for (auto& item : snapshot_.settings) {
    auto const recovery = std::ranges::find_if(
        recovery_records_.rbegin(), recovery_records_.rend(),
        [&](SystemSettingsRecoveryRecord const& record) {
          return record.setting_id == item.id &&
                 record.status ==
                     RecoveryRecordStatus::waiting_explorer_restart;
        });
    if (recovery == recovery_records_.rend()) {
      continue;
    }
    auto const definition =
        settings_domain::find_setting(*current, item.id);
    auto const kind =
        definition == nullptr ? std::nullopt : map_setting(*definition);
    auto const expected =
        kind.has_value() ? target_value(*kind) : std::nullopt;
    auto observed =
        kind.has_value() ? platform_.read(*kind) : SystemSettingsRead{};
    item.selected = false;
    if (expected.has_value() &&
        observed.status == SystemSettingsAdapterStatus::succeeded &&
        observed.value.has_value() &&
        same_value(*observed.value, *expected)) {
      item.state = SystemSettingApplyState::waiting_explorer_restart;
      item.detail = "等待资源管理器重启后重新检测";
      snapshot_.waiting_for_explorer_restart = true;
    } else {
      item.state = SystemSettingApplyState::failed;
      item.detail = observed.detail.empty()
                        ? "重新打开后未检测到预期系统设置"
                        : observed.detail;
      snapshot_.status = SystemSettingsSnapshotStatus::failed;
    }
  }
  snapshot_.can_apply = std::ranges::any_of(
      snapshot_.settings, [](auto const& item) { return item.selected; });
  return snapshot_;
}

bool SystemSettingsApplyService::select_recommended_plan(
    settings_domain::StableId const& plan_id) {
  auto current = catalog_.current_settings_catalog();
  if (!current.has_value()) {
    return false;
  }
  if (current->catalog.revision != snapshot_.catalog_revision) {
    static_cast<void>(refresh());
    current = catalog_.current_settings_catalog();
    if (!current.has_value() ||
        current->catalog.revision != snapshot_.catalog_revision) {
      return false;
    }
  }
  auto const found = std::ranges::find(current->catalog.plans, plan_id,
                                       &settings_domain::OptimizationPlan::id);
  if (found == current->catalog.plans.end()) {
    return false;
  }
  auto const* availability =
      settings_domain::find_plan_availability(*current, plan_id);
  if (availability == nullptr || !availability->enabled) {
    return false;
  }
  for (auto& item : snapshot_.settings) {
    item.selected = false;
  }
  snapshot_.selected_plan = found->id;
  snapshot_.plan_name = found->display_name;
  snapshot_.plan_description = found->description;
  for (auto const& member : found->members) {
    if (auto* item = find_snapshot(member.setting_id); item != nullptr) {
      item->selected = member.default_selected;
    }
  }
  snapshot_.can_apply = std::ranges::any_of(
      snapshot_.settings, [](auto const& item) { return item.selected; });
  return true;
}

bool SystemSettingsApplyService::set_selected(
    settings_domain::StableId const& setting_id, bool selected) {
  auto* item = find_snapshot(setting_id);
  if (item == nullptr) {
    return false;
  }
  item->selected = selected;
  snapshot_.selected_plan.reset();
  snapshot_.can_apply = std::ranges::any_of(
      snapshot_.settings, [](auto const& candidate) { return candidate.selected; });
  return true;
}

SystemSettingsApplyResult SystemSettingsApplyService::apply_selected(
    SystemSettingsApplyRequest request) {
  SystemSettingsApplyResult result{
      .status = SystemSettingsSnapshotStatus::failed,
      .settings = snapshot_.settings,
  };
  if (snapshot_.status == SystemSettingsSnapshotStatus::unavailable ||
      snapshot_.catalog_revision == 0) {
    result.detail = "当前有效系统设置目录不可用";
    return result;
  }
  if (!snapshot_.can_apply) {
    result.status = SystemSettingsSnapshotStatus::completed;
    result.detail = "没有选择系统优化项";
    return result;
  }

  auto const correlation =
      CorrelationId{"system-settings-" + std::to_string(next_correlation_id_++)};
  auto const lease = occupancy_.try_acquire(
      OperationIdentity{.kind = "system-settings",
                        .operation_id = "apply",
                        .correlation_id = correlation.value});
  if (lease.code != OccupancyResultCode::acquired || !lease.lease.has_value()) {
    result.detail = "系统设置正在由另一项工作处理";
    return result;
  }

  snapshot_.status = SystemSettingsSnapshotStatus::applying;
  append_log(correlation, "apply-start", ExecutionResult::started, "",
             "冻结当前有效系统设置目录和选择");

  FrozenSelection frozen;
  frozen.catalog_revision = snapshot_.catalog_revision;
  auto current = catalog_.current_settings_catalog();
  if (!current.has_value() ||
      current->catalog.revision != frozen.catalog_revision) {
    result.detail = "系统设置目录在执行前发生变化";
    snapshot_.status = SystemSettingsSnapshotStatus::failed;
    append_log(correlation, "apply", ExecutionResult::failed, "",
               result.detail);
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }
  for (auto const& setting : current->catalog.settings) {
    auto const* item = find_snapshot(setting.id);
    if (item != nullptr && item->selected) {
      frozen.settings.push_back(setting);
    }
  }

  auto const platform_version = platform_.windows_version();
  auto dependency_ready = [&](settings_domain::SettingDefinition const& setting) {
    for (auto const& dependency : setting.depends_on) {
      auto const* item = find_snapshot(dependency);
      if (item == nullptr || !item->selected || !is_success(item->state)) {
        return false;
      }
    }
    return true;
  };

  std::vector<settings_domain::SettingDefinition> ordered;
  std::map<std::string, settings_domain::SettingDefinition const*> by_id;
  for (auto const& setting : frozen.settings) {
    by_id.emplace(setting.id.value, &setting);
  }
  std::set<std::string> visiting;
  std::set<std::string> visited;
  auto visit = [&](auto const& self,
                   settings_domain::SettingDefinition const& setting) -> void {
    if (visited.contains(setting.id.value)) {
      return;
    }
    if (!visiting.insert(setting.id.value).second) {
      return;
    }
    for (auto const& dependency : setting.depends_on) {
      auto const found = by_id.find(dependency.value);
      if (found != by_id.end()) {
        self(self, *found->second);
      }
    }
    visiting.erase(setting.id.value);
    visited.insert(setting.id.value);
    ordered.push_back(setting);
  };
  for (auto const& setting : frozen.settings) {
    visit(visit, setting);
  }
  bool waiting_restart = false;
  for (auto const& setting : ordered) {
    auto* item = find_snapshot(setting.id);
    if (item == nullptr) {
      continue;
    }
    if (!dependency_ready(setting)) {
      item->state = SystemSettingApplyState::blocked_by_dependency;
      item->detail = "依赖项未成功完成";
      continue;
    }

    auto const kind = map_setting(setting);
    auto const applicable =
        platform_version.has_value() &&
        setting.known_windows_range.contains(*platform_version);
    if (!applicable &&
        !(request.force_attempt_confirmed && item->can_force_attempt)) {
      item->state = item->can_force_attempt
                        ? SystemSettingApplyState::force_confirmation_required
                        : SystemSettingApplyState::not_applicable;
      item->detail = item->can_force_attempt
                         ? "高级视图需要确认后仍然尝试"
                         : "当前 Windows 版本可能不适用";
      continue;
    }
    if (!kind.has_value()) {
      item->state = SystemSettingApplyState::failed;
      item->detail = "目录设置未映射到受控 Windows 能力";
      continue;
    }

    auto const target = target_value(*kind);
    if (!target.has_value()) {
      item->state = SystemSettingApplyState::failed;
      item->detail = "受控 Windows 目标状态无效";
      continue;
    }
    auto observed = platform_.read(*kind);
    if (observed.status != SystemSettingsAdapterStatus::succeeded ||
        !observed.value.has_value()) {
      item->state = SystemSettingApplyState::failed;
      item->detail = observed.detail.empty() ? "读取当前状态失败" : observed.detail;
      continue;
    }
    if (same_value(*observed.value, *target)) {
      item->state = SystemSettingApplyState::already_effective;
      item->detail = "已生效，跳过重复修改";
      continue;
    }

    SystemSettingsRecoveryRecord record{
        .record_id = next_recovery_record_id_++,
        .setting_id = setting.id,
        .catalog_revision = frozen.catalog_revision,
        .setting = *kind,
        .original_value = *observed.value,
        .restart_requirement = setting.restart_requirement,
        .status = RecoveryRecordStatus::pending,
    };
    auto persisted = recovery_store_.save(record);
    if (persisted.status != RecoveryStorageStatus::committed) {
      item->state = SystemSettingApplyState::failed;
      item->detail = persisted.detail.empty() ? "原值保存失败" : persisted.detail;
      continue;
    }
    recovery_records_.push_back(record);
    snapshot_.recovery_records = recovery_records_;

    append_log(correlation, "read-and-save-original", ExecutionResult::succeeded,
               setting.id.value, "原值已保存");
    auto applied = platform_.apply(*kind);
    if (applied.status != SystemSettingsAdapterStatus::succeeded) {
      item->state = SystemSettingApplyState::failed;
      item->detail = applied.detail.empty() ? "应用设置失败" : applied.detail;
      append_log(correlation, "apply", ExecutionResult::failed, setting.id.value,
                 item->detail);
      continue;
    }
    auto verified = platform_.read(*kind);
    if (verified.status != SystemSettingsAdapterStatus::succeeded ||
        !verified.value.has_value() || !same_value(*verified.value, *target)) {
      item->state = SystemSettingApplyState::failed;
      item->detail = verified.detail.empty() ? "应用后验证失败" : verified.detail;
      append_log(correlation, "verify", ExecutionResult::failed, setting.id.value,
                 item->detail);
      continue;
    }

    record.status =
        setting.restart_requirement == settings_domain::RestartRequirement::explorer
            ? RecoveryRecordStatus::waiting_explorer_restart
            : RecoveryRecordStatus::applied;
    auto committed = recovery_store_.save(record);
    if (committed.status != RecoveryStorageStatus::committed) {
      item->state = SystemSettingApplyState::failed;
      item->detail = committed.detail.empty() ? "执行结果提交失败"
                                               : committed.detail;
      continue;
    }
    if (auto found = std::ranges::find(recovery_records_, record.record_id,
                                       &SystemSettingsRecoveryRecord::record_id);
        found != recovery_records_.end()) {
      *found = record;
    }
    if (setting.restart_requirement ==
        settings_domain::RestartRequirement::explorer) {
      item->state = SystemSettingApplyState::waiting_explorer_restart;
      item->detail = "等待资源管理器重启后重新检测";
      waiting_restart = true;
    } else {
      item->state = SystemSettingApplyState::applied;
      item->detail = "已应用并验证";
    }
    append_log(correlation, "verify-and-commit", ExecutionResult::succeeded,
               setting.id.value, std::string{state_name(item->state)});
  }

  if (waiting_restart &&
      request.explorer_restart_action ==
          SystemSettingsApplyRequest::ExplorerRestartAction::restart_now) {
    result.explorer_restart_attempted = true;
    auto restarted = platform_.restart_explorer();
    if (restarted.status == SystemSettingsAdapterStatus::succeeded) {
      for (auto& item : snapshot_.settings) {
        if (item.state != SystemSettingApplyState::waiting_explorer_restart) {
          continue;
        }
        auto const kind = [&]() -> std::optional<ControlledSystemSetting> {
          auto found = std::ranges::find(
              current->catalog.settings, item.id,
              &settings_domain::SettingDefinition::id);
          return found == current->catalog.settings.end() ? std::nullopt
                                                           : map_setting(*found);
        }();
        auto const expected =
            kind.has_value() ? target_value(*kind) : std::nullopt;
        auto observed =
            kind.has_value() ? platform_.read(*kind) : SystemSettingsRead{};
        if (expected.has_value() &&
            observed.status == SystemSettingsAdapterStatus::succeeded &&
            observed.value.has_value() &&
            same_value(*observed.value, *expected)) {
          std::string recovery_detail;
          if (mark_explorer_restart_verified(item.id, recovery_detail)) {
            item.state = SystemSettingApplyState::applied;
            item.detail = "已重启资源管理器并重新验证";
          } else {
            item.state = SystemSettingApplyState::failed;
            item.detail = std::move(recovery_detail);
          }
        } else {
          item.state = SystemSettingApplyState::failed;
          item.detail = observed.detail.empty() ? "资源管理器重启后验证失败"
                                                : observed.detail;
        }
      }
      waiting_restart = std::ranges::any_of(
          snapshot_.settings, [](auto const& item) {
            return item.state ==
                   SystemSettingApplyState::waiting_explorer_restart;
          });
    } else {
      result.detail = restarted.detail.empty() ? "资源管理器重启失败"
                                               : restarted.detail;
    }
  }

  snapshot_.waiting_for_explorer_restart = waiting_restart;
  snapshot_.status = std::ranges::any_of(
                         snapshot_.settings, [](auto const& item) {
                           return item.state == SystemSettingApplyState::failed;
                         })
                         ? SystemSettingsSnapshotStatus::failed
                         : SystemSettingsSnapshotStatus::completed;
  snapshot_.recovery_records = recovery_records_;
  result.status = snapshot_.status;
  result.settings = snapshot_.settings;
  result.waiting_for_explorer_restart = waiting_restart;
  if (result.detail.empty()) {
    result.detail = waiting_restart ? "系统优化已应用，等待资源管理器重启"
                                    : "系统优化已完成";
  }
  append_log(correlation, "apply-finished",
             result.status == SystemSettingsSnapshotStatus::completed
                 ? ExecutionResult::succeeded
                 : ExecutionResult::failed,
             "", result.detail);
  static_cast<void>(occupancy_.release(*lease.lease));
  return result;
}

SystemSettingsApplyResult SystemSettingsApplyService::restart_explorer_now() {
  SystemSettingsApplyResult result{
      .status = snapshot_.status,
      .settings = snapshot_.settings,
      .waiting_for_explorer_restart = snapshot_.waiting_for_explorer_restart,
  };
  if (!snapshot_.waiting_for_explorer_restart) {
    result.detail = "没有等待资源管理器重启的系统设置";
    return result;
  }

  auto const correlation = CorrelationId{
      "system-settings-restart-" + std::to_string(next_correlation_id_++)};
  auto const lease = occupancy_.try_acquire(
      OperationIdentity{.kind = "system-settings",
                        .operation_id = "restart-explorer",
                        .correlation_id = correlation.value});
  if (lease.code != OccupancyResultCode::acquired || !lease.lease.has_value()) {
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = "系统设置正在由另一项工作处理";
    return result;
  }

  result.explorer_restart_attempted = true;
  auto restarted = platform_.restart_explorer();
  if (restarted.status != SystemSettingsAdapterStatus::succeeded) {
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = restarted.detail.empty() ? "资源管理器重启失败"
                                             : restarted.detail;
    append_log(correlation, "restart-explorer", ExecutionResult::failed, "",
               result.detail);
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }

  auto current = catalog_.current_settings_catalog();
  if (!current.has_value() ||
      current->catalog.revision != snapshot_.catalog_revision) {
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = "系统设置目录在资源管理器重启前发生变化";
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }
  for (auto& item : snapshot_.settings) {
    if (item.state != SystemSettingApplyState::waiting_explorer_restart) {
      continue;
    }
    auto const definition = std::ranges::find(
        current->catalog.settings, item.id,
        &settings_domain::SettingDefinition::id);
    if (definition == current->catalog.settings.end()) {
      item.state = SystemSettingApplyState::failed;
      item.detail = "等待重启的设置已不在当前目录中";
      continue;
    }
    auto const kind = map_setting(*definition);
    auto const expected = kind.has_value() ? target_value(*kind) : std::nullopt;
    auto observed = kind.has_value() ? platform_.read(*kind) : SystemSettingsRead{};
    if (!expected.has_value() ||
        observed.status != SystemSettingsAdapterStatus::succeeded ||
        !observed.value.has_value() ||
        !same_value(*observed.value, *expected)) {
      item.state = SystemSettingApplyState::failed;
      item.detail = observed.detail.empty() ? "资源管理器重启后验证失败"
                                            : observed.detail;
      continue;
    }
    std::string recovery_detail;
    if (!mark_explorer_restart_verified(item.id, recovery_detail)) {
      item.state = SystemSettingApplyState::failed;
      item.detail = std::move(recovery_detail);
      continue;
    }
    item.state = SystemSettingApplyState::applied;
    item.detail = "已重启资源管理器并重新验证";
  }
  snapshot_.waiting_for_explorer_restart = std::ranges::any_of(
      snapshot_.settings, [](auto const& item) {
        return item.state == SystemSettingApplyState::waiting_explorer_restart;
      });
  snapshot_.status = std::ranges::any_of(
                         snapshot_.settings, [](auto const& item) {
                           return item.state == SystemSettingApplyState::failed;
                         })
                         ? SystemSettingsSnapshotStatus::failed
                         : SystemSettingsSnapshotStatus::completed;
  result.status = snapshot_.status;
  result.settings = snapshot_.settings;
  result.waiting_for_explorer_restart = snapshot_.waiting_for_explorer_restart;
  result.detail = result.waiting_for_explorer_restart
                      ? "等待资源管理器重启后的验证"
                      : "资源管理器重启后验证完成";
  append_log(correlation, "restart-explorer",
             result.status == SystemSettingsSnapshotStatus::completed
                 ? ExecutionResult::succeeded
                 : ExecutionResult::failed,
             "", result.detail);
  static_cast<void>(occupancy_.release(*lease.lease));
  return result;
}

std::vector<SystemSettingsRecoveryRecord>
SystemSettingsApplyService::recovery_records() const {
  return recovery_records_;
}

std::optional<ControlledSystemSetting> SystemSettingsApplyService::map_setting(
    settings_domain::SettingDefinition const& setting) const noexcept {
  if (setting.semantics.identity == kClassicContextMenuIdentity &&
      setting.semantics.apply_capability == kClassicContextMenuApply &&
      setting.semantics.detect_capability == kClassicContextMenuDetect &&
      setting.semantics.recover_capability == kClassicContextMenuRecover) {
    return ControlledSystemSetting::classic_context_menu;
  }
  if (setting.semantics.identity == kWindows10ExplorerIdentity &&
      setting.semantics.apply_capability == kWindows10ExplorerApply &&
      setting.semantics.detect_capability == kWindows10ExplorerDetect &&
      setting.semantics.recover_capability == kWindows10ExplorerRecover) {
    return ControlledSystemSetting::windows10_explorer;
  }
  return std::nullopt;
}

std::optional<WindowsSystemSettingValue>
SystemSettingsApplyService::target_value(ControlledSystemSetting setting) {
  switch (setting) {
    case ControlledSystemSetting::classic_context_menu:
      return WindowsSystemSettingValue{ClassicContextMenuMode::classic};
    case ControlledSystemSetting::windows10_explorer:
      return WindowsSystemSettingValue{ExplorerPresentationMode::windows10};
  }
  return std::nullopt;
}

SystemSettingApplySnapshot* SystemSettingsApplyService::find_snapshot(
    settings_domain::StableId const& id) noexcept {
  auto found = std::ranges::find(snapshot_.settings, id,
                                 &SystemSettingApplySnapshot::id);
  return found == snapshot_.settings.end() ? nullptr : &*found;
}

SystemSettingApplySnapshot const* SystemSettingsApplyService::find_snapshot(
    settings_domain::StableId const& id) const noexcept {
  auto found = std::ranges::find(snapshot_.settings, id,
                                 &SystemSettingApplySnapshot::id);
  return found == snapshot_.settings.end() ? nullptr : &*found;
}

bool SystemSettingsApplyService::mark_explorer_restart_verified(
    settings_domain::StableId const& setting_id, std::string& detail) {
  auto found = std::ranges::find_if(
      recovery_records_.rbegin(), recovery_records_.rend(),
      [&](SystemSettingsRecoveryRecord const& record) {
        return record.setting_id == setting_id &&
               record.status ==
                   RecoveryRecordStatus::waiting_explorer_restart;
      });
  if (found == recovery_records_.rend()) {
    detail = "等待资源管理器重启的恢复记录不可用";
    return false;
  }
  auto record = *found;
  record.status = RecoveryRecordStatus::applied;
  auto committed = recovery_store_.save(record);
  if (committed.status != RecoveryStorageStatus::committed) {
    detail = committed.detail.empty() ? "资源管理器重启验证提交失败"
                                      : committed.detail;
    return false;
  }
  *found = std::move(record);
  snapshot_.recovery_records = recovery_records_;
  return true;
}

void SystemSettingsApplyService::append_log(
    CorrelationId const& correlation, std::string stage, ExecutionResult result,
    std::string setting_id, std::string detail) {
  ExecutionEvent event{
      .kind = ExecutionEventKind::adapter_result,
      .component = "system-settings",
      .stage = std::move(stage),
      .result = result,
      .fields = {{"setting_id", std::move(setting_id),
                 DiagnosticValueDisposition::retain},
                {"detail", std::move(detail),
                 DiagnosticValueDisposition::retain}}};
  static_cast<void>(log_.append(correlation, event));
}

}  // namespace azzs::application
