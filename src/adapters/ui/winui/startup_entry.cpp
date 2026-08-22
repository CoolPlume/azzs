#include "pch.h"

#include "resource_fallback.hpp"

// App.xaml.g.hpp renames its generated entry point when the project defines
// DISABLE_XAML_GENERATED_MAIN. Keep the wrapper at the process boundary so
// failures raised before App::App() still produce a visible diagnostic.
int __stdcall wXamlGeneratedMain(HINSTANCE, HINSTANCE, PWSTR, int);

namespace {

void show_startup_failure() noexcept {
  ::OutputDebugStringW(L"Azzs WinUI startup failed before the application was created.\n");
  auto const title = azzs::ui::winui::compiled_resource_string(
      IDS_AZZS_STARTUP_FAILURE_TITLE);
  auto const message = azzs::ui::winui::compiled_resource_string(
      IDS_AZZS_STARTUP_FAILURE_UNEXPECTED);
  (void)::MessageBoxW(
      nullptr, message.empty() ? L"Startup failure." : message.c_str(),
      title.empty() ? L"Windows Initial Setup Workbench" : title.c_str(),
      MB_OK | MB_ICONERROR | MB_TOPMOST);
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
