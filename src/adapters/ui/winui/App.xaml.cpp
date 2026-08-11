#include "pch.h"

#include "App.xaml.h"
#include "../../../composition/windows/composition_root.hpp"

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

namespace winrt::Azzs::Ui::implementation {

App::App() {
  InitializeComponent();
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  window_ = azzs::composition::windows::create_main_window();
  window_.Activate();
}

}  // namespace winrt::Azzs::Ui::implementation
