#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "azzs/application/application_settings.hpp"
#include "azzs/application/debug_log_policy/debug_log_policy.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/software_catalog_lifecycle.hpp"

namespace azzs::application {

// This coordinator is the only owner of the debug-mode preference and the
// trusted maintenance authority. It deliberately delegates all catalog state
// and validation to SoftwareCatalogLifecycle.
struct DebugModeCatalogEditorSnapshot final {
  ApplicationSettingsDebugSnapshot settings;
  DebugLogPolicySnapshot log_policy;
  software_catalog::SoftwareCatalogLifecycleSnapshot catalog;
};

class DebugModeCatalogEditor final
    : public ApplicationSettingsDebugProvider,
      public DebugLogPolicyProvider,
      public software_catalog::CatalogMaintenanceAccess {
 public:
  DebugModeCatalogEditor(
      ExecutionLog& log,
      std::shared_ptr<DebugModePreferenceStore> preferences) noexcept;

  DebugModeCatalogEditor(DebugModeCatalogEditor const&) = delete;
  DebugModeCatalogEditor& operator=(DebugModeCatalogEditor const&) = delete;

  // The composition root constructs this owner before the lifecycle so the
  // lifecycle can ask it for access. Binding is completed before restore.
  void bind_catalog_lifecycle(
      software_catalog::SoftwareCatalogLifecycle& lifecycle) noexcept;

  [[nodiscard]] ApplicationSettingsDebugSnapshot snapshot() override;
  [[nodiscard]] ApplicationSettingsDebugActionResult set_enabled(
      bool enabled) override;
  [[nodiscard]] DebugLogPolicyRead read_debug_log_policy() const override;
  [[nodiscard]] software_catalog::CatalogEditorAccess editor_access()
      const noexcept override;

  [[nodiscard]] DebugModeCatalogEditorSnapshot editor_snapshot() const;

  [[nodiscard]] software_catalog::CatalogActionResult edit_document(
      domain::software_catalog::SoftwareCatalogDocument document);
  [[nodiscard]] software_catalog::CatalogActionResult add_software(
      domain::software_catalog::SoftwareDefinition software);
  [[nodiscard]] software_catalog::CatalogActionResult duplicate_software(
      std::string_view source_id, std::string duplicate_id);
  [[nodiscard]] software_catalog::CatalogActionResult update_software(
      domain::software_catalog::SoftwareDefinition software);
  [[nodiscard]] software_catalog::CatalogActionResult set_software_enabled(
      std::string_view id, bool enabled);
  [[nodiscard]] software_catalog::CatalogActionResult remove_software(
      std::string_view id);
  [[nodiscard]] software_catalog::CatalogActionResult add_driver(
      domain::software_catalog::DriverDefinition driver);
  [[nodiscard]] software_catalog::CatalogActionResult update_driver(
      domain::software_catalog::DriverDefinition driver);
  [[nodiscard]] software_catalog::CatalogActionResult set_driver_enabled(
      std::string_view id, bool enabled);
  [[nodiscard]] software_catalog::CatalogActionResult remove_driver(
      std::string_view id);

  [[nodiscard]] software_catalog::CatalogActionResult checkpoint_unsaved();
  [[nodiscard]] software_catalog::CatalogActionResult save_draft();
  [[nodiscard]] software_catalog::CatalogActionResult delete_saved_draft();
  [[nodiscard]] software_catalog::CatalogActionResult discard_unsaved();
  [[nodiscard]] software_catalog::CatalogActionResult apply_saved_draft();
  [[nodiscard]] software_catalog::CatalogActionResult handle_close(
      software_catalog::CatalogCloseChoice choice);
  [[nodiscard]] software_catalog::CatalogCandidatePreview preview_manual_import(
      std::string path);
  [[nodiscard]] software_catalog::CatalogActionResult apply_preview(
      std::string_view confirmation_token);

  // This is only entered by the workbench-close flow after the user explicitly
  // asks to return to the editor or a save fails while debug mode is hidden.
  void begin_temporary_close_recovery() noexcept;
  void end_temporary_close_recovery() noexcept;

 private:
  [[nodiscard]] std::optional<domain::software_catalog::SoftwareCatalogDocument>
  working_document() const;
  [[nodiscard]] software_catalog::CatalogActionResult unavailable(
      std::string message) const;
  [[nodiscard]] software_catalog::CatalogActionResult rejected(
      std::string message) const;
  [[nodiscard]] ApplicationSettingsDebugSnapshot settings_snapshot() const;
  [[nodiscard]] DebugLogPolicyRead unavailable_log_policy(
      std::string reason) const;

  ExecutionLog& log_;
  std::shared_ptr<DebugModePreferenceStore> preferences_;
  software_catalog::SoftwareCatalogLifecycle* lifecycle_{};
  bool enabled_{false};
  bool preferences_available_{false};
  bool temporary_close_recovery_{false};
  std::string preference_error_;
};

}  // namespace azzs::application
