#include "pch.h"

#include "App.xaml.h"
#include "../../../composition/windows/composition_root.hpp"
#include "native_resource_fallback.hpp"

#include <utility>

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

namespace winrt::Azzs::Ui::implementation {
namespace {

void show_last_resort_startup_failure(unsigned int message_id) noexcept {
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

  auto const title = native_resources::load_string(
      AZZS_NATIVE_STRING_APP_STARTUP_FAILURE_TITLE);
  auto const message = native_resources::load_string(message_id);
  if (!title.empty() && !message.empty()) {
    ::OutputDebugStringW(message.c_str());
    ::OutputDebugStringW(L"\n");
    (void)::MessageBoxW(nullptr, message.c_str(), title.c_str(),
                        MB_OK | MB_ICONERROR | MB_TOPMOST);
  } else {
    ::OutputDebugStringW(L"WinUI startup failure resources unavailable.\n");
  }
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
    show_last_resort_startup_failure(
        AZZS_NATIVE_STRING_APP_STARTUP_FAILURE_RESOURCES);
    return;
  }

  try {
    auto startup = azzs::composition::windows::assemble_startup();
    startup_status_ = std::move(startup.status);
    window_ = std::move(startup.window);
    if (!window_) {
      show_last_resort_startup_failure(
          AZZS_NATIVE_STRING_APP_STARTUP_FAILURE_WINDOW);
    }
  } catch (...) {
    // The composition root normally converts failures to a static failure
    // window. This boundary also covers the failure presenter itself. Keep a
    // no-throw platform-level signal instead of returning with no active window.
    record_unexpected_startup_failure(startup_status_);
    window_ = nullptr;
    show_last_resort_startup_failure(
        AZZS_NATIVE_STRING_APP_STARTUP_FAILURE_UNEXPECTED);
  }
}

}  // namespace winrt::Azzs::Ui::implementation
