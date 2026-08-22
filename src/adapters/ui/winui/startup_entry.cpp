#include "pch.h"

// App.xaml.g.hpp renames its generated entry point when the project defines
// DISABLE_XAML_GENERATED_MAIN. Keep the wrapper at the process boundary so
// failures raised before App::App() still produce a visible diagnostic.
int __stdcall wXamlGeneratedMain(HINSTANCE, HINSTANCE, PWSTR, int);

namespace {

void show_startup_failure() noexcept {
  ::OutputDebugStringW(L"Azzs WinUI startup failed before the application was created.\n");
  (void)::MessageBoxW(
      nullptr, L"工作台未能启动。请重新启动工作台；如果问题持续，请收集诊断资料。",
      L"无法进入工作台", MB_OK | MB_ICONERROR | MB_TOPMOST);
}

}  // namespace

int __stdcall wWinMain(HINSTANCE instance, HINSTANCE previous_instance,
                       PWSTR command_line, int show_command) {
  try {
    return wXamlGeneratedMain(instance, previous_instance, command_line,
                              show_command);
  } catch (...) {
    show_startup_failure();
    return 1;
  }
}
