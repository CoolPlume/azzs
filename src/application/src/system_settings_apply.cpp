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
  auto project_waiting_recovery = [&] {
    for (auto const& record : recovery_records_) {
      if (record.status != RecoveryRecordStatus::waiting_explorer_restart) {
        continue;
      }
      snapshot_.waiting_for_explorer_restart = true;
      auto const expected = expected_value_for_recovery(record);
      auto observed = platform_.read(record.setting);
      auto* item = find_snapshot(record.setting_id);
      if (expected.has_value() &&
          observed.status == SystemSettingsAdapterStatus::succeeded &&
          observed.value.has_value() && same_value(*observed.value, *expected)) {
        if (item != nullptr) {
          item->selected = false;
          item->state = SystemSettingApplyState::waiting_explorer_restart;
          item->detail = record.operation == RecoveryRecordOperation::restore
                             ? "撤销后等待资源管理器重启并重新检测"
                             : "等待资源管理器重启后重新检测";
        }
        continue;
      }
      if (item != nullptr) {
        item->selected = false;
        item->state = SystemSettingApplyState::failed;
        item->detail = observed.detail.empty()
                           ? "重新打开后未检测到预期系统设置"
                           : observed.detail;
      }
      if (snapshot_.status != SystemSettingsSnapshotStatus::unavailable) {
        snapshot_.status = SystemSettingsSnapshotStatus::failed;
      }
    }
  };
  auto current = catalog_.current_settings_catalog();
  if (!current.has_value()) {
    snapshot_.status = SystemSettingsSnapshotStatus::unavailable;
    snapshot_.detail = "当前有效系统设置目录不可用";
    project_waiting_recovery();
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
  project_waiting_recovery();
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
    if (request.target == SystemSettingsApplyRequest::Target::catalog_target &&
        !dependency_ready(setting)) {
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

    auto const target = target_value(*kind, request.target);
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
        .display_name = setting.display_name,
        .catalog_revision = frozen.catalog_revision,
        .setting = *kind,
        .original_value = *observed.value,
        .restart_requirement = setting.restart_requirement,
        .status = RecoveryRecordStatus::pending,
        .operation = request.target == SystemSettingsApplyRequest::Target::
                                      windows11_default
                         ? RecoveryRecordOperation::windows11_default
                         : RecoveryRecordOperation::apply,
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
    auto applied = request.target == SystemSettingsApplyRequest::Target::
                       windows11_default
                       ? platform_.restore(*kind, *target)
                       : platform_.apply(*kind);
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
        auto const record = std::ranges::find_if(
            recovery_records_.rbegin(), recovery_records_.rend(),
            [&](SystemSettingsRecoveryRecord const& candidate) {
              return candidate.setting_id == item.id &&
                     candidate.status ==
                         RecoveryRecordStatus::waiting_explorer_restart;
            });
        if (record == recovery_records_.rend()) {
          item.state = SystemSettingApplyState::failed;
          item.detail = "等待重启的恢复记录不可用";
          continue;
        }
        auto const expected = expected_value_for_recovery(*record);
        auto observed =
            platform_.read(record->setting);
        if (expected.has_value() &&
            observed.status == SystemSettingsAdapterStatus::succeeded &&
            observed.value.has_value() &&
            same_value(*observed.value, *expected)) {
          std::string recovery_detail;
          if (mark_explorer_restart_verified(record->record_id,
                                             recovery_detail)) {
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

  bool verification_failed = false;
  for (auto& record : recovery_records_) {
    if (record.status != RecoveryRecordStatus::waiting_explorer_restart) {
      continue;
    }
    auto const expected = expected_value_for_recovery(record);
    auto observed = platform_.read(record.setting);
    if (!expected.has_value() ||
        observed.status != SystemSettingsAdapterStatus::succeeded ||
        !observed.value.has_value() ||
        !same_value(*observed.value, *expected)) {
      verification_failed = true;
      if (auto* item = find_snapshot(record.setting_id); item != nullptr) {
        item->state = SystemSettingApplyState::failed;
        item->detail = observed.detail.empty() ? "资源管理器重启后验证失败"
                                              : observed.detail;
      }
      continue;
    }
    std::string recovery_detail;
    if (!mark_explorer_restart_verified(record.record_id, recovery_detail)) {
      verification_failed = true;
      if (auto* item = find_snapshot(record.setting_id); item != nullptr) {
        item->state = SystemSettingApplyState::failed;
        item->detail = std::move(recovery_detail);
      }
      continue;
    }
    if (auto* item = find_snapshot(record.setting_id); item != nullptr) {
      item->state = SystemSettingApplyState::applied;
      item->detail = "已重启资源管理器并重新验证";
    }
  }
  snapshot_.waiting_for_explorer_restart = std::ranges::any_of(
      recovery_records_, [](auto const& record) {
        return record.status == RecoveryRecordStatus::waiting_explorer_restart;
      });
  snapshot_.status = verification_failed || std::ranges::any_of(
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

SystemSettingsApplyResult SystemSettingsApplyService::restore_windows11_default(
    settings_domain::StableId const& setting_id,
    SystemSettingsApplyRequest::ExplorerRestartAction restart_action) {
  auto* item = find_snapshot(setting_id);
  if (item == nullptr) {
    return {.status = SystemSettingsSnapshotStatus::failed,
            .settings = snapshot_.settings,
            .detail = "当前设置目录中不存在该受控系统设置"};
  }
  auto const selected_plan = snapshot_.selected_plan;
  auto const plan_name = snapshot_.plan_name;
  auto const plan_description = snapshot_.plan_description;
  std::vector<settings_domain::StableId> selected;
  for (auto const& candidate : snapshot_.settings) {
    if (candidate.selected) {
      selected.push_back(candidate.id);
    }
  }
  for (auto& candidate : snapshot_.settings) {
    candidate.selected = false;
  }
  item->selected = true;
  snapshot_.selected_plan.reset();
  snapshot_.can_apply = true;
  auto result = apply_selected(
      {.target = SystemSettingsApplyRequest::Target::windows11_default,
       .explorer_restart_action = restart_action});
  for (auto& candidate : snapshot_.settings) {
    candidate.selected = std::ranges::find(selected, candidate.id) !=
                         selected.end();
  }
  snapshot_.selected_plan = selected_plan;
  snapshot_.plan_name = plan_name;
  snapshot_.plan_description = plan_description;
  snapshot_.can_apply = std::ranges::any_of(
      snapshot_.settings, [](auto const& candidate) { return candidate.selected; });
  result.settings = snapshot_.settings;
  return result;
}

std::vector<SystemSettingsRecoveryRecord>
SystemSettingsApplyService::recovery_records() const {
  return recovery_records_;
}

SystemSettingsUndoResult SystemSettingsApplyService::undo(
    settings_domain::StableId const& setting_id,
    SystemSettingsUndoRequest request) {
  SystemSettingsUndoResult result;
  auto record = std::ranges::find_if(
      recovery_records_.rbegin(), recovery_records_.rend(),
      [&](SystemSettingsRecoveryRecord const& candidate) {
        return candidate.setting_id == setting_id &&
               candidate.status != RecoveryRecordStatus::restored;
      });
  if (record == recovery_records_.rend()) {
    result.status = SystemSettingsUndoStatus::no_record;
    result.detail = "没有可撤销的恢复记录";
    return result;
  }
  result.record_id = record->record_id;
  if (record->status == RecoveryRecordStatus::waiting_explorer_restart) {
    result.status = SystemSettingsUndoStatus::confirmation_required;
    result.detail = "该设置正在等待资源管理器重启验证";
    return result;
  }

  auto const correlation = CorrelationId{
      "system-settings-undo-" + std::to_string(next_correlation_id_++)};
  auto const lease = occupancy_.try_acquire(
      OperationIdentity{.kind = "system-settings",
                        .operation_id = "undo",
                        .correlation_id = correlation.value});
  if (lease.code != OccupancyResultCode::acquired || !lease.lease.has_value()) {
    result.detail = "系统设置正在由另一项工作处理";
    return result;
  }

  auto current = *record;
  current.operation = RecoveryRecordOperation::restore;
  current.status = RecoveryRecordStatus::restoring;
  if (auto saved = recovery_store_.save(current);
      saved.status != RecoveryStorageStatus::committed) {
    result.detail = saved.detail.empty() ? "撤销状态保存失败" : saved.detail;
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }
  *record = current;
  snapshot_.recovery_records = recovery_records_;
  append_log(correlation, "undo-start", ExecutionResult::started,
             current.setting_id.value, "使用已冻结的原值和恢复记录");

  auto restored = platform_.restore(current.setting, current.original_value);
  if (restored.status != SystemSettingsAdapterStatus::succeeded) {
    current.status = RecoveryRecordStatus::restore_failed;
    static_cast<void>(recovery_store_.save(current));
    *record = current;
    snapshot_.recovery_records = recovery_records_;
    result.detail = restored.detail.empty() ? "恢复原值失败" : restored.detail;
    append_log(correlation, "undo-restore", ExecutionResult::failed,
               current.setting_id.value, result.detail);
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }
  auto verified = platform_.read(current.setting);
  if (verified.status != SystemSettingsAdapterStatus::succeeded ||
      !verified.value.has_value() ||
      !same_value(*verified.value, current.original_value)) {
    current.status = RecoveryRecordStatus::restore_failed;
    static_cast<void>(recovery_store_.save(current));
    *record = current;
    snapshot_.recovery_records = recovery_records_;
    result.detail = verified.detail.empty() ? "恢复原值后验证失败"
                                             : verified.detail;
    append_log(correlation, "undo-verify", ExecutionResult::failed,
               current.setting_id.value, result.detail);
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }

  current.status = current.restart_requirement ==
                           settings_domain::RestartRequirement::explorer
                       ? RecoveryRecordStatus::waiting_explorer_restart
                       : RecoveryRecordStatus::restored;
  if (auto committed = recovery_store_.save(current);
      committed.status != RecoveryStorageStatus::committed) {
    current.status = RecoveryRecordStatus::restore_failed;
    static_cast<void>(recovery_store_.save(current));
    *record = current;
    snapshot_.recovery_records = recovery_records_;
    result.detail = committed.detail.empty() ? "撤销结果提交失败"
                                               : committed.detail;
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }
  *record = current;
  snapshot_.recovery_records = recovery_records_;
  snapshot_.waiting_for_explorer_restart = std::ranges::any_of(
      recovery_records_, [](auto const& candidate) {
        return candidate.status == RecoveryRecordStatus::waiting_explorer_restart;
      });
  if (current.status == RecoveryRecordStatus::waiting_explorer_restart) {
    result.status = SystemSettingsUndoStatus::waiting_explorer_restart;
    result.detail = "原值已恢复，等待资源管理器重启后重新验证";
    if (request.explorer_restart_action ==
        SystemSettingsApplyRequest::ExplorerRestartAction::restart_now) {
      result.explorer_restart_attempted = true;
      static_cast<void>(occupancy_.release(*lease.lease));
      auto restarted = restart_explorer_now();
      if (restarted.status == SystemSettingsSnapshotStatus::failed) {
        result.status = SystemSettingsUndoStatus::failed;
        result.detail = restarted.detail;
      } else if (auto const* updated = find_recovery_record(current.record_id);
                 updated != nullptr &&
                 updated->status == RecoveryRecordStatus::restored) {
        result.status = SystemSettingsUndoStatus::restored;
        result.detail = "已重启资源管理器并验证恢复原值";
      }
      append_log(correlation, "undo-finished",
                 result.status == SystemSettingsUndoStatus::failed
                     ? ExecutionResult::failed
                     : ExecutionResult::succeeded,
                 current.setting_id.value, result.detail);
      return result;
    }
  } else {
    result.status = SystemSettingsUndoStatus::restored;
    result.detail = "已恢复保存的原值并验证";
  }
  append_log(correlation, "undo-finished",
             result.status == SystemSettingsUndoStatus::failed
                 ? ExecutionResult::failed
                 : ExecutionResult::succeeded,
             current.setting_id.value, result.detail);
  static_cast<void>(occupancy_.release(*lease.lease));
  return result;
}

RecoveryRecordDeleteResult SystemSettingsApplyService::delete_recovery_record(
    std::uint64_t record_id, bool confirmed) {
  RecoveryRecordDeleteResult result;
  auto const* record = find_recovery_record(record_id);
  if (record == nullptr) {
    result.status = RecoveryRecordDeleteStatus::not_found;
    result.detail = "恢复记录不存在";
    return result;
  }
  if (std::ranges::any_of(recovery_records_, [&](auto const& candidate) {
        return recovery_is_protected(candidate.status);
      })) {
    result.status = RecoveryRecordDeleteStatus::blocked;
    result.detail = "等待重启或待恢复流程期间不能删除恢复记录";
    return result;
  }
  if (!confirmed) {
    result.status = RecoveryRecordDeleteStatus::confirmation_required;
    result.detail = "删除后将无法恢复“" + record->display_name + "”的保存原值";
    return result;
  }
  auto committed = recovery_store_.erase(record_id);
  if (committed.status != RecoveryStorageStatus::committed) {
    result.detail = committed.detail.empty() ? "恢复记录删除失败" : committed.detail;
    return result;
  }
  recovery_records_.erase(std::ranges::find(
      recovery_records_, record_id, &SystemSettingsRecoveryRecord::record_id));
  snapshot_.recovery_records = recovery_records_;
  result.status = RecoveryRecordDeleteStatus::deleted;
  result.detail = "恢复记录已删除";
  return result;
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
SystemSettingsApplyService::target_value(
    ControlledSystemSetting setting, SystemSettingsApplyRequest::Target target) {
  if (target == SystemSettingsApplyRequest::Target::windows11_default) {
    switch (setting) {
      case ControlledSystemSetting::classic_context_menu:
        return WindowsSystemSettingValue{ClassicContextMenuMode::windows11};
      case ControlledSystemSetting::windows10_explorer:
        return WindowsSystemSettingValue{ExplorerPresentationMode::windows11};
    }
  }
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

SystemSettingsRecoveryRecord* SystemSettingsApplyService::find_recovery_record(
    std::uint64_t record_id) noexcept {
  auto found = std::ranges::find(recovery_records_, record_id,
                                 &SystemSettingsRecoveryRecord::record_id);
  return found == recovery_records_.end() ? nullptr : &*found;
}

SystemSettingsRecoveryRecord const*
SystemSettingsApplyService::find_recovery_record(
    std::uint64_t record_id) const noexcept {
  auto found = std::ranges::find(recovery_records_, record_id,
                                 &SystemSettingsRecoveryRecord::record_id);
  return found == recovery_records_.end() ? nullptr : &*found;
}

std::optional<WindowsSystemSettingValue>
SystemSettingsApplyService::expected_value_for_recovery(
    SystemSettingsRecoveryRecord const& record) const {
  return record.operation == RecoveryRecordOperation::restore
             ? std::optional<WindowsSystemSettingValue>{record.original_value}
             : target_value(record.setting,
                            record.operation ==
                                    RecoveryRecordOperation::windows11_default
                                ? SystemSettingsApplyRequest::Target::
                                      windows11_default
                                : SystemSettingsApplyRequest::Target::
                                      catalog_target);
}

bool SystemSettingsApplyService::recovery_is_protected(
    RecoveryRecordStatus status) const noexcept {
  return status == RecoveryRecordStatus::pending ||
         status == RecoveryRecordStatus::restoring ||
         status == RecoveryRecordStatus::waiting_explorer_restart;
}

bool SystemSettingsApplyService::mark_explorer_restart_verified(
    std::uint64_t record_id, std::string& detail) {
  auto* found = find_recovery_record(record_id);
  if (found == nullptr ||
      found->status != RecoveryRecordStatus::waiting_explorer_restart) {
    detail = "等待资源管理器重启的恢复记录不可用";
    return false;
  }
  auto record = *found;
  record.status = record.operation == RecoveryRecordOperation::restore
                      ? RecoveryRecordStatus::restored
                      : RecoveryRecordStatus::applied;
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
