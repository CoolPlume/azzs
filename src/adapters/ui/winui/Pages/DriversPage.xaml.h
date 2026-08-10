#pragma once

#include "DriversPage.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct DriversPage : DriversPageT<DriversPage> {
  DriversPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct DriversPage : DriversPageT<DriversPage, implementation::DriversPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
