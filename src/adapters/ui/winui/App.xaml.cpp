#include "pch.h"

#include "App.xaml.h"
#include "../../../composition/windows/composition_root.hpp"

#include <utility>

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

namespace winrt::Azzs::Ui::implementation {

App::App() {
  InitializeComponent();
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  auto startup = azzs::composition::windows::assemble_startup();
  window_ = std::move(startup.window);
}

}  // namespace winrt::Azzs::Ui::implementation
