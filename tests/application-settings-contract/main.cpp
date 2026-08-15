#include <cstdlib>
#include <iostream>
#include <memory>

#include "azzs/application/application_settings.hpp"
#include "azzs/application/debug_mode_catalog_editor.hpp"
#include "azzs/application/execution_log.hpp"

namespace {

using azzs::application::ApplicationSettingsConfirmationAction;
using azzs::application::ArchitecturePreferenceRead;
using azzs::application::ArchitecturePreferenceReadStatus;
using azzs::application::ArchitecturePreferenceStore;
using azzs::application::ArchitecturePreferenceWriteStatus;
using azzs::application::ArchitecturePreferences;
using azzs::application::CacheRetentionPreferenceRead;
using azzs::application::CacheRetentionPreferenceReadStatus;
using azzs::application::CacheRetentionPreferenceStore;
using azzs::application::CacheRetentionPreferenceWriteStatus;
using azzs::application::CacheRetentionPreferences;
using azzs::application::CorrelationId;
using azzs::application::DebugLogGranularity;
using azzs::application::DebugModeCatalogEditor;
using azzs::application::DebugModePreferenceRead;
using azzs::application::DebugModePreferenceReadStatus;
using azzs::application::DebugModePreferenceStore;
using azzs::application::DebugModePreferenceWriteStatus;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogDebugModeRead;
using azzs::application::ExecutionLogDebugModeResult;
using azzs::application::ExecutionLogDebugModeStatus;
using azzs::application::ExecutionLogReceipt;
using azzs::application::requires_explicit_confirmation;
using azzs::domain::architecture_selection::ArchitecturePreference;
using azzs::domain::offline_package_cache::CacheRetentionPolicy;

class InMemoryArchitecturePreferenceStore final
    : public ArchitecturePreferenceStore {
 public:
  [[nodiscard]] ArchitecturePreferenceRead read_architecture_preference()
      override {
    return read;
  }

  [[nodiscard]] ArchitecturePreferenceWriteStatus write_architecture_preference(
      ArchitecturePreference preference) override {
    last_written = preference;
    return write_status;
  }

  ArchitecturePreferenceRead read{
      .status = ArchitecturePreferenceReadStatus::loaded};
  ArchitecturePreferenceWriteStatus write_status{
      ArchitecturePreferenceWriteStatus::saved};
  ArchitecturePreference last_written{
      ArchitecturePreference::prefer_arm64_prompt_fallback};
};

class InMemoryCacheRetentionPreferenceStore final
    : public CacheRetentionPreferenceStore {
 public:
  [[nodiscard]] CacheRetentionPreferenceRead read_cache_retention() override {
    return read;
  }

  [[nodiscard]] CacheRetentionPreferenceWriteStatus write_cache_retention(
      CacheRetentionPolicy retention) override {
    last_written = retention;
    return write_status;
  }

  CacheRetentionPreferenceRead read{
      .status = CacheRetentionPreferenceReadStatus::loaded};
  CacheRetentionPreferenceWriteStatus write_status{
      CacheRetentionPreferenceWriteStatus::saved};
  CacheRetentionPolicy last_written{CacheRetentionPolicy::retain_seven_days};
};

class InMemoryDebugModePreferenceStore final
    : public DebugModePreferenceStore {
 public:
  [[nodiscard]] DebugModePreferenceRead read_debug_mode() override {
    return read;
  }

  [[nodiscard]] DebugModePreferenceWriteStatus write_debug_mode(
      bool enabled) override {
    if (write_status == DebugModePreferenceWriteStatus::saved) {
      read.enabled = enabled;
      last_written = enabled;
    }
    return write_status;
  }

  DebugModePreferenceRead read{
      .status = DebugModePreferenceReadStatus::loaded};
  DebugModePreferenceWriteStatus write_status{
      DebugModePreferenceWriteStatus::saved};
  bool last_written{false};
};

class RecordingDebugLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override { return {}; }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const&) override {
    return {};
  }

  [[nodiscard]] ExecutionLogDebugModeResult set_debug_mode(
      bool enabled) override {
    if (!accepts_debug_mode_updates) {
      return {.status = ExecutionLogDebugModeStatus::unavailable,
              .enabled = enabled_,
              .error = "injected debug-mode logging failure"};
    }
    enabled_ = enabled;
    return {.status = ExecutionLogDebugModeStatus::applied,
            .enabled = enabled_};
  }

  [[nodiscard]] ExecutionLogDebugModeRead debug_mode() const override {
    return {.available = reports_debug_mode_state,
            .enabled = enabled_,
            .error = reports_debug_mode_state
                         ? std::string{}
                         : "injected debug-mode logging state failure"};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override { return {}; }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {};
  }

  bool accepts_debug_mode_updates{true};
  bool reports_debug_mode_state{true};
  bool enabled_{false};
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "application settings contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool verify_architecture_preferences_are_fail_closed() {
  bool passed = true;
  auto store = std::make_shared<InMemoryArchitecturePreferenceStore>();
  store->read.preference = ArchitecturePreference::prefer_x64;
  ArchitecturePreferences preferences{store};
  passed &= expect(preferences.preference() == ArchitecturePreference::prefer_x64,
                   "a loaded architecture preference must initialize the owner");

  auto const saved = preferences.set_preference(
      ArchitecturePreference::prefer_arm64_auto_fallback);
  passed &= expect(saved == ArchitecturePreference::prefer_arm64_auto_fallback &&
                       store->last_written ==
                           ArchitecturePreference::prefer_arm64_auto_fallback,
                   "a successful architecture preference write must update the owner");

  store->write_status = ArchitecturePreferenceWriteStatus::unavailable;
  auto const retained = preferences.set_preference(
      ArchitecturePreference::prefer_arm64_prompt_fallback);
  passed &= expect(retained == ArchitecturePreference::prefer_arm64_auto_fallback &&
                       preferences.preference() ==
                           ArchitecturePreference::prefer_arm64_auto_fallback,
                   "a failed architecture preference write must retain the prior value");

  auto unavailable = std::make_shared<InMemoryArchitecturePreferenceStore>();
  unavailable->read.status = ArchitecturePreferenceReadStatus::unavailable;
  unavailable->read.preference = ArchitecturePreference::prefer_x64;
  ArchitecturePreferences fallback{unavailable};
  passed &= expect(
      fallback.preference() == ArchitecturePreference::prefer_arm64_prompt_fallback,
      "an unavailable architecture preference read must keep the safe default");
  return passed;
}

[[nodiscard]] bool verify_cache_preferences_are_fail_closed() {
  bool passed = true;
  auto store = std::make_shared<InMemoryCacheRetentionPreferenceStore>();
  store->read.retention = CacheRetentionPolicy::retain_thirty_days;
  CacheRetentionPreferences preferences{store};
  passed &= expect(preferences.retention() == CacheRetentionPolicy::retain_thirty_days,
                   "a loaded cache retention preference must initialize the owner");

  auto const saved =
      preferences.set_retention(CacheRetentionPolicy::retain_indefinitely);
  passed &= expect(saved == CacheRetentionPolicy::retain_indefinitely &&
                       store->last_written == CacheRetentionPolicy::retain_indefinitely,
                   "a successful cache retention write must update the owner");

  store->write_status = CacheRetentionPreferenceWriteStatus::unavailable;
  auto const retained =
      preferences.set_retention(CacheRetentionPolicy::delete_immediately);
  passed &= expect(retained == CacheRetentionPolicy::retain_indefinitely &&
                       preferences.retention() ==
                           CacheRetentionPolicy::retain_indefinitely,
                   "a failed cache retention write must retain the prior value");

  auto unavailable = std::make_shared<InMemoryCacheRetentionPreferenceStore>();
  unavailable->read.status = CacheRetentionPreferenceReadStatus::unavailable;
  unavailable->read.retention = CacheRetentionPolicy::delete_immediately;
  CacheRetentionPreferences fallback{unavailable};
  passed &= expect(fallback.retention() == CacheRetentionPolicy::retain_seven_days,
                   "an unavailable cache retention read must keep the safe default");
  return passed;
}

[[nodiscard]] bool verify_dangerous_operations_require_confirmation() {
  constexpr ApplicationSettingsConfirmationAction actions[]{
      ApplicationSettingsConfirmationAction::clear_cache,
      ApplicationSettingsConfirmationAction::clear_logs,
      ApplicationSettingsConfirmationAction::delete_recovery_record,
      ApplicationSettingsConfirmationAction::catalog_change,
  };
  bool passed = true;
  for (auto const action : actions) {
    passed &= expect(requires_explicit_confirmation(action),
                     "a destructive settings operation must require confirmation");
  }
  return passed;
}

[[nodiscard]] bool verify_debug_mode_is_persisted_and_fail_closed() {
  bool passed = true;
  auto preferences = std::make_shared<InMemoryDebugModePreferenceStore>();
  RecordingDebugLog log;
  DebugModeCatalogEditor editor{log, preferences};

  auto initial = editor.snapshot();
  passed &= expect(initial.available && !initial.enabled && !log.enabled_ &&
                       initial.log_granularity == DebugLogGranularity::normal,
                   "debug mode must restore disabled by default with normal logging");

  auto const enabled = editor.set_enabled(true);
  passed &= expect(
      enabled.code == azzs::application::ApplicationSettingsDebugActionCode::updated &&
          preferences->last_written && log.enabled_ && enabled.snapshot.enabled &&
          enabled.snapshot.log_granularity == DebugLogGranularity::maximum,
      "an enabled debug mode must persist and request maximum logging detail");

  preferences->write_status = DebugModePreferenceWriteStatus::unavailable;
  auto const failed_persistence = editor.set_enabled(false);
  passed &= expect(
      failed_persistence.code ==
              azzs::application::ApplicationSettingsDebugActionCode::unavailable &&
          failed_persistence.snapshot.enabled && log.enabled_,
      "a failed preference write must retain the prior debug and logging state");

  preferences->write_status = DebugModePreferenceWriteStatus::saved;
  log.accepts_debug_mode_updates = false;
  auto const limited_logging = editor.set_enabled(false);
  passed &= expect(
      limited_logging.code ==
              azzs::application::ApplicationSettingsDebugActionCode::updated &&
          !limited_logging.snapshot.enabled && log.enabled_ &&
          limited_logging.snapshot.log_granularity ==
              DebugLogGranularity::unavailable,
      "a logging control failure must be exposed instead of claiming a granularity");

  auto const unbound_temporary_recovery =
      editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::
              recovered_unsaved_continue);
  passed &= expect(
      !unbound_temporary_recovery &&
          editor.editor_access() ==
              azzs::application::software_catalog::CatalogEditorAccess::
                  unavailable,
      "an unbound catalog lifecycle must not grant temporary editor authority");

  auto unavailable_preferences = std::make_shared<InMemoryDebugModePreferenceStore>();
  unavailable_preferences->read.status = DebugModePreferenceReadStatus::unavailable;
  RecordingDebugLog unavailable_log;
  DebugModeCatalogEditor unavailable_editor{unavailable_log,
                                              unavailable_preferences};
  passed &= expect(
      !unavailable_editor.snapshot().available &&
          unavailable_editor.editor_access() ==
              azzs::application::software_catalog::CatalogEditorAccess::unavailable,
      "an unavailable preference store must fail closed without editor authority");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_architecture_preferences_are_fail_closed();
  passed &= verify_cache_preferences_are_fail_closed();
  passed &= verify_dangerous_operations_require_confirmation();
  passed &= verify_debug_mode_is_persisted_and_fail_closed();
  if (!passed) {
    return EXIT_FAILURE;
  }

  std::cout << "application settings contract passed\n";
  return EXIT_SUCCESS;
}
