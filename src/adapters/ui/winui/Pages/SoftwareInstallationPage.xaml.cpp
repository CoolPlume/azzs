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
  }, {});
}

void SoftwareInstallationPage::project(
    azzs::application::software_selection::SoftwareSelectionSnapshot const& snapshot,
    azzs::application::offline_package_cache::OfflinePackageCacheSnapshot const& cache) {
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
  using SurfaceImplementation = winrt::Azzs::Ui::DesignSystem::Controls::
      implementation::ReadOnlyPresentationSurface;
  winrt::get_self<SurfaceImplementation>(SoftwareSelectionStatus())->project(
      azzs::ui::presentation::make_software_selection_presentation(
          snapshot, std::move(text)),
      "software-selection.status", azzs::ui::presentation::ViewMode::standard,
      {}, 0, "SoftwareInstallation");
  auto cache_text = azzs::ui::presentation::OfflinePackageCachePresentationText{
      .accessible_name = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheStatusAccessibleName")),
      .available_title = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheAvailableTitle")),
      .unavailable_title = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheUnavailableTitle")),
      .available_body_prefix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheAvailableBodyPrefix")),
      .unavailable_body_prefix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheUnavailableBodyPrefix")),
      .item_suffix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheItemSuffix")),
      .network_suffix = winrt::to_string(
          resources.GetString(L"OfflinePackageCacheNetworkSuffix")),
  };
  winrt::get_self<SurfaceImplementation>(OfflinePackageCacheStatus())->project(
      azzs::ui::presentation::make_offline_package_cache_presentation(
          cache, std::move(cache_text)),
      "offline-package-cache.status", azzs::ui::presentation::ViewMode::standard,
      {}, 0, "SoftwareInstallation");
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
