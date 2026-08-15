#include "pch.h"

#include "SoftwareCatalogEditorPage.xaml.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>

#if __has_include("Pages/SoftwareCatalogEditorPage.g.cpp")
#include "Pages/SoftwareCatalogEditorPage.g.cpp"
#endif

namespace {

namespace catalog = azzs::domain::software_catalog;
namespace catalog_app = azzs::application::software_catalog;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialog;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult;
using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

[[nodiscard]] winrt::hstring resource_string(wchar_t const* key) {
  return ResourceLoader{}.GetString(key);
}

[[nodiscard]] std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

[[nodiscard]] std::string join_values(
    std::vector<std::string> const& values) {
  std::string text;
  for (auto const& value : values) {
    if (!text.empty()) {
      text += ", ";
    }
    text += value;
  }
  return text;
}

[[nodiscard]] std::vector<std::string> split_values(std::string_view text) {
  std::vector<std::string> values;
  std::string value;
  auto append = [&] {
    auto const first = value.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
      auto const last = value.find_last_not_of(" \t\r\n");
      values.push_back(value.substr(first, last - first + 1));
    }
    value.clear();
  };
  for (auto const character : text) {
    if (character == ',' || character == ';' || character == '\n') {
      append();
    } else {
      value.push_back(character);
    }
  }
  append();
  return values;
}

[[nodiscard]] std::optional<std::string> optional_text(
    winrt::hstring const& text) {
  auto value = winrt::to_string(text);
  return value.empty() ? std::nullopt
                       : std::optional<std::string>{std::move(value)};
}

[[nodiscard]] int tier_index(std::optional<catalog::SoftwareTier> tier) {
  if (tier == catalog::SoftwareTier::basic) {
    return 0;
  }
  if (tier == catalog::SoftwareTier::normal) {
    return 1;
  }
  return -1;
}

[[nodiscard]] std::optional<catalog::SoftwareTier> tier_from_index(
    int index) {
  if (index == 0) {
    return catalog::SoftwareTier::basic;
  }
  if (index == 1) {
    return catalog::SoftwareTier::normal;
  }
  return std::nullopt;
}

[[nodiscard]] int version_policy_index(
    std::optional<catalog::VersionPolicy> policy) {
  if (policy == catalog::VersionPolicy::latest_stable) {
    return 0;
  }
  if (policy == catalog::VersionPolicy::latest_stable_with_history) {
    return 1;
  }
  if (policy == catalog::VersionPolicy::fixed) {
    return 2;
  }
  if (policy == catalog::VersionPolicy::maintainer_provided) {
    return 3;
  }
  return -1;
}

[[nodiscard]] std::optional<catalog::VersionPolicy> version_policy_from_index(
    int index) {
  switch (index) {
    case 0:
      return catalog::VersionPolicy::latest_stable;
    case 1:
      return catalog::VersionPolicy::latest_stable_with_history;
    case 2:
      return catalog::VersionPolicy::fixed;
    case 3:
      return catalog::VersionPolicy::maintainer_provided;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] int source_purpose_index(
    std::optional<catalog::SourcePurpose> purpose) {
  if (purpose == catalog::SourcePurpose::primary) {
    return 0;
  }
  if (purpose == catalog::SourcePurpose::alternative) {
    return 1;
  }
  if (purpose == catalog::SourcePurpose::project_backup) {
    return 2;
  }
  return -1;
}

[[nodiscard]] std::optional<catalog::SourcePurpose> source_purpose_from_index(
    int index) {
  switch (index) {
    case 0:
      return catalog::SourcePurpose::primary;
    case 1:
      return catalog::SourcePurpose::alternative;
    case 2:
      return catalog::SourcePurpose::project_backup;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] wchar_t const* source_purpose_key(
    std::optional<catalog::SourcePurpose> purpose) {
  if (purpose == catalog::SourcePurpose::primary) {
    return L"SoftwareCatalogEditorSourcePrimary.Content";
  }
  if (purpose == catalog::SourcePurpose::alternative) {
    return L"SoftwareCatalogEditorSourceAlternative.Content";
  }
  if (purpose == catalog::SourcePurpose::project_backup) {
    return L"SoftwareCatalogEditorSourceProjectBackup.Content";
  }
  return L"SoftwareCatalogEditorSourcePurposeUnassigned";
}

[[nodiscard]] winrt::hstring source_label(
    catalog::CatalogSource const& source) {
  auto label = std::wstring{resource_string(source_purpose_key(source.purpose))};
  if (!source.address.empty()) {
    label += L" - ";
    label += winrt::to_hstring(source.address).c_str();
  }
  return winrt::hstring{std::move(label)};
}

[[nodiscard]] bool catalog_action_succeeded(
    catalog_app::CatalogActionResult const& result) noexcept {
  return result.succeeded();
}

[[nodiscard]] InfoBarSeverity catalog_action_severity(
    catalog_app::CatalogActionResult const& result) noexcept {
  return catalog_action_succeeded(result) ? InfoBarSeverity::Informational
                                          : InfoBarSeverity::Error;
}

[[nodiscard]] winrt::hstring draft_state_text(
    catalog_app::DraftWorkState state) {
  switch (state) {
    case catalog_app::DraftWorkState::none:
      return resource_string(L"SoftwareCatalogEditorDraftNone");
    case catalog_app::DraftWorkState::saved_not_applied:
      return resource_string(L"SoftwareCatalogEditorDraftSaved");
    case catalog_app::DraftWorkState::unsaved_changes:
      return resource_string(L"SoftwareCatalogEditorDraftUnsaved");
    case catalog_app::DraftWorkState::recovered_unsaved:
      return resource_string(L"SoftwareCatalogEditorDraftRecovered");
  }
  return resource_string(L"SoftwareCatalogEditorDraftNone");
}

[[nodiscard]] wchar_t const* catalog_issue_key(catalog::CatalogIssueCode code) {
  switch (code) {
    case catalog::CatalogIssueCode::malformed_toml:
      return L"SoftwareCatalogEditorIssueMalformedToml";
    case catalog::CatalogIssueCode::unsupported_schema:
      return L"SoftwareCatalogEditorIssueUnsupportedSchema";
    case catalog::CatalogIssueCode::missing_required_field:
      return L"SoftwareCatalogEditorIssueMissingRequiredField";
    case catalog::CatalogIssueCode::invalid_field:
      return L"SoftwareCatalogEditorIssueInvalidField";
    case catalog::CatalogIssueCode::duplicate_field:
      return L"SoftwareCatalogEditorIssueDuplicateField";
    case catalog::CatalogIssueCode::duplicate_stable_id:
      return L"SoftwareCatalogEditorIssueDuplicateStableId";
    case catalog::CatalogIssueCode::stable_id_reused:
      return L"SoftwareCatalogEditorIssueStableIdReused";
    case catalog::CatalogIssueCode::unknown_execution_semantics:
      return L"SoftwareCatalogEditorIssueUnknownExecutionSemantics";
    case catalog::CatalogIssueCode::invalid_reference:
      return L"SoftwareCatalogEditorIssueInvalidReference";
    case catalog::CatalogIssueCode::missing_dependency:
      return L"SoftwareCatalogEditorIssueMissingDependency";
    case catalog::CatalogIssueCode::dependency_cycle:
      return L"SoftwareCatalogEditorIssueDependencyCycle";
    case catalog::CatalogIssueCode::unavailable_dependency:
      return L"SoftwareCatalogEditorIssueUnavailableDependency";
    case catalog::CatalogIssueCode::draft_release_state:
      return L"SoftwareCatalogEditorIssueDraftReleaseState";
    case catalog::CatalogIssueCode::required_item_missing:
      return L"SoftwareCatalogEditorIssueRequiredItemMissing";
    case catalog::CatalogIssueCode::required_item_disabled:
      return L"SoftwareCatalogEditorIssueRequiredItemDisabled";
    case catalog::CatalogIssueCode::release_revision_regression:
      return L"SoftwareCatalogEditorIssueReleaseRevisionRegression";
    case catalog::CatalogIssueCode::release_revision_conflict:
      return L"SoftwareCatalogEditorIssueReleaseRevisionConflict";
    case catalog::CatalogIssueCode::release_dependency_error:
      return L"SoftwareCatalogEditorIssueReleaseDependencyError";
    case catalog::CatalogIssueCode::install_profile_unavailable:
      return L"SoftwareCatalogEditorIssueInstallProfileUnavailable";
    case catalog::CatalogIssueCode::install_profile_not_release_ready:
      return L"SoftwareCatalogEditorIssueInstallProfileNotReleaseReady";
    case catalog::CatalogIssueCode::prohibited_content:
      return L"SoftwareCatalogEditorIssueProhibitedContent";
  }
  return L"SoftwareCatalogEditorIssueUnknown";
}

[[nodiscard]] wchar_t const* catalog_action_key(
    catalog_app::CatalogActionCode code) {
  switch (code) {
    case catalog_app::CatalogActionCode::succeeded:
      return L"SoftwareCatalogEditorActionSucceeded";
    case catalog_app::CatalogActionCode::no_change:
      return L"SoftwareCatalogEditorActionNoChange";
    case catalog_app::CatalogActionCode::rejected:
      return L"SoftwareCatalogEditorActionRejected";
    case catalog_app::CatalogActionCode::debug_mode_required:
      return L"SoftwareCatalogEditorActionDebugModeRequired";
    case catalog_app::CatalogActionCode::not_restored:
      return L"SoftwareCatalogEditorActionNotRestored";
    case catalog_app::CatalogActionCode::unavailable:
      return L"SoftwareCatalogEditorActionUnavailable";
    case catalog_app::CatalogActionCode::occupied:
      return L"SoftwareCatalogEditorActionOccupied";
    case catalog_app::CatalogActionCode::conflict:
      return L"SoftwareCatalogEditorActionConflict";
    case catalog_app::CatalogActionCode::read_only:
      return L"SoftwareCatalogEditorActionReadOnly";
    case catalog_app::CatalogActionCode::persistence_failed:
      return L"SoftwareCatalogEditorActionPersistenceFailed";
    case catalog_app::CatalogActionCode::outcome_unknown:
      return L"SoftwareCatalogEditorActionOutcomeUnknown";
    case catalog_app::CatalogActionCode::saved_cleanup_pending:
      return L"SoftwareCatalogEditorActionSavedCleanupPending";
    case catalog_app::CatalogActionCode::applied_cleanup_pending:
      return L"SoftwareCatalogEditorActionAppliedCleanupPending";
    case catalog_app::CatalogActionCode::applied_log_incomplete:
      return L"SoftwareCatalogEditorActionAppliedLogIncomplete";
    case catalog_app::CatalogActionCode::returned_to_editor:
      return L"SoftwareCatalogEditorActionReturnedToEditor";
  }
  return L"SoftwareCatalogEditorActionUnavailable";
}

[[nodiscard]] winrt::hstring issue_text(
    std::vector<catalog::CatalogIssue> const& issues) {
  std::wstring text;
  for (auto const& issue : issues) {
    if (!text.empty()) {
      text += L'\n';
    }
    if (!issue.location.empty()) {
      text += winrt::to_hstring(issue.location).c_str();
      text += L": ";
    }
    text += resource_string(catalog_issue_key(issue.code)).c_str();
  }
  return winrt::hstring{std::move(text)};
}

}  // namespace

namespace winrt::Azzs::Ui::Pages::implementation {

SoftwareCatalogEditorPage::SoftwareCatalogEditorPage() { InitializeComponent(); }

void SoftwareCatalogEditorPage::bind(
    azzs::application::DebugModeCatalogEditor& editor) {
  editor_ = std::addressof(editor);
  refresh();
}

void SoftwareCatalogEditorPage::show_action_result(
    catalog_app::CatalogActionResult const& result) {
  project_action(result);
}

void SoftwareCatalogEditorPage::OnSearchChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&) {
  if (!projecting_) {
    project_document(selected_software_id());
  }
}

void SoftwareCatalogEditorPage::OnSoftwareSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    project_selected_software();
  }
}

void SoftwareCatalogEditorPage::OnSoftwareFieldLostFocus(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!projecting_) {
    commit_selected_software();
  }
}

void SoftwareCatalogEditorPage::OnSoftwareFieldSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    commit_selected_software();
  }
}

void SoftwareCatalogEditorPage::OnSoftwareEnabledToggled(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!projecting_) {
    commit_selected_software();
  }
}

void SoftwareCatalogEditorPage::OnCategorySelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    project_selected_category();
  }
}

void SoftwareCatalogEditorPage::OnCategoryFieldLostFocus(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!projecting_) {
    commit_selected_category();
  }
}

void SoftwareCatalogEditorPage::OnAddCategory(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (document_.has_value() && editing_enabled_) {
    document_->categories.push_back(catalog::CatalogCategory{
        .id = next_category_id(),
        .name = winrt::to_string(
            resource_string(L"SoftwareCatalogEditorNewCategory")),
    });
    commit_document();
  }
}

void SoftwareCatalogEditorPage::OnCategoryLocalizationSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    project_selected_category_localization();
  }
}

void SoftwareCatalogEditorPage::OnCategoryLocalizationFieldLostFocus(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!projecting_) {
    commit_selected_category_localization();
  }
}

void SoftwareCatalogEditorPage::OnAddCategoryLocalization(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const category = selected_category_index();
  if (!document_.has_value() || !category.has_value() || !editing_enabled_) {
    return;
  }
  auto& localizations = document_->categories[*category].localizations;
  constexpr std::array<std::string_view, 4> candidates{
      "en-US", "zh-TW", "ja-JP", "ko-KR"};
  auto const available = std::find_if(
      candidates.begin(), candidates.end(), [&](std::string_view candidate) {
        return std::none_of(localizations.begin(), localizations.end(),
                            [&](catalog::CatalogLocalization const& localization) {
                              return localization.locale == candidate;
                            });
      });
  if (available == candidates.end()) {
    set_status(resource_string(L"SoftwareCatalogEditorLocalizationLimit"),
               InfoBarSeverity::Warning);
    return;
  }
  localizations.push_back(catalog::CatalogLocalization{
      .locale = std::string{*available},
  });
  commit_document();
}

void SoftwareCatalogEditorPage::OnDeleteCategoryLocalization(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const category = selected_category_index();
  auto const localization = selected_category_localization_index();
  if (!document_.has_value() || !category.has_value() ||
      !localization.has_value() || !editing_enabled_) {
    return;
  }
  auto& localizations = document_->categories[*category].localizations;
  localizations.erase(localizations.begin() +
                      static_cast<std::ptrdiff_t>(*localization));
  commit_document();
}

void SoftwareCatalogEditorPage::OnSourceSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    project_selected_source();
  }
}

void SoftwareCatalogEditorPage::OnSourceFieldSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    commit_selected_source();
  }
}

void SoftwareCatalogEditorPage::OnSourceFieldLostFocus(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!projecting_) {
    commit_selected_source();
  }
}

void SoftwareCatalogEditorPage::OnAddSource(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const selected = selected_software_index();
  if (!document_.has_value() || !selected.has_value() || !editing_enabled_) {
    return;
  }
  auto& sources = document_->software[*selected].sources;
  sources.push_back(catalog::CatalogSource{
      .purpose = sources.empty() ? catalog::SourcePurpose::primary
                                 : catalog::SourcePurpose::alternative,
  });
  commit_document();
}

void SoftwareCatalogEditorPage::OnDeleteSource(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const software = selected_software_index();
  auto const source = selected_source_index();
  if (!document_.has_value() || !software.has_value() || !source.has_value() ||
      !editing_enabled_) {
    return;
  }
  document_->software[*software].sources.erase(
      document_->software[*software].sources.begin() +
      static_cast<std::ptrdiff_t>(*source));
  commit_document();
}

void SoftwareCatalogEditorPage::OnLocalizationSelectionChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&) {
  if (!projecting_) {
    project_selected_localization();
  }
}

void SoftwareCatalogEditorPage::OnLocalizationFieldLostFocus(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!projecting_) {
    commit_selected_localization();
  }
}

void SoftwareCatalogEditorPage::OnAddLocalization(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const selected = selected_software_index();
  if (!document_.has_value() || !selected.has_value() || !editing_enabled_) {
    return;
  }
  auto& localizations = document_->software[*selected].localizations;
  constexpr std::array<std::string_view, 4> candidates{
      "en-US", "zh-TW", "ja-JP", "ko-KR"};
  auto const available = std::find_if(
      candidates.begin(), candidates.end(), [&](std::string_view candidate) {
        return std::none_of(localizations.begin(), localizations.end(),
                            [&](catalog::CatalogLocalization const& localization) {
                              return localization.locale == candidate;
                            });
      });
  if (available == candidates.end()) {
    set_status(resource_string(L"SoftwareCatalogEditorLocalizationLimit"),
               InfoBarSeverity::Warning);
    return;
  }
  localizations.push_back(catalog::CatalogLocalization{
      .locale = std::string{*available},
  });
  commit_document();
}

void SoftwareCatalogEditorPage::OnDeleteLocalization(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto const software = selected_software_index();
  auto const localization = selected_localization_index();
  if (!document_.has_value() || !software.has_value() ||
      !localization.has_value() || !editing_enabled_) {
    return;
  }
  document_->software[*software].localizations.erase(
      document_->software[*software].localizations.begin() +
      static_cast<std::ptrdiff_t>(*localization));
  commit_document();
}

void SoftwareCatalogEditorPage::OnAddSoftware(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ == nullptr || !document_.has_value()) {
    return;
  }
  catalog::SoftwareDefinition software{
      .id = next_software_id(),
      .enabled = true,
      .enabled_declared = true,
      .name = winrt::to_string(
          resource_string(L"SoftwareCatalogEditorNewSoftware")),
      .tier = catalog::SoftwareTier::normal,
      .category_id = document_->categories.empty()
                         ? std::string{}
                         : document_->categories.front().id,
      .branch = winrt::to_string(
          resource_string(L"SoftwareCatalogEditorNewSoftwareBranch")),
      .version_policy = catalog::VersionPolicy::latest_stable,
      .dependencies = {},
      .dependencies_declared = true,
      .bundled_editions = {},
      .bundled_editions_declared = true,
      .notice = {},
  };
  project_action(editor_->add_software(std::move(software)));
}

void SoftwareCatalogEditorPage::OnDuplicateSoftware(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ == nullptr) {
    return;
  }
  auto const selected = selected_software_index();
  if (!selected.has_value() || !document_.has_value()) {
    return;
  }
  project_action(editor_->duplicate_software(document_->software[*selected].id,
                                             next_software_id()));
}

winrt::fire_and_forget SoftwareCatalogEditorPage::OnDeleteSoftware(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto lifetime = get_strong();
  if (editor_ == nullptr || !document_.has_value()) {
    co_return;
  }
  auto const selected = selected_software_index();
  if (!selected.has_value()) {
    co_return;
  }
  auto const id = document_->software[*selected].id;
  ContentDialog dialog;
  dialog.XamlRoot(XamlRoot());
  dialog.Title(winrt::box_value(resource_string(L"SoftwareCatalogEditorDeleteTitle")));
  dialog.Content(
      winrt::box_value(resource_string(L"SoftwareCatalogEditorDeleteContent")));
  dialog.PrimaryButtonText(resource_string(L"SoftwareCatalogEditorDeleteConfirm"));
  dialog.CloseButtonText(resource_string(L"SoftwareCatalogEditorDeleteCancel"));
  if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
    project_action(editor_->remove_software(id));
  }
}

void SoftwareCatalogEditorPage::OnSaveDraft(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ != nullptr) {
    project_action(editor_->save_draft());
  }
}

winrt::fire_and_forget SoftwareCatalogEditorPage::OnDeleteSavedDraft(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto lifetime = get_strong();
  if (editor_ == nullptr) {
    co_return;
  }
  ContentDialog dialog;
  dialog.XamlRoot(XamlRoot());
  dialog.Title(winrt::box_value(
      resource_string(L"SoftwareCatalogEditorDeleteSavedDraftTitle")));
  dialog.Content(winrt::box_value(
      resource_string(L"SoftwareCatalogEditorDeleteSavedDraftContent")));
  dialog.PrimaryButtonText(
      resource_string(L"SoftwareCatalogEditorDeleteSavedDraftConfirm"));
  dialog.CloseButtonText(
      resource_string(L"SoftwareCatalogEditorDeleteSavedDraftCancel"));
  if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
    project_action(editor_->delete_saved_draft());
  }
}

void SoftwareCatalogEditorPage::OnApplySavedDraft(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ != nullptr) {
    project_action(editor_->apply_saved_draft());
  }
}

void SoftwareCatalogEditorPage::OnDiscardUnsaved(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ != nullptr) {
    project_action(editor_->discard_unsaved());
  }
}

void SoftwareCatalogEditorPage::OnPreviewImport(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ == nullptr) {
    return;
  }
  auto preview = editor_->preview_manual_import(
      winrt::to_string(ImportPathTextBox().Text()));
  pending_import_token_ = preview.ready ? std::move(preview.confirmation_token)
                                        : std::string{};
  ApplyImportButton().IsEnabled(!pending_import_token_.empty());
  if (preview.ready) {
    project_import_preview(preview);
    set_status(resource_string(L"SoftwareCatalogEditorImportReady"),
               InfoBarSeverity::Informational);
    return;
  }
  clear_import_preview();
  set_status(resource_string(L"SoftwareCatalogEditorImportPreviewFailed"),
             InfoBarSeverity::Error);
}

void SoftwareCatalogEditorPage::OnImportPathChanged(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&) {
  if (!projecting_) {
    clear_import_preview();
  }
}

void SoftwareCatalogEditorPage::OnApplyImport(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (editor_ == nullptr || pending_import_token_.empty()) {
    return;
  }
  auto const token = std::exchange(pending_import_token_, {});
  auto const result = editor_->apply_preview(token);
  if (result.succeeded()) {
    clear_import_preview();
  }
  project_action(result);
}

void SoftwareCatalogEditorPage::refresh() {
  if (editor_ == nullptr) {
    return;
  }
  project(editor_->editor_snapshot());
}

void SoftwareCatalogEditorPage::project(
    azzs::application::DebugModeCatalogEditorSnapshot const& snapshot) {
  projecting_ = true;
  auto const selected_id = selected_software_id();
  auto const can_edit = snapshot.settings.catalog_editor_available ||
                        snapshot.settings.temporary_close_recovery;
  auto const can_import = snapshot.settings.manual_catalog_import_available;
  editing_enabled_ = can_edit;

  LocalTrialInfoBar().IsOpen(
      snapshot.catalog.current.has_value() &&
      snapshot.catalog.current->identity ==
          catalog_app::EffectiveCatalogIdentity::local_trial);
  if (!can_edit && !snapshot.settings.temporary_close_recovery) {
    set_status(resource_string(L"SoftwareCatalogEditorUnavailable"),
               InfoBarSeverity::Warning);
  }
  DraftStateText().Text(draft_state_text(snapshot.catalog.draft.state));

  auto const source = snapshot.catalog.draft.document.has_value()
                          ? snapshot.catalog.draft.document
                          : snapshot.catalog.current_document;
  document_ = source;
  project_validation_issues(snapshot);
  project_document(selected_id);
  auto const has_selection = selected_software_index().has_value();
  AddSoftwareButton().IsEnabled(can_edit && document_.has_value());
  AddCategoryButton().IsEnabled(can_edit && document_.has_value());
  DuplicateSoftwareButton().IsEnabled(can_edit && has_selection);
  DeleteSoftwareButton().IsEnabled(can_edit && has_selection);
  SaveDraftButton().IsEnabled(can_edit &&
                              snapshot.catalog.draft.state !=
                                  catalog_app::DraftWorkState::none &&
                              snapshot.catalog.draft.state !=
                                  catalog_app::DraftWorkState::saved_not_applied);
  DeleteSavedDraftButton().IsEnabled(
      snapshot.settings.catalog_editor_available &&
      snapshot.catalog.draft.state ==
          catalog_app::DraftWorkState::saved_not_applied);
  auto const apply_as_local_trial =
      snapshot.catalog.draft.state ==
          catalog_app::DraftWorkState::saved_not_applied &&
      !snapshot.catalog.draft.validation_failed &&
      !snapshot.catalog.draft.release_issues.empty();
  ApplySavedDraftButton().Label(
      resource_string(apply_as_local_trial ? L"SoftwareCatalogEditorApplyLocalTrial"
                                            : L"SoftwareCatalogEditorApply.Label"));
  ApplySavedDraftButton().IsEnabled(
      snapshot.settings.catalog_editor_available &&
      snapshot.catalog.draft.state == catalog_app::DraftWorkState::saved_not_applied);
  DiscardUnsavedButton().IsEnabled(can_edit &&
                                   (snapshot.catalog.draft.state ==
                                        catalog_app::DraftWorkState::unsaved_changes ||
                                    snapshot.catalog.draft.state ==
                                        catalog_app::DraftWorkState::recovered_unsaved));
  ImportPathTextBox().IsEnabled(can_import);
  PreviewImportButton().IsEnabled(can_import);
  ApplyImportButton().IsEnabled(can_import && !pending_import_token_.empty());
  projecting_ = false;
  project_selected_software();
}

void SoftwareCatalogEditorPage::project_validation_issues(
    azzs::application::DebugModeCatalogEditorSnapshot const& snapshot) {
  std::vector<catalog::CatalogIssue> const* runtime_issues{};
  std::vector<catalog::CatalogIssue> const* release_issues{};
  if (snapshot.catalog.draft.state != catalog_app::DraftWorkState::none) {
    runtime_issues = std::addressof(snapshot.catalog.draft.runtime_issues);
    release_issues = std::addressof(snapshot.catalog.draft.release_issues);
  } else if (snapshot.catalog.current.has_value()) {
    runtime_issues = std::addressof(snapshot.catalog.current->local_issues);
    release_issues = std::addressof(snapshot.catalog.current->release_issues);
  }

  RuntimeIssuesInfoBar().Title(
      resource_string(L"SoftwareCatalogEditorRuntimeIssuesTitle"));
  RuntimeIssuesInfoBar().Message(runtime_issues == nullptr
                                      ? winrt::hstring{}
                                      : issue_text(*runtime_issues));
  RuntimeIssuesInfoBar().IsOpen(runtime_issues != nullptr &&
                                !runtime_issues->empty());
  ReleaseIssuesInfoBar().Title(
      resource_string(L"SoftwareCatalogEditorReleaseIssuesTitle"));
  ReleaseIssuesInfoBar().Message(release_issues == nullptr
                                      ? winrt::hstring{}
                                      : issue_text(*release_issues));
  ReleaseIssuesInfoBar().IsOpen(release_issues != nullptr &&
                                !release_issues->empty());
}

void SoftwareCatalogEditorPage::project_import_preview(
    catalog_app::CatalogCandidatePreview const& preview) {
  std::wstring text;
  auto append_line = [&text](winrt::hstring const& label,
                             std::wstring_view value) {
    if (!text.empty()) {
      text += L'\n';
    }
    text += label.c_str();
    text += L": ";
    text += value;
  };
  auto append_message = [&text](winrt::hstring const& message) {
    if (!text.empty()) {
      text += L'\n';
    }
    text += message.c_str();
  };

  append_line(resource_string(L"SoftwareCatalogEditorImportPreviewPath"),
              winrt::to_hstring(preview.path).c_str());
  append_line(resource_string(L"SoftwareCatalogEditorImportPreviewRevision"),
              std::to_wstring(preview.revision));
  append_line(resource_string(L"SoftwareCatalogEditorImportPreviewItemCount"),
              std::to_wstring(preview.item_count));
  append_message(resource_string(L"SoftwareCatalogEditorImportPreviewFutureOperations"));

  if (preview.downgrade) {
    std::vector<std::string> affected = preview.selection_impact.removed;
    affected.insert(affected.end(), preview.selection_impact.changed.begin(),
                    preview.selection_impact.changed.end());
    std::ranges::sort(affected);
    affected.erase(std::unique(affected.begin(), affected.end()), affected.end());
    append_message(resource_string(L"SoftwareCatalogEditorImportPreviewDowngrade"));
    if (affected.empty()) {
      append_message(
          resource_string(L"SoftwareCatalogEditorImportPreviewDowngradeNoItems"));
    } else {
      append_line(resource_string(L"SoftwareCatalogEditorImportPreviewAffectedItems"),
                  winrt::to_hstring(join_values(affected)).c_str());
    }
  }

  ImportPreviewText().Text(winrt::hstring{std::move(text)});
}

void SoftwareCatalogEditorPage::project_document(
    std::optional<std::string> selected_id) {
  auto const was_projecting = std::exchange(projecting_, true);
  visible_software_indices_.clear();
  SoftwareList().Items().Clear();
  if (!document_.has_value()) {
    projecting_ = was_projecting;
    if (!was_projecting) {
      project_selected_software();
    }
    return;
  }
  auto const filter = lower_ascii(winrt::to_string(SearchTextBox().Text()));
  for (std::size_t index = 0; index < document_->software.size(); ++index) {
    auto const& software = document_->software[index];
    auto const searchable = lower_ascii(software.id + " " + software.name);
    if (!filter.empty() && searchable.find(filter) == std::string::npos) {
      continue;
    }
    auto label = software.id + " - " + software.name;
    if (!software.enabled) {
      label += winrt::to_string(
          resource_string(L"SoftwareCatalogEditorDisabledSuffix"));
    }
    visible_software_indices_.push_back(index);
    SoftwareList().Items().Append(winrt::box_value(winrt::to_hstring(label)));
  }
  if (!visible_software_indices_.empty()) {
    auto selected = std::find_if(
        visible_software_indices_.begin(), visible_software_indices_.end(),
        [&](std::size_t index) {
          return selected_id.has_value() &&
                 document_->software[index].id == *selected_id;
        });
    SoftwareList().SelectedIndex(
        selected == visible_software_indices_.end()
            ? 0
            : static_cast<int>(std::distance(visible_software_indices_.begin(),
                                             selected)));
  }
  projecting_ = was_projecting;
  if (!was_projecting) {
    project_selected_software();
  }
}

void SoftwareCatalogEditorPage::project_selected_software() {
  projecting_ = true;
  auto const selected = selected_software_index();
  auto const enabled =
      editing_enabled_ && selected.has_value() && document_.has_value();
  SoftwareIdTextBox().IsEnabled(enabled);
  SoftwareNameTextBox().IsEnabled(enabled);
  SoftwareBranchTextBox().IsEnabled(enabled);
  SoftwareCategoryComboBox().IsEnabled(enabled);
  SoftwareTierComboBox().IsEnabled(enabled);
  SoftwareVersionPolicyComboBox().IsEnabled(enabled);
  SoftwareFixedVersionTextBox().IsEnabled(enabled);
  SoftwareInstallProfileTextBox().IsEnabled(enabled);
  SoftwareDependenciesTextBox().IsEnabled(enabled);
  SoftwareBundledEditionsTextBox().IsEnabled(enabled);
  SoftwareNoticeTextBox().IsEnabled(enabled);
  SoftwareEnabledToggle().IsEnabled(enabled);
  if (enabled) {
    auto const& software = document_->software[*selected];
    SoftwareIdTextBox().Text(winrt::to_hstring(software.id));
    SoftwareNameTextBox().Text(winrt::to_hstring(software.name));
    SoftwareBranchTextBox().Text(winrt::to_hstring(software.branch));
    SoftwareTierComboBox().SelectedIndex(tier_index(software.tier));
    SoftwareVersionPolicyComboBox().SelectedIndex(
        version_policy_index(software.version_policy));
    SoftwareFixedVersionTextBox().Text(
        winrt::to_hstring(software.fixed_version.value_or(std::string{})));
    SoftwareInstallProfileTextBox().Text(
        winrt::to_hstring(software.install_profile.value_or(std::string{})));
    SoftwareDependenciesTextBox().Text(
        winrt::to_hstring(join_values(software.dependencies)));
    SoftwareBundledEditionsTextBox().Text(
        winrt::to_hstring(join_values(software.bundled_editions)));
    SoftwareNoticeTextBox().Text(winrt::to_hstring(software.notice));
    SoftwareEnabledToggle().IsOn(software.enabled);
  } else {
    SoftwareIdTextBox().Text({});
    SoftwareNameTextBox().Text({});
    SoftwareBranchTextBox().Text({});
    SoftwareTierComboBox().SelectedIndex(-1);
    SoftwareVersionPolicyComboBox().SelectedIndex(-1);
    SoftwareFixedVersionTextBox().Text({});
    SoftwareInstallProfileTextBox().Text({});
    SoftwareDependenciesTextBox().Text({});
    SoftwareBundledEditionsTextBox().Text({});
    SoftwareNoticeTextBox().Text({});
    SoftwareEnabledToggle().IsOn(false);
  }
  project_categories();
  project_sources();
  project_localizations();
  projecting_ = false;
}

void SoftwareCatalogEditorPage::project_categories() {
  auto const was_projecting = std::exchange(projecting_, true);
  std::optional<std::string> selected_category_id;
  if (auto const selected = selected_category_index(); selected.has_value() &&
      document_.has_value()) {
    selected_category_id = document_->categories[*selected].id;
  }
  CategoryList().Items().Clear();
  SoftwareCategoryComboBox().Items().Clear();
  auto const editable = editing_enabled_ && document_.has_value();
  CategoryList().IsEnabled(editable);
  CategoryIdTextBox().IsEnabled(editable);
  CategoryNameTextBox().IsEnabled(editable);
  if (!document_.has_value()) {
    CategoryList().SelectedIndex(-1);
    SoftwareCategoryComboBox().SelectedIndex(-1);
    project_selected_category();
    project_category_localizations();
    projecting_ = was_projecting;
    return;
  }

  auto const software = selected_software_index();
  auto selected_category = -1;
  auto software_category = -1;
  for (std::size_t index = 0; index < document_->categories.size(); ++index) {
    auto const& category = document_->categories[index];
    CategoryList().Items().Append(winrt::box_value(
        winrt::to_hstring(category.id + " - " + category.name)));
    SoftwareCategoryComboBox().Items().Append(
        winrt::box_value(winrt::to_hstring(category.id)));
    if (selected_category_id.has_value() &&
        *selected_category_id == category.id) {
      selected_category = static_cast<int>(index);
    }
    if (software.has_value() &&
        document_->software[*software].category_id == category.id) {
      software_category = static_cast<int>(index);
    }
  }
  if (selected_category < 0) {
    selected_category = software_category;
  }
  CategoryList().SelectedIndex(selected_category);
  SoftwareCategoryComboBox().SelectedIndex(software_category);
  project_selected_category();
  project_category_localizations();
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_selected_category() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const selected = selected_category_index();
  auto const enabled = editing_enabled_ && selected.has_value() &&
                       document_.has_value();
  CategoryIdTextBox().IsEnabled(enabled);
  CategoryNameTextBox().IsEnabled(enabled);
  if (enabled) {
    auto const& category = document_->categories[*selected];
    CategoryIdTextBox().Text(winrt::to_hstring(category.id));
    CategoryNameTextBox().Text(winrt::to_hstring(category.name));
  } else {
    CategoryIdTextBox().Text({});
    CategoryNameTextBox().Text({});
  }
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_category_localizations() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const selected_index = CategoryLocalizationList().SelectedIndex();
  CategoryLocalizationList().Items().Clear();
  auto const category = selected_category_index();
  auto const enabled = editing_enabled_ && category.has_value() &&
                       document_.has_value();
  AddCategoryLocalizationButton().IsEnabled(enabled);
  if (category.has_value() && document_.has_value()) {
    for (auto const& localization :
         document_->categories[*category].localizations) {
      CategoryLocalizationList().Items().Append(
          winrt::box_value(winrt::to_hstring(localization.locale)));
    }
  }
  CategoryLocalizationList().IsEnabled(enabled);
  auto const size = CategoryLocalizationList().Items().Size();
  CategoryLocalizationList().SelectedIndex(
      size == 0 ? -1 : std::min<int>(selected_index < 0 ? 0 : selected_index,
                                     static_cast<int>(size) - 1));
  project_selected_category_localization();
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_selected_category_localization() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const category = selected_category_index();
  auto const selected = selected_category_localization_index();
  auto const enabled = editing_enabled_ && category.has_value() &&
                       selected.has_value() && document_.has_value();
  DeleteCategoryLocalizationButton().IsEnabled(enabled);
  CategoryLocalizationLocaleTextBox().IsEnabled(enabled);
  CategoryLocalizationNameTextBox().IsEnabled(enabled);
  if (enabled) {
    auto const& localization =
        document_->categories[*category].localizations[*selected];
    CategoryLocalizationLocaleTextBox().Text(
        winrt::to_hstring(localization.locale));
    CategoryLocalizationNameTextBox().Text(
        winrt::to_hstring(localization.name.value_or(std::string{})));
  } else {
    CategoryLocalizationLocaleTextBox().Text({});
    CategoryLocalizationNameTextBox().Text({});
  }
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_sources() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const selected_index = SourceList().SelectedIndex();
  SourceList().Items().Clear();
  auto const software = selected_software_index();
  auto const enabled = editing_enabled_ && software.has_value() &&
                       document_.has_value();
  AddSourceButton().IsEnabled(enabled);
  if (software.has_value() && document_.has_value()) {
    for (auto const& source : document_->software[*software].sources) {
      SourceList().Items().Append(
          winrt::box_value(winrt::to_hstring(source_label(source))));
    }
  }
  SourceList().IsEnabled(enabled);
  auto const size = SourceList().Items().Size();
  SourceList().SelectedIndex(
      size == 0 ? -1 : std::min<int>(selected_index < 0 ? 0 : selected_index,
                                     static_cast<int>(size) - 1));
  project_selected_source();
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_selected_source() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const software = selected_software_index();
  auto const selected = selected_source_index();
  auto const enabled = editing_enabled_ && software.has_value() &&
                       selected.has_value() && document_.has_value();
  DeleteSourceButton().IsEnabled(enabled);
  SourcePurposeComboBox().IsEnabled(enabled);
  SourceAddressTextBox().IsEnabled(enabled);
  SourceVersionTextBox().IsEnabled(enabled);
  if (enabled) {
    auto const& source = document_->software[*software].sources[*selected];
    SourcePurposeComboBox().SelectedIndex(source_purpose_index(source.purpose));
    SourceAddressTextBox().Text(winrt::to_hstring(source.address));
    SourceVersionTextBox().Text(
        winrt::to_hstring(source.version.value_or(std::string{})));
  } else {
    SourcePurposeComboBox().SelectedIndex(-1);
    SourceAddressTextBox().Text({});
    SourceVersionTextBox().Text({});
  }
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_localizations() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const selected_index = LocalizationList().SelectedIndex();
  LocalizationList().Items().Clear();
  auto const software = selected_software_index();
  auto const enabled = editing_enabled_ && software.has_value() &&
                       document_.has_value();
  AddLocalizationButton().IsEnabled(enabled);
  if (software.has_value() && document_.has_value()) {
    for (auto const& localization : document_->software[*software].localizations) {
      LocalizationList().Items().Append(
          winrt::box_value(winrt::to_hstring(localization.locale)));
    }
  }
  LocalizationList().IsEnabled(enabled);
  auto const size = LocalizationList().Items().Size();
  LocalizationList().SelectedIndex(
      size == 0 ? -1 : std::min<int>(selected_index < 0 ? 0 : selected_index,
                                     static_cast<int>(size) - 1));
  project_selected_localization();
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::project_selected_localization() {
  auto const was_projecting = std::exchange(projecting_, true);
  auto const software = selected_software_index();
  auto const selected = selected_localization_index();
  auto const enabled = editing_enabled_ && software.has_value() &&
                       selected.has_value() && document_.has_value();
  DeleteLocalizationButton().IsEnabled(enabled);
  LocalizationLocaleTextBox().IsEnabled(enabled);
  LocalizationNameTextBox().IsEnabled(enabled);
  LocalizationNoticeTextBox().IsEnabled(enabled);
  LocalizationOptimizationNoteTextBox().IsEnabled(enabled);
  LocalizationEducationDescriptionTextBox().IsEnabled(enabled);
  if (enabled) {
    auto const& localization =
        document_->software[*software].localizations[*selected];
    LocalizationLocaleTextBox().Text(winrt::to_hstring(localization.locale));
    LocalizationNameTextBox().Text(
        winrt::to_hstring(localization.name.value_or(std::string{})));
    LocalizationNoticeTextBox().Text(
        winrt::to_hstring(localization.notice.value_or(std::string{})));
    LocalizationOptimizationNoteTextBox().Text(winrt::to_hstring(
        localization.optimization_note.value_or(std::string{})));
    LocalizationEducationDescriptionTextBox().Text(winrt::to_hstring(
        localization.education_description.value_or(std::string{})));
  } else {
    LocalizationLocaleTextBox().Text({});
    LocalizationNameTextBox().Text({});
    LocalizationNoticeTextBox().Text({});
    LocalizationOptimizationNoteTextBox().Text({});
    LocalizationEducationDescriptionTextBox().Text({});
  }
  projecting_ = was_projecting;
}

void SoftwareCatalogEditorPage::commit_selected_software() {
  if (editor_ == nullptr || !document_.has_value() || !editing_enabled_) {
    return;
  }
  auto const selected = selected_software_index();
  if (!selected.has_value()) {
    return;
  }
  auto& software = document_->software[*selected];
  software.id = winrt::to_string(SoftwareIdTextBox().Text());
  software.name = winrt::to_string(SoftwareNameTextBox().Text());
  software.branch = winrt::to_string(SoftwareBranchTextBox().Text());
  auto const category = SoftwareCategoryComboBox().SelectedIndex();
  software.category_id =
      category >= 0 && static_cast<std::size_t>(category) <
                           document_->categories.size()
          ? document_->categories[static_cast<std::size_t>(category)].id
          : std::string{};
  software.tier = tier_from_index(SoftwareTierComboBox().SelectedIndex());
  software.version_policy =
      version_policy_from_index(SoftwareVersionPolicyComboBox().SelectedIndex());
  software.fixed_version = optional_text(SoftwareFixedVersionTextBox().Text());
  software.install_profile =
      optional_text(SoftwareInstallProfileTextBox().Text());
  software.dependencies =
      split_values(winrt::to_string(SoftwareDependenciesTextBox().Text()));
  software.dependencies_declared = true;
  software.bundled_editions =
      split_values(winrt::to_string(SoftwareBundledEditionsTextBox().Text()));
  software.bundled_editions_declared = true;
  software.notice = winrt::to_string(SoftwareNoticeTextBox().Text());
  software.enabled = SoftwareEnabledToggle().IsOn();
  software.enabled_declared = true;
  commit_document();
}

void SoftwareCatalogEditorPage::commit_selected_category() {
  if (!document_.has_value() || !editing_enabled_) {
    return;
  }
  auto const selected = selected_category_index();
  if (!selected.has_value()) {
    return;
  }
  auto& category = document_->categories[*selected];
  auto const previous_id = category.id;
  category.id = winrt::to_string(CategoryIdTextBox().Text());
  category.name = winrt::to_string(CategoryNameTextBox().Text());
  if (category.id != previous_id) {
    for (auto& software : document_->software) {
      if (software.category_id == previous_id) {
        software.category_id = category.id;
      }
    }
  }
  commit_document();
}

void SoftwareCatalogEditorPage::commit_selected_category_localization() {
  if (!document_.has_value() || !editing_enabled_) {
    return;
  }
  auto const category = selected_category_index();
  auto const selected = selected_category_localization_index();
  if (!category.has_value() || !selected.has_value()) {
    return;
  }
  auto& localization =
      document_->categories[*category].localizations[*selected];
  localization.locale =
      winrt::to_string(CategoryLocalizationLocaleTextBox().Text());
  localization.name = optional_text(CategoryLocalizationNameTextBox().Text());
  commit_document();
}

void SoftwareCatalogEditorPage::commit_selected_source() {
  if (!document_.has_value() || !editing_enabled_) {
    return;
  }
  auto const software = selected_software_index();
  auto const selected = selected_source_index();
  if (!software.has_value() || !selected.has_value()) {
    return;
  }
  auto& source = document_->software[*software].sources[*selected];
  source.purpose =
      source_purpose_from_index(SourcePurposeComboBox().SelectedIndex());
  source.address = winrt::to_string(SourceAddressTextBox().Text());
  source.version = optional_text(SourceVersionTextBox().Text());
  commit_document();
}

void SoftwareCatalogEditorPage::commit_selected_localization() {
  if (!document_.has_value() || !editing_enabled_) {
    return;
  }
  auto const software = selected_software_index();
  auto const selected = selected_localization_index();
  if (!software.has_value() || !selected.has_value()) {
    return;
  }
  auto& localization = document_->software[*software].localizations[*selected];
  localization.locale = winrt::to_string(LocalizationLocaleTextBox().Text());
  localization.name = optional_text(LocalizationNameTextBox().Text());
  localization.notice = optional_text(LocalizationNoticeTextBox().Text());
  localization.optimization_note =
      optional_text(LocalizationOptimizationNoteTextBox().Text());
  localization.education_description =
      optional_text(LocalizationEducationDescriptionTextBox().Text());
  commit_document();
}

void SoftwareCatalogEditorPage::commit_document() {
  if (editor_ != nullptr && document_.has_value()) {
    project_action(editor_->edit_document(*document_));
  }
}

void SoftwareCatalogEditorPage::project_action(
    catalog_app::CatalogActionResult const& result) {
  refresh();
  set_status(resource_string(catalog_action_key(result.code)),
             catalog_action_severity(result));
  if (catalog_action_succeeded(result) && result.current_changed) {
    ApplySavedDraftButton().Focus(Microsoft::UI::Xaml::FocusState::Programmatic);
  }
}

void SoftwareCatalogEditorPage::clear_import_preview() {
  pending_import_token_.clear();
  ImportPreviewText().Text({});
  ApplyImportButton().IsEnabled(false);
}

void SoftwareCatalogEditorPage::set_status(
    winrt::hstring const& message, InfoBarSeverity severity) {
  StatusInfoBar().Title(resource_string(L"SoftwareCatalogEditorStatusTitle"));
  StatusInfoBar().Message(message);
  StatusInfoBar().Severity(severity);
  StatusInfoBar().IsOpen(!message.empty());
}

std::optional<std::size_t> SoftwareCatalogEditorPage::selected_software_index() {
  auto const selected = SoftwareList().SelectedIndex();
  if (!document_.has_value() || selected < 0 ||
      static_cast<std::size_t>(selected) >= visible_software_indices_.size()) {
    return std::nullopt;
  }
  auto const document_index =
      visible_software_indices_[static_cast<std::size_t>(selected)];
  return document_index < document_->software.size()
             ? std::optional<std::size_t>{document_index}
             : std::nullopt;
}

std::optional<std::string> SoftwareCatalogEditorPage::selected_software_id() {
  auto const selected = selected_software_index();
  return selected.has_value()
             ? std::optional<std::string>{document_->software[*selected].id}
             : std::nullopt;
}

std::optional<std::size_t> SoftwareCatalogEditorPage::selected_category_index() {
  auto const selected = CategoryList().SelectedIndex();
  if (!document_.has_value() || selected < 0 ||
      static_cast<std::size_t>(selected) >= document_->categories.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selected);
}

std::optional<std::size_t>
SoftwareCatalogEditorPage::selected_category_localization_index() {
  auto const category = selected_category_index();
  auto const selected = CategoryLocalizationList().SelectedIndex();
  if (!document_.has_value() || !category.has_value() || selected < 0 ||
      static_cast<std::size_t>(selected) >=
          document_->categories[*category].localizations.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selected);
}

std::optional<std::size_t> SoftwareCatalogEditorPage::selected_source_index() {
  auto const software = selected_software_index();
  auto const selected = SourceList().SelectedIndex();
  if (!document_.has_value() || !software.has_value() || selected < 0 ||
      static_cast<std::size_t>(selected) >=
          document_->software[*software].sources.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selected);
}

std::optional<std::size_t>
SoftwareCatalogEditorPage::selected_localization_index() {
  auto const software = selected_software_index();
  auto const selected = LocalizationList().SelectedIndex();
  if (!document_.has_value() || !software.has_value() || selected < 0 ||
      static_cast<std::size_t>(selected) >=
          document_->software[*software].localizations.size()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(selected);
}

std::string SoftwareCatalogEditorPage::next_software_id() const {
  if (!document_.has_value()) {
    return "new-software";
  }
  auto sequence = next_software_sequence_;
  while (true) {
    auto const id = "new-software-" + std::to_string(sequence);
    auto const duplicate = std::any_of(
        document_->software.begin(), document_->software.end(),
        [&id](catalog::SoftwareDefinition const& software) {
          return software.id == id;
        });
    if (!duplicate) {
      return id;
    }
    ++sequence;
  }
}

std::string SoftwareCatalogEditorPage::next_category_id() const {
  if (!document_.has_value()) {
    return "new-category";
  }
  auto sequence = next_category_sequence_;
  while (true) {
    auto const id = "new-category-" + std::to_string(sequence);
    auto const duplicate = std::any_of(
        document_->categories.begin(), document_->categories.end(),
        [&id](catalog::CatalogCategory const& category) {
          return category.id == id;
        });
    if (!duplicate) {
      return id;
    }
    ++sequence;
  }
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
