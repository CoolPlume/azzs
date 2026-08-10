#pragma once

#include "Pages/OverviewPage.xaml.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct OverviewPage : OverviewPageT<OverviewPage> {
  OverviewPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct OverviewPage : OverviewPageT<OverviewPage, implementation::OverviewPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
