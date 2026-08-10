#pragma once

#include "SoftwareInstallationPage.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage> {
  SoftwareInstallationPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareInstallationPage
    : SoftwareInstallationPageT<SoftwareInstallationPage,
                                implementation::SoftwareInstallationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
