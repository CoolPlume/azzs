#pragma once

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.h"
#include "Pages/SoftwareInstallationPage.g.h"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_selection.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage> {
  SoftwareInstallationPage();
  void project(
      azzs::application::software_selection::SoftwareSelectionSnapshot const& selection,
      azzs::application::offline_package_cache::OfflinePackageCacheSnapshot const& cache);
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage,
                                implementation::SoftwareInstallationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
