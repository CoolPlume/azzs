#include "pch.h"

#include "App.xaml.h"
#include "../../../composition/windows/composition_root.hpp"

#include <utility>

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

namespace winrt::Azzs::Ui::implementation {
namespace {

void show_last_resort_startup_failure(wchar_t const* message_key) noexcept {
  try {
    using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

    auto const resources = ResourceLoader{};
    auto const title = resources.GetString(L"AppStartupFailureTitle");
    auto const message = resources.GetString(message_key);
    if (!title.empty() && !message.empty()) {
      ::OutputDebugStringW(message.c_str());
      ::OutputDebugStringW(L"\n");
      (void)::MessageBoxW(nullptr, message.c_str(), title.c_str(),
                          MB_OK | MB_ICONERROR | MB_TOPMOST);
      return;
    }
  } catch (...) {
    // Resource loading can fail while the application is still bootstrapping.
  }

  // Keep this last-resort boundary no-throw and independent of WinUI assets,
  // including the case where the loader returns an empty resource.
  ::OutputDebugStringW(L"Startup failure.\n");
  (void)::MessageBoxW(nullptr, L"Startup failure.",
                      L"Windows Initial Setup Workbench",
                      MB_OK | MB_ICONERROR | MB_TOPMOST);
}

void record_unexpected_startup_failure(
    std::optional<::azzs::application::startup::StartupAssemblyStatus>& status)
    noexcept {
  try {
    status = azzs::application::startup::startup_assembly_failed(
        azzs::application::startup::StartupAssemblyStage::
            unexpected_assembly_exception);
  } catch (...) {
    status.reset();
  }
}

}  // namespace

App::App() {
  try {
    InitializeComponent();
  } catch (...) {
    bootstrap_failed_ = true;
    record_unexpected_startup_failure(startup_status_);
  }
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
  if (bootstrap_failed_) {
    show_last_resort_startup_failure(L"AppStartupFailureResources");
    return;
  }

  try {
    auto startup = azzs::composition::windows::assemble_startup();
    startup_status_ = std::move(startup.status);
    window_ = std::move(startup.window);
    if (!window_) {
      show_last_resort_startup_failure(L"AppStartupFailureWindow");
    }
  } catch (...) {
    // The composition root normally converts failures to a static failure
    // window. This boundary also covers the failure presenter itself. Keep a
    // no-throw platform-level signal instead of returning with no active window.
    record_unexpected_startup_failure(startup_status_);
    window_ = nullptr;
    show_last_resort_startup_failure(L"AppStartupFailureUnexpected");
  }
}

}  // namespace winrt::Azzs::Ui::implementation
