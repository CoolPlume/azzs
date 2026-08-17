#include "pch.h"

#include "App.xaml.h"
#include "../../../composition/windows/composition_root.hpp"

#include <utility>

#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif

namespace winrt::Azzs::Ui::implementation {
namespace {

void show_last_resort_startup_failure(wchar_t const* message) noexcept {
  ::OutputDebugStringW(message);
  ::OutputDebugStringW(L"\n");
  (void)::MessageBoxW(nullptr, message, L"无法进入工作台",
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
    show_last_resort_startup_failure(
        L"界面资源初始化失败。请重新启动工作台；如果问题持续，请收集诊断资料。");
    return;
  }

  try {
    auto startup = azzs::composition::windows::assemble_startup();
    startup_status_ = std::move(startup.status);
    window_ = std::move(startup.window);
    if (!window_) {
      show_last_resort_startup_failure(
          L"工作台未能创建启动窗口。请重新启动工作台；如果问题持续，请收集诊断资料。");
    }
  } catch (...) {
    // The composition root normally converts failures to a static failure
    // window. This boundary also covers the failure presenter itself. Keep a
    // no-throw platform-level signal instead of returning with no active window.
    record_unexpected_startup_failure(startup_status_);
    window_ = nullptr;
    show_last_resort_startup_failure(
        L"工作台启动失败。请重新启动工作台；如果问题持续，请收集诊断资料。");
  }
}

}  // namespace winrt::Azzs::Ui::implementation
