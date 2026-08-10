#pragma once

#include "Pages/SoftwareOptimizationPage.xaml.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SoftwareOptimizationPage
    : SoftwareOptimizationPageT<SoftwareOptimizationPage> {
  SoftwareOptimizationPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SoftwareOptimizationPage
    : SoftwareOptimizationPageT<SoftwareOptimizationPage,
                                implementation::SoftwareOptimizationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
