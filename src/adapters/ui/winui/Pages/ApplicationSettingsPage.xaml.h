#pragma once

#include "Pages/ApplicationSettingsPage.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage> {
  ApplicationSettingsPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct ApplicationSettingsPage
    : ApplicationSettingsPageT<ApplicationSettingsPage,
                               implementation::ApplicationSettingsPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
