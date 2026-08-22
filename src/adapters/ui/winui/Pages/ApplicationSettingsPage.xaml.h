#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "Pages/ApplicationSettingsPage.g.h"
#include "azzs/application/application_settings.hpp"
#include "azzs/application/workbench.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage> {
  using AdvancedViewChangedHandler = std::function<bool(bool)>;
  using CatalogEditorRequestedHandler = std::function<void()>;

  ApplicationSettingsPage();

  void bind(std::shared_ptr<azzs::application::Workbench> workbench,
            azzs::application::ApplicationSettingsService& settings,
            azzs::application::UpdateSnapshot const& update_snapshot,
            azzs::application::ApplicationSettingsSnapshot const& snapshot,
            bool advanced_view,
            AdvancedViewChangedHandler advanced_view_changed,
            CatalogEditorRequestedHandler catalog_editor_requested);
  void OnAdvancedViewToggled(
       Windows::Foundation::IInspectable const&,
       Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnCacheRetentionSelectionChanged(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  void OnArchitecturePreferenceSelectionChanged(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  winrt::fire_and_forget OnClearCacheClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  winrt::fire_and_forget OnClearLogsClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnExportDiagnosticClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnRecoveryRecordSelectionChanged(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
  winrt::fire_and_forget OnDeleteRecoveryRecordClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  winrt::fire_and_forget OnCatalogActionClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDebugModeToggled(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnOpenCatalogEditorClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateCommandClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateRetryClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateRestoreClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateManualClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplicationUpdateDiagnosticClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void project(azzs::application::ApplicationSettingsSnapshot const& snapshot);
  void project_update(azzs::application::UpdateSnapshot const& snapshot);
  void project_action(azzs::application::ApplicationSettingsActionResult const& result);
  void project_recovery_selection();
  [[nodiscard]] std::optional<std::uint64_t> selected_recovery_record();
  [[nodiscard]] static bool recovery_record_is_protected(
      azzs::application::RecoveryRecordStatus status) noexcept;

  std::shared_ptr<azzs::application::Workbench> workbench_;
  azzs::application::ApplicationSettingsService* settings_{};
  AdvancedViewChangedHandler advanced_view_changed_;
  CatalogEditorRequestedHandler catalog_editor_requested_;
  std::vector<azzs::application::SystemSettingsRecoveryRecord>
      recovery_records_;
  bool advanced_view_{false};
  bool projecting_{false};
  bool confirmation_dialog_open_{false};
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage,
                               implementation::ApplicationSettingsPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
