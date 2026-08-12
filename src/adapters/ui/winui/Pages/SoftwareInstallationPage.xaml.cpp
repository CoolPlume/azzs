#include "pch.h"

#include "SoftwareInstallationPage.xaml.h"

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.xaml.h"
#include "DesignSystem/software_selection_presentation.hpp"

#if __has_include("Pages/SoftwareInstallationPage.g.cpp")
#include "Pages/SoftwareInstallationPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {

SoftwareInstallationPage::SoftwareInstallationPage() {
  InitializeComponent();
  project({
      .mode = azzs::application::software_selection::SelectionLifecycleMode::
          not_restored,
  });
}

void SoftwareInstallationPage::project(
    azzs::application::software_selection::SoftwareSelectionSnapshot const&
        snapshot) {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  auto const resources = ResourceLoader{};
  auto text = azzs::ui::presentation::SoftwareSelectionPresentationText{
      .accessible_name = winrt::to_string(
          resources.GetString(L"SoftwareSelectionStatusAccessibleName")),
      .available_title = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAvailableTitle")),
      .available_body_prefix = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAvailableBodyPrefix")),
      .available_body_suffix = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAvailableBodySuffix")),
      .absent_catalog_title = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAbsentCatalogTitle")),
      .absent_catalog_body = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAbsentCatalogBody")),
      .not_restored_body = winrt::to_string(
          resources.GetString(L"SoftwareSelectionNotRestoredBody")),
      .restore_failed_body = winrt::to_string(
          resources.GetString(L"SoftwareSelectionRestoreFailedBody")),
      .advanced_available = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAdvancedAvailable")),
      .advanced_absent_catalog = winrt::to_string(
          resources.GetString(L"SoftwareSelectionAdvancedAbsentCatalog")),
  };
  SoftwareSelectionStatus().project(
      azzs::ui::presentation::make_software_selection_presentation(
          snapshot, std::move(text)),
      "software-selection.status", azzs::ui::presentation::ViewMode::standard,
      {}, 0, "SoftwareInstallation");
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
