#pragma once

#include "App.xaml.g.h"

namespace winrt::Azzs::Ui::implementation {

struct App : AppT<App> {
  App();

  void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

 private:
  Microsoft::UI::Xaml::Window window_{nullptr};
};

}  // namespace winrt::Azzs::Ui::implementation
