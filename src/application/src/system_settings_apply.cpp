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

[[nodiscard]] SystemSettingsOperationStatus operation_status(
    SystemSettingApplyState state) noexcept {
  switch (state) {
    case SystemSettingApplyState::already_effective:
      return SystemSettingsOperationStatus::skipped;
    case SystemSettingApplyState::applied:
      return SystemSettingsOperationStatus::completed;
    case SystemSettingApplyState::waiting_explorer_restart:
      return SystemSettingsOperationStatus::waiting_explorer_restart;
    case SystemSettingApplyState::not_applicable:
      return SystemSettingsOperationStatus::not_applicable;
    case SystemSettingApplyState::force_confirmation_required:
      return SystemSettingsOperationStatus::confirmation_required;
    case SystemSettingApplyState::blocked_by_dependency:
      return SystemSettingsOperationStatus::blocked;
    case SystemSettingApplyState::not_selected:
    case SystemSettingApplyState::failed:
      return SystemSettingsOperationStatus::failed;
  }
  return SystemSettingsOperationStatus::failed;
}

[[nodiscard]] SystemSettingsOperationStatus operation_status(
    RecoveryRecordStatus status) noexcept {
  switch (status) {
    case RecoveryRecordStatus::applied:
      return SystemSettingsOperationStatus::completed;
    case RecoveryRecordStatus::restored:
      return SystemSettingsOperationStatus::restored;
    case RecoveryRecordStatus::waiting_explorer_restart:
      return SystemSettingsOperationStatus::waiting_explorer_restart;
    case RecoveryRecordStatus::pending:
    case RecoveryRecordStatus::restoring:
    case RecoveryRecordStatus::restore_failed:
      return SystemSettingsOperationStatus::failed;
  }
  return SystemSettingsOperationStatus::failed;
}

[[nodiscard]] SystemSettingsOperationStatus operation_status(
    SystemSettingsSnapshotStatus status) noexcept {
  return status == SystemSettingsSnapshotStatus::completed
             ? SystemSettingsOperationStatus::completed
             : SystemSettingsOperationStatus::failed;
}

[[nodiscard]] SystemSettingsWindowsEnvironmentFact windows_environment_fact(
    std::optional<SystemSettingsWindowsVersionFact> const& version) {
  if (!version.has_value()) {
    return {.availability = SystemSettingsFactAvailability::not_obtained,
            .display_version = "NOT_OBTAINED",
            .internal_build = 0,
            .reason =
                "NOT_OBTAINED: Windows environment was unavailable at operation"};
  }
  return {.availability = SystemSettingsFactAvailability::obtained,
          .display_version = version->display_version,
          .internal_build = version->internal_build,
          .reason = {}};
}

[[nodiscard]] SystemSettingsOperationFact legacy_operation_fact(
    SystemSettingsRecoveryRecord const& record) {
  auto const status = operation_status(record.status);
  constexpr std::string_view reason{
      "NOT_OBTAINED: legacy recovery record predates immutable operation history"};
  return {
      .operation = record.operation == RecoveryRecordOperation::restore
                       ? SystemSettingsOperationKind::restore
                       : record.operation == RecoveryRecordOperation::windows11_default
                             ? SystemSettingsOperationKind::windows11_default
                             : SystemSettingsOperationKind::apply,
      .catalog_availability = SystemSettingsFactAvailability::not_obtained,
      .catalog_identity = "NOT_OBTAINED",
      .catalog_revision = record.catalog_revision,
      .catalog_reason = std::string{reason},
      .windows_environment = {.availability =
                                  SystemSettingsFactAvailability::not_obtained,
                              .display_version = "NOT_OBTAINED",
                              .internal_build = 0,
                              .reason = std::string{reason}},
      .status = status,
      .reason = std::string{reason},
      .settings = {{.setting_id = record.setting_id,
                    .display_name = record.display_name,
                    .controlled_identity = "NOT_OBTAINED",
                    .catalog_revision = record.catalog_revision,
                    .declared_range_availability =
                        SystemSettingsFactAvailability::not_obtained,
                    .declared_range_reason = std::string{reason},
                    .original_value = record.original_value,
                    .recovery_record_id = record.record_id,
                    .restart_requirement = record.restart_requirement,
                    .status = status,
                    .reason = std::string{reason}}},
      .timeline = {{.ordinal = 1,
                    .stage = "legacy-recovery-record",
                    .status = status,
                    .reason = std::string{reason}}},
  };
}

[[nodiscard]] bool operation_fact_references_recovery_record(
    SystemSettingsOperationFact const& fact, std::uint64_t record_id) {
  return std::ranges::any_of(
      fact.settings, [record_id](SystemSettingsOperationSettingFact const& setting) {
        return setting.recovery_record_id == record_id;
      });
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
    operation_history_ = std::move(stored.operation_history);
    for (auto const& fact : operation_history_.facts) {
      next_operation_fact_id_ =
          std::max(next_operation_fact_id_, fact.fact_id + 1);
    }
    for (auto const& record : recovery_records_) {
      auto const already_recorded = std::ranges::any_of(
          operation_history_.facts, [&record](SystemSettingsOperationFact const& fact) {
            return operation_fact_references_recovery_record(fact,
                                                             record.record_id);
          });
      if (already_recorded) {
        continue;
      }
      auto fact = legacy_operation_fact(record);
      fact.fact_id = next_operation_fact_id_++;
      pending_legacy_operation_facts_.push_back(fact);
      operation_history_.facts.push_back(std::move(fact));
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
                settings_domain::RecoveryRequirement::restore_record_required &&
            (!version.has_value() ||
             version->generation != settings_domain::WindowsGeneration::windows_10),
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

  std::string legacy_history_detail;
  if (!persist_pending_legacy_operation_facts(legacy_history_detail)) {
    result.detail = std::move(legacy_history_detail);
    static_cast<void>(occupancy_.release(*lease.lease));
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

  auto const frozen_plan_id = snapshot_.selected_plan;
  auto const frozen_plan_name = snapshot_.plan_name;
  auto const platform_version = platform_.windows_version();
  auto const captured_windows_environment =
      windows_environment_fact(platform_.windows_version_fact());
  std::vector<SystemSettingsRecoveryRecord> operation_records;
  auto explorer_restart_result =
      SystemSettingsExplorerRestartResult::not_required;
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
    auto const force_attempt_allowed =
        item->can_force_attempt &&
        (!platform_version.has_value() ||
         platform_version->generation !=
             settings_domain::WindowsGeneration::windows_10);
    if (!applicable &&
        !(request.force_attempt_confirmed && force_attempt_allowed)) {
      item->state = force_attempt_allowed
                        ? SystemSettingApplyState::force_confirmation_required
                        : SystemSettingApplyState::not_applicable;
      item->detail = force_attempt_allowed
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
    operation_records.push_back(record);
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
      explorer_restart_result = SystemSettingsExplorerRestartResult::succeeded;
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
          explorer_restart_result =
              SystemSettingsExplorerRestartResult::verification_failed;
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
            explorer_restart_result =
                SystemSettingsExplorerRestartResult::verification_failed;
          }
        } else {
          item.state = SystemSettingApplyState::failed;
          item.detail = observed.detail.empty() ? "资源管理器重启后验证失败"
                                                : observed.detail;
          explorer_restart_result =
              SystemSettingsExplorerRestartResult::verification_failed;
        }
      }
      waiting_restart = std::ranges::any_of(
          snapshot_.settings, [](auto const& item) {
            return item.state ==
                   SystemSettingApplyState::waiting_explorer_restart;
          });
    } else {
      explorer_restart_result = SystemSettingsExplorerRestartResult::failed;
      result.detail = restarted.detail.empty() ? "资源管理器重启失败"
                                               : restarted.detail;
    }
  } else if (waiting_restart) {
    explorer_restart_result = SystemSettingsExplorerRestartResult::deferred;
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
  result.waiting_for_explorer_restart = waiting_restart;
  if (result.detail.empty()) {
    result.detail = waiting_restart ? "系统优化已应用，等待资源管理器重启"
                                    : "系统优化已完成";
  }
  for (auto& record : operation_records) {
    auto const updated = std::ranges::find(
        recovery_records_, record.record_id,
        &SystemSettingsRecoveryRecord::record_id);
    if (updated != recovery_records_.end()) {
      record = *updated;
    }
  }
  SystemSettingsOperationFact fact{
      .operation = request.target == SystemSettingsApplyRequest::Target::
                            windows11_default
                       ? SystemSettingsOperationKind::windows11_default
                       : SystemSettingsOperationKind::apply,
      .catalog_availability = SystemSettingsFactAvailability::obtained,
      .catalog_identity = "system-settings-catalog",
      .catalog_revision = frozen.catalog_revision,
      .catalog_reason = {},
      .selected_plan_id = frozen_plan_id,
      .selected_plan_name = frozen_plan_name,
      .windows_environment = captured_windows_environment,
      .explorer_restart_requested =
          request.explorer_restart_action ==
          SystemSettingsApplyRequest::ExplorerRestartAction::restart_now,
      .explorer_restart_result = explorer_restart_result,
      .windows_restart_barrier = std::ranges::any_of(
          frozen.settings, [](auto const& setting) {
            return setting.restart_requirement ==
                   settings_domain::RestartRequirement::windows;
          }),
      .status = result.status == SystemSettingsSnapshotStatus::failed
                    ? SystemSettingsOperationStatus::failed
                    : waiting_restart
                          ? SystemSettingsOperationStatus::waiting_explorer_restart
                          : SystemSettingsOperationStatus::completed,
      .reason = result.detail,
  };
  for (auto const& setting : frozen.settings) {
    auto const* item = find_snapshot(setting.id);
    auto const kind = map_setting(setting);
    auto const record = std::ranges::find(
        operation_records, setting.id,
        &SystemSettingsRecoveryRecord::setting_id);
    fact.settings.push_back({
        .setting_id = setting.id,
        .display_name = setting.display_name,
        .controlled_identity = setting.semantics.identity,
        .catalog_revision = frozen.catalog_revision,
        .declared_range_availability = SystemSettingsFactAvailability::obtained,
        .declared_windows_range = setting.known_windows_range,
        .declared_range_reason = {},
        .original_value = record == operation_records.end()
                              ? std::nullopt
                              : std::optional<WindowsSystemSettingValue>{
                                    record->original_value},
        .target_value = kind.has_value()
                            ? target_value(*kind, request.target)
                            : std::nullopt,
        .recovery_record_id = record == operation_records.end()
                                  ? std::nullopt
                                  : std::optional<std::uint64_t>{
                                        record->record_id},
        .restart_requirement = setting.restart_requirement,
        .force_attempt_confirmed = request.force_attempt_confirmed,
        .status = item == nullptr ? SystemSettingsOperationStatus::failed
                                  : operation_status(item->state),
        .reason = item == nullptr ? "冻结设置投影不可用" : item->detail,
    });
  }
  fact.timeline = {
      {.ordinal = 1,
       .stage = "selection-frozen",
       .status = SystemSettingsOperationStatus::completed,
       .reason = "已冻结所选设置、方案和目录修订"},
      {.ordinal = 2,
       .stage = "operation-finished",
       .status = fact.status,
       .reason = fact.reason},
  };
  std::string history_detail;
  if (!append_operation_fact(std::move(fact), history_detail)) {
    snapshot_.status = SystemSettingsSnapshotStatus::failed;
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = std::move(history_detail);
  }
  result.settings = snapshot_.settings;
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

  std::string legacy_history_detail;
  if (!persist_pending_legacy_operation_facts(legacy_history_detail)) {
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = std::move(legacy_history_detail);
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }

  std::vector<std::uint64_t> restarting_record_ids;
  for (auto const& record : recovery_records_) {
    if (record.status == RecoveryRecordStatus::waiting_explorer_restart) {
      restarting_record_ids.push_back(record.record_id);
    }
  }
  auto const captured_windows_environment =
      windows_environment_fact(platform_.windows_version_fact());
  auto append_restart_history =
      [&](SystemSettingsOperationStatus status,
          SystemSettingsExplorerRestartResult restart_result,
          std::string const& reason) {
        SystemSettingsOperationFact fact{
            .operation = SystemSettingsOperationKind::restart_explorer,
            .catalog_availability = SystemSettingsFactAvailability::not_obtained,
            .catalog_identity = "NOT_OBTAINED",
            .catalog_reason =
                "NOT_OBTAINED: Explorer restart uses frozen recovery records",
            .windows_environment = captured_windows_environment,
            .explorer_restart_requested = true,
            .explorer_restart_result = restart_result,
            .status = status,
            .reason = reason,
        };
        for (auto const record_id : restarting_record_ids) {
          auto const record = std::ranges::find(
              recovery_records_, record_id,
              &SystemSettingsRecoveryRecord::record_id);
          if (record == recovery_records_.end()) {
            continue;
          }
          fact.settings.push_back({
              .setting_id = record->setting_id,
              .display_name = record->display_name,
              .controlled_identity = "NOT_OBTAINED",
              .catalog_revision = record->catalog_revision,
              .declared_range_availability =
                  SystemSettingsFactAvailability::not_obtained,
              .declared_range_reason =
                  "NOT_OBTAINED: recovery record does not contain declared Windows range",
              .original_value = record->original_value,
              .target_value = expected_value_for_recovery(*record),
              .recovery_record_id = record->record_id,
              .restart_requirement = record->restart_requirement,
              .status = operation_status(record->status),
              .reason = reason,
          });
        }
        fact.timeline = {
            {.ordinal = 1,
             .stage = "explorer-restart-requested",
             .status = SystemSettingsOperationStatus::completed,
             .reason = "已按冻结恢复记录请求资源管理器重启"},
            {.ordinal = 2,
             .stage = "explorer-restart-finished",
             .status = status,
             .reason = reason},
        };
        std::string history_detail;
        return append_operation_fact(std::move(fact), history_detail)
                   ? std::optional<std::string>{}
                   : std::optional<std::string>{std::move(history_detail)};
      };

  result.explorer_restart_attempted = true;
  auto restarted = platform_.restart_explorer();
  if (restarted.status != SystemSettingsAdapterStatus::succeeded) {
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = restarted.detail.empty() ? "资源管理器重启失败"
                                             : restarted.detail;
    if (auto history_detail = append_restart_history(
            SystemSettingsOperationStatus::failed,
            SystemSettingsExplorerRestartResult::failed, result.detail);
        history_detail.has_value()) {
      result.detail = std::move(*history_detail);
    }
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
  if (auto history_detail = append_restart_history(
          result.status == SystemSettingsSnapshotStatus::completed
              ? SystemSettingsOperationStatus::completed
              : SystemSettingsOperationStatus::failed,
          verification_failed
              ? SystemSettingsExplorerRestartResult::verification_failed
              : SystemSettingsExplorerRestartResult::succeeded,
          result.detail);
      history_detail.has_value()) {
    result.status = SystemSettingsSnapshotStatus::failed;
    result.detail = std::move(*history_detail);
  }
  result.settings = snapshot_.settings;
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

SystemSettingsOperationHistory SystemSettingsApplyService::operation_history()
    const {
  return operation_history_;
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

  std::string legacy_history_detail;
  if (!persist_pending_legacy_operation_facts(legacy_history_detail)) {
    result.status = SystemSettingsUndoStatus::failed;
    result.detail = std::move(legacy_history_detail);
    static_cast<void>(occupancy_.release(*lease.lease));
    return result;
  }

  auto const frozen_recovery = *record;
  auto const captured_windows_environment =
      windows_environment_fact(platform_.windows_version_fact());
  auto append_undo_history = [&](SystemSettingsOperationStatus status,
                                 std::string const& reason) {
    SystemSettingsOperationFact fact{
        .operation = SystemSettingsOperationKind::restore,
        .catalog_availability = SystemSettingsFactAvailability::not_obtained,
        .catalog_identity = "NOT_OBTAINED",
        .catalog_revision = frozen_recovery.catalog_revision,
        .catalog_reason =
            "NOT_OBTAINED: restore uses the frozen recovery record, not the current catalog",
        .windows_environment = captured_windows_environment,
        .explorer_restart_requested =
            request.explorer_restart_action ==
            SystemSettingsApplyRequest::ExplorerRestartAction::restart_now,
        .explorer_restart_result =
            status == SystemSettingsOperationStatus::waiting_explorer_restart
                ? SystemSettingsExplorerRestartResult::deferred
                : SystemSettingsExplorerRestartResult::not_required,
        .status = status,
        .reason = reason,
        .settings = {{
            .setting_id = frozen_recovery.setting_id,
            .display_name = frozen_recovery.display_name,
            .controlled_identity = "NOT_OBTAINED",
            .catalog_revision = frozen_recovery.catalog_revision,
            .declared_range_availability =
                SystemSettingsFactAvailability::not_obtained,
            .declared_range_reason =
                "NOT_OBTAINED: recovery record does not contain declared Windows range",
            .original_value = frozen_recovery.original_value,
            .target_value = frozen_recovery.original_value,
            .recovery_record_id = frozen_recovery.record_id,
            .restart_requirement = frozen_recovery.restart_requirement,
            .status = status,
            .reason = reason,
        }},
        .timeline = {{
            .ordinal = 1,
            .stage = "restore-frozen-recovery",
            .status = SystemSettingsOperationStatus::completed,
            .reason = "已冻结原值和恢复记录"},
                     {.ordinal = 2,
                      .stage = "restore-finished",
                      .status = status,
                      .reason = reason}},
    };
    std::string history_detail;
    return append_operation_fact(std::move(fact), history_detail)
               ? std::optional<std::string>{}
               : std::optional<std::string>{std::move(history_detail)};
  };

  auto current = *record;
  current.operation = RecoveryRecordOperation::restore;
  current.status = RecoveryRecordStatus::restoring;
  if (auto saved = recovery_store_.save(current);
      saved.status != RecoveryStorageStatus::committed) {
    result.detail = saved.detail.empty() ? "撤销状态保存失败" : saved.detail;
    if (auto history_detail = append_undo_history(
            SystemSettingsOperationStatus::failed, result.detail);
        history_detail.has_value()) {
      result.detail = std::move(*history_detail);
    }
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
    if (auto history_detail = append_undo_history(
            SystemSettingsOperationStatus::failed, result.detail);
        history_detail.has_value()) {
      result.detail = std::move(*history_detail);
    }
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
    if (auto history_detail = append_undo_history(
            SystemSettingsOperationStatus::failed, result.detail);
        history_detail.has_value()) {
      result.detail = std::move(*history_detail);
    }
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
    if (auto history_detail = append_undo_history(
            SystemSettingsOperationStatus::failed, result.detail);
        history_detail.has_value()) {
      result.detail = std::move(*history_detail);
    }
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
    if (auto history_detail = append_undo_history(
            SystemSettingsOperationStatus::waiting_explorer_restart,
            result.detail);
        history_detail.has_value()) {
      result.status = SystemSettingsUndoStatus::failed;
      result.detail = std::move(*history_detail);
      static_cast<void>(occupancy_.release(*lease.lease));
      return result;
    }
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
    if (auto history_detail = append_undo_history(
            SystemSettingsOperationStatus::restored, result.detail);
        history_detail.has_value()) {
      result.status = SystemSettingsUndoStatus::failed;
      result.detail = std::move(*history_detail);
    }
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
  std::string legacy_history_detail;
  if (!persist_pending_legacy_operation_facts(legacy_history_detail)) {
    result.detail = std::move(legacy_history_detail);
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

bool SystemSettingsApplyService::append_operation_fact(
    SystemSettingsOperationFact fact, std::string& detail) {
  fact.fact_id = next_operation_fact_id_++;
  auto committed = recovery_store_.append_operation_fact(fact);
  if (committed.status != RecoveryStorageStatus::committed) {
    detail = committed.detail.empty() ? "系统设置操作历史提交失败"
                                      : committed.detail;
    return false;
  }
  operation_history_.facts.push_back(std::move(fact));
  return true;
}

bool SystemSettingsApplyService::persist_pending_legacy_operation_facts(
    std::string& detail) {
  auto pending = pending_legacy_operation_facts_.begin();
  while (pending != pending_legacy_operation_facts_.end()) {
    auto committed = recovery_store_.append_operation_fact(*pending);
    if (committed.status != RecoveryStorageStatus::committed) {
      detail = committed.detail.empty()
                   ? "旧系统设置恢复记录历史迁移失败"
                   : committed.detail;
      return false;
    }
    pending = pending_legacy_operation_facts_.erase(pending);
  }
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
