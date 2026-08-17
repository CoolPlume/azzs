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
  try {
    auto startup = azzs::composition::windows::assemble_startup();
    startup_status_ = std::move(startup.status);
    window_ = std::move(startup.window);
  } catch (...) {
    // The composition root normally converts failures to a static failure
    // window. This boundary only protects the process if that presentation
    // itself cannot be created.
    startup_status_ = azzs::application::startup::startup_assembly_failed(
        azzs::application::startup::StartupAssemblyStage::
            unexpected_assembly_exception);
    window_ = nullptr;
  }
}

}  // namespace winrt::Azzs::Ui::implementation
