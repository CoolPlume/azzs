#pragma once

#include <memory>

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.h"
#include "DesignSystem/presentation_contract.hpp"
#include "Pages/SoftwareInstallationPage.g.h"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/workbench_services.hpp"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage> {
  SoftwareInstallationPage();
  void bind(std::shared_ptr<azzs::application::WorkbenchServices> services);

 private:
  void project(
      azzs::application::software_selection::SoftwareSelectionSnapshot const& selection,
      azzs::application::offline_package_cache::OfflinePackageCacheSnapshot const& cache,
      azzs::domain::installation_batch::InstallationBatchSnapshot const& batch);
  void refresh();
  void handle_installation_batch_intent(
      azzs::ui::presentation::PresentationIntent const& intent);

  std::shared_ptr<azzs::application::WorkbenchServices> services_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage,
                                implementation::SoftwareInstallationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
