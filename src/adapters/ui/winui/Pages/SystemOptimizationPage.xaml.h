#pragma once

#include "Pages/SystemOptimizationPage.xaml.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct SystemOptimizationPage : SystemOptimizationPageT<SystemOptimizationPage> {
  SystemOptimizationPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SystemOptimizationPage
    : SystemOptimizationPageT<SystemOptimizationPage,
                              implementation::SystemOptimizationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
