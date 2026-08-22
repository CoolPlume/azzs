#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "Pages/SoftwareCatalogEditorPage.g.h"
#include "azzs/application/debug_mode_catalog_editor.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareCatalogEditorPage
    : SoftwareCatalogEditorPageT<SoftwareCatalogEditorPage> {
  SoftwareCatalogEditorPage();

  void bind(azzs::application::DebugModeCatalogEditor& editor);
  void show_action_result(
      azzs::application::software_catalog::CatalogActionResult const& result);
  void OnSearchChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& args);
  void OnSoftwareSelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnSoftwareFieldLostFocus(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnSoftwareFieldSelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnSoftwareEnabledToggled(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnCategorySelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnCategoryFieldLostFocus(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnAddCategory(Windows::Foundation::IInspectable const& sender,
                     Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnCategoryLocalizationSelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnCategoryLocalizationFieldLostFocus(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnAddCategoryLocalization(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnDeleteCategoryLocalization(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnSourceSelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnSourceFieldSelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnSourceFieldLostFocus(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnAddSource(Windows::Foundation::IInspectable const& sender,
                   Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnDeleteSource(Windows::Foundation::IInspectable const& sender,
                      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnLocalizationSelectionChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
  void OnLocalizationFieldLostFocus(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnAddLocalization(Windows::Foundation::IInspectable const& sender,
                         Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnDeleteLocalization(Windows::Foundation::IInspectable const& sender,
                            Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnAddSoftware(Windows::Foundation::IInspectable const& sender,
                     Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnDuplicateSoftware(Windows::Foundation::IInspectable const& sender,
                           Microsoft::UI::Xaml::RoutedEventArgs const& args);
  winrt::fire_and_forget OnDeleteSoftware(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnSaveDraft(Windows::Foundation::IInspectable const& sender,
                   Microsoft::UI::Xaml::RoutedEventArgs const& args);
  winrt::fire_and_forget OnDeleteSavedDraft(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnApplySavedDraft(Windows::Foundation::IInspectable const& sender,
                          Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnDiscardUnsaved(Windows::Foundation::IInspectable const& sender,
                        Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnPreviewImport(Windows::Foundation::IInspectable const& sender,
                       Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnImportPathChanged(
      Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& args);
  void OnApplyImport(Windows::Foundation::IInspectable const& sender,
                     Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  void refresh();
  void project(azzs::application::DebugModeCatalogEditorSnapshot const& snapshot);
  void project_document(std::optional<std::string> selected_id = {});
  void project_selected_software();
  void project_categories();
  void project_selected_category();
  void project_category_localizations();
  void project_selected_category_localization();
  void project_sources();
  void project_selected_source();
  void project_localizations();
  void project_selected_localization();
  void commit_selected_software();
  void commit_selected_category();
  void commit_selected_category_localization();
  void commit_selected_source();
  void commit_selected_localization();
  void commit_document();
  void project_validation_issues(
      azzs::application::DebugModeCatalogEditorSnapshot const& snapshot);
  void project_import_preview(
      azzs::application::software_catalog::CatalogCandidatePreview const& preview);
  void project_action(
      azzs::application::software_catalog::CatalogActionResult const& result);
  void clear_import_preview();
  void set_status(winrt::hstring const& message,
                  Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);
  [[nodiscard]] std::optional<std::size_t> selected_software_index();
  [[nodiscard]] std::optional<std::string> selected_software_id();
  [[nodiscard]] std::optional<std::size_t> selected_category_index();
  [[nodiscard]] std::optional<std::size_t>
  selected_category_localization_index();
  [[nodiscard]] std::optional<std::size_t> selected_source_index();
  [[nodiscard]] std::optional<std::size_t> selected_localization_index();
  [[nodiscard]] std::string next_software_id() const;
  [[nodiscard]] std::string next_category_id() const;

  azzs::application::DebugModeCatalogEditor* editor_{};
  std::optional<azzs::domain::software_catalog::SoftwareCatalogDocument>
      document_;
  std::vector<std::size_t> visible_software_indices_;
  std::string pending_import_token_;
  std::size_t next_software_sequence_{1};
  std::size_t next_category_sequence_{1};
  bool editing_enabled_{false};
  bool projecting_{false};
  bool confirmation_dialog_open_{false};
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareCatalogEditorPage
    : SoftwareCatalogEditorPageT<SoftwareCatalogEditorPage,
                                 implementation::SoftwareCatalogEditorPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
