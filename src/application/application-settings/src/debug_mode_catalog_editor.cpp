#include "azzs/application/debug_mode_catalog_editor.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace azzs::application {
namespace {

namespace catalog = domain::software_catalog;
namespace catalog_app = software_catalog;

[[nodiscard]] bool is_ready(catalog_app::SoftwareCatalogLifecycleSnapshot const&
                                snapshot) noexcept {
  return snapshot.mode == catalog_app::CatalogLifecycleMode::ready ||
         snapshot.mode == catalog_app::CatalogLifecycleMode::read_only;
}

template <typename Item>
[[nodiscard]] auto find_item(std::vector<Item>& values, std::string_view id) {
  return std::find_if(values.begin(), values.end(), [id](Item const& item) {
    return item.id == id;
  });
}

template <typename Item>
[[nodiscard]] auto find_item(std::vector<Item> const& values,
                             std::string_view id) {
  return std::find_if(values.begin(), values.end(), [id](Item const& item) {
    return item.id == id;
  });
}

}  // namespace

DebugModeCatalogEditor::DebugModeCatalogEditor(
    ExecutionLog& log, std::shared_ptr<DebugModePreferenceStore> preferences)
    noexcept
    : log_(log), preferences_(std::move(preferences)) {
  if (!preferences_) {
    preference_error_ = "debug-mode preference storage is not configured";
    return;
  }

  try {
    auto const preference = preferences_->read_debug_mode();
    if (preference.status != DebugModePreferenceReadStatus::loaded) {
      preference_error_ = "debug-mode preference storage is unavailable";
      return;
    }
    preferences_available_ = true;
    enabled_ = preference.enabled;
    auto const configured = log_.set_debug_mode(enabled_);
    if (configured.status != ExecutionLogDebugModeStatus::applied ||
        configured.enabled != enabled_) {
      preference_error_ = configured.error.empty()
                              ? "execution log could not restore debug detail"
                              : std::move(configured.error);
    }
  } catch (...) {
    preference_error_ = "debug-mode preference storage failed";
  }
}

void DebugModeCatalogEditor::bind_catalog_lifecycle(
    catalog_app::SoftwareCatalogLifecycle& lifecycle) noexcept {
  lifecycle_ = std::addressof(lifecycle);
}

ApplicationSettingsDebugSnapshot DebugModeCatalogEditor::snapshot() {
  return settings_snapshot();
}

ApplicationSettingsDebugActionResult DebugModeCatalogEditor::set_enabled(
    bool enabled) {
  if (!preferences_available_ || !preferences_) {
    auto current = settings_snapshot();
    auto detail = current.detail.empty()
                      ? std::string{"debug-mode preference storage is unavailable"}
                      : current.detail;
    return {.code = ApplicationSettingsDebugActionCode::unavailable,
            .snapshot = std::move(current),
            .detail = std::move(detail)};
  }

  auto const previous = enabled_;
  ExecutionLogDebugModeResult log_result;
  try {
    log_result = log_.set_debug_mode(enabled);
  } catch (...) {
    log_result.error = "execution log debug-mode request failed";
  }

  DebugModePreferenceWriteStatus persisted{
      DebugModePreferenceWriteStatus::unavailable};
  try {
    persisted = preferences_->write_debug_mode(enabled);
  } catch (...) {
    preference_error_ = "debug-mode preference storage failed";
  }
  if (persisted != DebugModePreferenceWriteStatus::saved) {
    if (log_result.status == ExecutionLogDebugModeStatus::applied) {
      static_cast<void>(log_.set_debug_mode(previous));
    }
    auto current = settings_snapshot();
    return {.code = ApplicationSettingsDebugActionCode::unavailable,
            .snapshot = std::move(current),
            .detail = "debug-mode preference could not be saved"};
  }

  enabled_ = enabled;
  preference_error_.clear();
  if (enabled_) {
    temporary_close_recovery_ = false;
  }
  auto current = settings_snapshot();
  std::string detail =
      enabled ? "debug mode enabled" : "debug mode disabled";
  if (log_result.status != ExecutionLogDebugModeStatus::applied) {
    detail += "; " + (log_result.error.empty()
                            ? std::string{"execution log detail is unavailable"}
                            : log_result.error);
  }
  return {.code = ApplicationSettingsDebugActionCode::updated,
          .snapshot = std::move(current),
          .detail = std::move(detail)};
}

DebugLogPolicyRead DebugModeCatalogEditor::read_debug_log_policy() const {
  if (!preferences_available_) {
    return unavailable_log_policy(preference_error_.empty()
                                      ? "debug-mode preference storage is unavailable"
                                      : preference_error_);
  }

  ExecutionLogDebugModeRead observed;
  try {
    observed = log_.debug_mode();
  } catch (...) {
    return unavailable_log_policy("execution log debug-mode state failed");
  }
  if (!observed.available) {
    return unavailable_log_policy(observed.error.empty()
                                      ? "execution log debug-mode state is unavailable"
                                      : observed.error);
  }
  if (observed.enabled != enabled_) {
    return unavailable_log_policy(
        "execution log detail does not match the saved debug-mode preference");
  }
  return {
      .available = true,
      .debug_enabled = enabled_,
      .granularity = enabled_ ? DebugLogGranularity::maximum
                              : DebugLogGranularity::normal,
      .filterable_fields = {kRequiredDebugLogFilterFields.begin(),
                            kRequiredDebugLogFilterFields.end()},
      .locating_semantics =
          DebugLogLocationSemantics::event_sequence_and_correlation_id,
      .coverage = {kRequiredDebugLogCoverage.begin(),
                   kRequiredDebugLogCoverage.end()},
  };
}

catalog_app::CatalogEditorAccess DebugModeCatalogEditor::editor_access()
    const noexcept {
  if (temporary_close_recovery_) {
    return catalog_app::CatalogEditorAccess::temporary_close_recovery;
  }
  return enabled_ ? catalog_app::CatalogEditorAccess::debug_mode
                  : catalog_app::CatalogEditorAccess::unavailable;
}

DebugModeCatalogEditorSnapshot DebugModeCatalogEditor::editor_snapshot() const {
  DebugLogPolicyReader policy_reader{
      std::shared_ptr<DebugLogPolicyProvider const>{
          const_cast<DebugModeCatalogEditor*>(this),
          [](DebugLogPolicyProvider const*) {}}};
  auto result = DebugModeCatalogEditorSnapshot{
      .settings = settings_snapshot(),
      .log_policy = policy_reader.snapshot(),
  };
  if (lifecycle_ != nullptr) {
    result.catalog = lifecycle_->snapshot();
  }
  return result;
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::edit_document(
    catalog::SoftwareCatalogDocument document) {
  if (lifecycle_ == nullptr) {
    return unavailable("software catalog lifecycle is not configured");
  }
  auto result = lifecycle_->edit_document(std::move(document));
  if (!result.succeeded() ||
      editor_access() != catalog_app::CatalogEditorAccess::debug_mode) {
    return result;
  }

  // Normal debug editing must publish a recovery checkpoint after every
  // accepted typed edit. Temporary close recovery deliberately cannot create
  // checkpoints, because that session only exists to save or discard content
  // that was already retained by the lifecycle.
  auto checkpoint = lifecycle_->checkpoint_unsaved();
  if (!checkpoint.succeeded()) {
    return checkpoint;
  }
  result.message = "unsaved catalog edits retained and checkpointed";
  return result;
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::add_software(
    catalog::SoftwareDefinition software) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  if (software.id.empty() ||
      find_item(document->software, software.id) != document->software.end() ||
      find_item(document->drivers, software.id) != document->drivers.end()) {
    return rejected("software identifier is missing or already exists");
  }
  document->software.push_back(std::move(software));
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::duplicate_software(
    std::string_view source_id, std::string duplicate_id) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const source = find_item(document->software, source_id);
  if (source == document->software.end() || duplicate_id.empty() ||
      find_item(document->software, duplicate_id) != document->software.end() ||
      find_item(document->drivers, duplicate_id) != document->drivers.end()) {
    return rejected("software copy source or identifier is invalid");
  }
  auto copy = *source;
  copy.id = std::move(duplicate_id);
  document->software.push_back(std::move(copy));
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::update_software(
    catalog::SoftwareDefinition software) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const found = find_item(document->software, software.id);
  if (found == document->software.end()) {
    return rejected("software identifier does not exist");
  }
  *found = std::move(software);
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::set_software_enabled(
    std::string_view id, bool enabled) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const found = find_item(document->software, id);
  if (found == document->software.end()) {
    return rejected("software identifier does not exist");
  }
  found->enabled = enabled;
  found->enabled_declared = true;
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::remove_software(
    std::string_view id) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const found = find_item(document->software, id);
  if (found == document->software.end()) {
    return rejected("software identifier does not exist");
  }
  document->software.erase(found);
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::add_driver(
    catalog::DriverDefinition driver) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  if (driver.id.empty() ||
      find_item(document->drivers, driver.id) != document->drivers.end() ||
      find_item(document->software, driver.id) != document->software.end()) {
    return rejected("driver identifier is missing or already exists");
  }
  document->drivers.push_back(std::move(driver));
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::update_driver(
    catalog::DriverDefinition driver) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const found = find_item(document->drivers, driver.id);
  if (found == document->drivers.end()) {
    return rejected("driver identifier does not exist");
  }
  *found = std::move(driver);
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::set_driver_enabled(
    std::string_view id, bool enabled) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const found = find_item(document->drivers, id);
  if (found == document->drivers.end()) {
    return rejected("driver identifier does not exist");
  }
  found->enabled = enabled;
  found->enabled_declared = true;
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::remove_driver(
    std::string_view id) {
  auto document = working_document();
  if (!document.has_value()) {
    return unavailable("no catalog document is available for editing");
  }
  auto const found = find_item(document->drivers, id);
  if (found == document->drivers.end()) {
    return rejected("driver identifier does not exist");
  }
  document->drivers.erase(found);
  return edit_document(std::move(*document));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::checkpoint_unsaved() {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->checkpoint_unsaved();
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::save_draft() {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->save_draft();
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::delete_saved_draft() {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->delete_saved_draft();
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::discard_unsaved() {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->discard_unsaved();
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::apply_saved_draft() {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->apply_saved_draft();
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::handle_close(
    catalog_app::CatalogCloseChoice choice) {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->handle_close(choice);
}

catalog_app::CatalogCandidatePreview
DebugModeCatalogEditor::preview_manual_import(std::string path) {
  if (lifecycle_ == nullptr) {
    return {.origin = catalog_app::CatalogCandidateOrigin::manual_import,
            .path = std::move(path),
            .error = "software catalog lifecycle is not configured"};
  }
  return lifecycle_->preview_manual_import(std::move(path));
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::apply_preview(
    std::string_view confirmation_token) {
  return lifecycle_ == nullptr
             ? unavailable("software catalog lifecycle is not configured")
             : lifecycle_->apply_preview(confirmation_token);
}

bool DebugModeCatalogEditor::begin_temporary_close_recovery(
    CatalogEditorTemporaryAccessReason reason) {
  if (enabled_ || lifecycle_ == nullptr) {
    return false;
  }

  auto const snapshot = lifecycle_->snapshot();
  if (!is_ready(snapshot)) {
    return false;
  }
  auto const state = snapshot.draft.state;
  auto const permitted =
      reason == CatalogEditorTemporaryAccessReason::recovered_unsaved_continue
          ? state == catalog_app::DraftWorkState::recovered_unsaved
          : state == catalog_app::DraftWorkState::recovered_unsaved ||
                state == catalog_app::DraftWorkState::unsaved_changes;
  if (!permitted) {
    return false;
  }

  temporary_close_recovery_ = true;
  return true;
}

void DebugModeCatalogEditor::end_temporary_close_recovery() noexcept {
  temporary_close_recovery_ = false;
}

std::optional<catalog::SoftwareCatalogDocument>
DebugModeCatalogEditor::working_document() const {
  if (lifecycle_ == nullptr) {
    return std::nullopt;
  }
  auto const snapshot = lifecycle_->snapshot();
  if (snapshot.draft.document.has_value()) {
    return snapshot.draft.document;
  }
  return snapshot.current_document;
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::unavailable(
    std::string message) const {
  return {.code = catalog_app::CatalogActionCode::unavailable,
          .message = std::move(message)};
}

catalog_app::CatalogActionResult DebugModeCatalogEditor::rejected(
    std::string message) const {
  return {.code = catalog_app::CatalogActionCode::rejected,
          .message = std::move(message)};
}

ApplicationSettingsDebugSnapshot
DebugModeCatalogEditor::settings_snapshot() const {
  auto const policy = read_debug_log_policy();
  auto const lifecycle_snapshot =
      lifecycle_ == nullptr ? catalog_app::SoftwareCatalogLifecycleSnapshot{}
                            : lifecycle_->snapshot();
  auto const lifecycle_ready = lifecycle_ != nullptr && is_ready(lifecycle_snapshot);
  auto detail = preference_error_;
  if (detail.empty() && !policy.available) {
    detail = policy.not_obtained_reason;
  }
  if (detail.empty()) {
    detail = enabled_ ? "debug mode is enabled" : "debug mode is disabled";
  }
  return {
      .available = preferences_available_,
      .enabled = enabled_,
      .catalog_editor_available =
          preferences_available_ && enabled_ && lifecycle_ready,
      .manual_catalog_import_available =
          preferences_available_ && enabled_ && lifecycle_ready,
      .log_granularity = policy.available ? policy.granularity
                                           : DebugLogGranularity::unavailable,
      .temporary_close_recovery = temporary_close_recovery_,
      .detail = std::move(detail),
  };
}

DebugLogPolicyRead DebugModeCatalogEditor::unavailable_log_policy(
    std::string reason) const {
  return {.available = false, .not_obtained_reason = std::move(reason)};
}

}  // namespace azzs::application
