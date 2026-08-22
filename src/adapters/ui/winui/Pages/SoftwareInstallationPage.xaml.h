#pragma once

#include <memory>

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.h"
#include "DesignSystem/presentation_contract.hpp"
#include "Pages/SoftwareInstallationPage.g.h"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/installation_batch_creation.hpp"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/workbench_services.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage> {
  SoftwareInstallationPage();
  void bind(std::shared_ptr<azzs::application::WorkbenchServices> services);
  void OnCreateBatch(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnSoftwareSelectionChanged(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  void project(
      azzs::application::software_selection::SoftwareSelectionSnapshot const& selection,
      azzs::application::offline_package_cache::OfflinePackageCacheSnapshot const& cache,
      azzs::domain::installation_batch::InstallationBatchSnapshot const& batch);
  void refresh();
  void handle_installation_batch_intent(
      azzs::ui::presentation::PresentationIntent const& intent);
  void set_catalog_status(winrt::hstring const& message,
                          Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);

  std::shared_ptr<azzs::application::WorkbenchServices> services_;
  bool projecting_{false};
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage,
                                implementation::SoftwareInstallationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
