#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "azzs/application/execution_log.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/settings_catalog/settings_catalog.hpp"

namespace azzs::application {

namespace settings_domain = domain::settings_catalog;

enum class ControlledSystemSetting : std::uint8_t {
  classic_context_menu,
  windows10_explorer,
};

enum class ClassicContextMenuMode : std::uint8_t {
  windows11,
  classic,
};

enum class ExplorerPresentationMode : std::uint8_t {
  windows11,
  windows10,
};

using WindowsSystemSettingValue =
    std::variant<ClassicContextMenuMode, ExplorerPresentationMode>;

enum class SystemSettingsAdapterStatus : std::uint8_t {
  succeeded,
  unsupported,
  read_failed,
  apply_failed,
  verify_failed,
  restore_failed,
  restart_failed,
};

struct SystemSettingsRead final {
  SystemSettingsAdapterStatus status{SystemSettingsAdapterStatus::read_failed};
  std::optional<WindowsSystemSettingValue> value;
  std::string detail;
};

struct SystemSettingsAdapterResult final {
  SystemSettingsAdapterStatus status{SystemSettingsAdapterStatus::apply_failed};
  std::string detail;
};

class SystemSettingsPlatformAdapter {
 public:
  virtual ~SystemSettingsPlatformAdapter() = default;

  [[nodiscard]] virtual std::optional<settings_domain::WindowsVersion>
  windows_version() const = 0;
  [[nodiscard]] virtual SystemSettingsRead read(
      ControlledSystemSetting setting) = 0;
  [[nodiscard]] virtual SystemSettingsAdapterResult apply(
      ControlledSystemSetting setting) = 0;
  [[nodiscard]] virtual SystemSettingsAdapterResult restore(
      ControlledSystemSetting setting, WindowsSystemSettingValue value) = 0;
  [[nodiscard]] virtual SystemSettingsAdapterResult restart_explorer() = 0;
};

enum class RecoveryRecordStatus : std::uint8_t {
  pending = 0,
  applied = 1,
  restored = 2,
  waiting_explorer_restart = 3,
};

struct SystemSettingsRecoveryRecord final {
  std::uint64_t record_id{0};
  settings_domain::StableId setting_id;
  std::uint64_t catalog_revision{0};
  ControlledSystemSetting setting{ControlledSystemSetting::classic_context_menu};
  WindowsSystemSettingValue original_value{ClassicContextMenuMode::windows11};
  settings_domain::RestartRequirement restart_requirement{
      settings_domain::RestartRequirement::none};
  RecoveryRecordStatus status{RecoveryRecordStatus::pending};

  friend bool operator==(SystemSettingsRecoveryRecord const&,
                         SystemSettingsRecoveryRecord const&) = default;
};

enum class RecoveryStorageStatus : std::uint8_t {
  loaded,
  committed,
  failed,
};

struct RecoveryStorageRead final {
  RecoveryStorageStatus status{RecoveryStorageStatus::failed};
  std::vector<SystemSettingsRecoveryRecord> records;
  std::string detail;
};

struct RecoveryStorageWrite final {
  RecoveryStorageStatus status{RecoveryStorageStatus::failed};
  std::string detail;
};

class SystemSettingsRecoveryStore {
 public:
  virtual ~SystemSettingsRecoveryStore() = default;

  [[nodiscard]] virtual RecoveryStorageRead read() = 0;
  [[nodiscard]] virtual RecoveryStorageWrite save(
      SystemSettingsRecoveryRecord record) = 0;
};

class SystemSettingsCatalogSnapshotSource {
 public:
  virtual ~SystemSettingsCatalogSnapshotSource() = default;

  [[nodiscard]] virtual std::optional<
      settings_domain::ValidatedSettingsCatalog>
  current_settings_catalog() = 0;
};

enum class SystemSettingApplyState : std::uint8_t {
  not_selected,
  already_effective,
  applied,
  waiting_explorer_restart,
  not_applicable,
  force_confirmation_required,
  blocked_by_dependency,
  failed,
};

struct SystemSettingApplySnapshot final {
  settings_domain::StableId id;
  std::string display_name;
  std::string description;
  std::optional<std::string> source_url;
  settings_domain::WindowsVersionRange known_windows_range;
  bool selected{false};
  bool applicable{false};
  bool can_force_attempt{false};
  bool recovery_available{false};
  settings_domain::RestartRequirement restart_requirement{
      settings_domain::RestartRequirement::none};
  SystemSettingApplyState state{SystemSettingApplyState::not_selected};
  std::string detail;
};

enum class SystemSettingsSnapshotStatus : std::uint8_t {
  unavailable,
  ready,
  applying,
  completed,
  failed,
};

struct SystemSettingsApplySnapshot final {
  SystemSettingsSnapshotStatus status{SystemSettingsSnapshotStatus::unavailable};
  std::uint64_t catalog_revision{0};
  std::optional<settings_domain::StableId> recommended_plan;
  std::optional<settings_domain::StableId> selected_plan;
  std::string plan_name;
  std::string plan_description;
  std::vector<SystemSettingApplySnapshot> settings;
  std::vector<SystemSettingsRecoveryRecord> recovery_records;
  bool waiting_for_explorer_restart{false};
  bool can_apply{false};
  std::string detail;
};

struct SystemSettingsApplyRequest final {
  bool force_attempt_confirmed{false};
  enum class ExplorerRestartAction : std::uint8_t {
    defer,
    restart_now,
  } explorer_restart_action{ExplorerRestartAction::defer};
};

struct SystemSettingsApplyResult final {
  SystemSettingsSnapshotStatus status{SystemSettingsSnapshotStatus::failed};
  std::vector<SystemSettingApplySnapshot> settings;
  bool explorer_restart_attempted{false};
  bool waiting_for_explorer_restart{false};
  std::string detail;
};

class SystemSettingsApplyService final {
 public:
  SystemSettingsApplyService(
      SystemSettingsCatalogSnapshotSource& catalog,
      SystemSettingsPlatformAdapter& platform,
      SystemSettingsRecoveryStore& recovery_store,
      SharedOperationOccupancy& occupancy, ExecutionLog& log) noexcept;

  [[nodiscard]] SystemSettingsApplySnapshot snapshot() const;
  [[nodiscard]] SystemSettingsApplySnapshot refresh();
  [[nodiscard]] bool select_recommended_plan(
      settings_domain::StableId const& plan_id);
  [[nodiscard]] bool set_selected(settings_domain::StableId const& setting_id,
                                  bool selected);
  [[nodiscard]] SystemSettingsApplyResult apply_selected(
      SystemSettingsApplyRequest request = {});
  [[nodiscard]] SystemSettingsApplyResult restart_explorer_now();

  // This is the read-only recovery contract consumed by the undo workflow.
  [[nodiscard]] std::vector<SystemSettingsRecoveryRecord> recovery_records()
      const;

 private:
  struct FrozenSelection final {
    std::uint64_t catalog_revision{0};
    std::vector<settings_domain::SettingDefinition> settings;
  };

  [[nodiscard]] std::optional<ControlledSystemSetting> map_setting(
      settings_domain::SettingDefinition const& setting) const noexcept;
  [[nodiscard]] static std::optional<WindowsSystemSettingValue> target_value(
      ControlledSystemSetting setting);
  [[nodiscard]] SystemSettingApplySnapshot* find_snapshot(
      settings_domain::StableId const& id) noexcept;
  [[nodiscard]] SystemSettingApplySnapshot const* find_snapshot(
      settings_domain::StableId const& id) const noexcept;
  [[nodiscard]] bool mark_explorer_restart_verified(
      settings_domain::StableId const& setting_id, std::string& detail);
  void append_log(CorrelationId const& correlation, std::string stage,
                  ExecutionResult result, std::string setting_id,
                  std::string detail = {});

  SystemSettingsCatalogSnapshotSource& catalog_;
  SystemSettingsPlatformAdapter& platform_;
  SystemSettingsRecoveryStore& recovery_store_;
  SharedOperationOccupancy& occupancy_;
  ExecutionLog& log_;
  SystemSettingsApplySnapshot snapshot_;
  std::vector<SystemSettingsRecoveryRecord> recovery_records_;
  std::uint64_t next_recovery_record_id_{1};
  std::uint64_t next_correlation_id_{1};
};

}  // namespace azzs::application
