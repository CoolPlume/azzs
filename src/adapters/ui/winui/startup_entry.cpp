#include "pch.h"
#include "native_resource_fallback.hpp"

// App.xaml.g.hpp renames its generated entry point when the project defines
// DISABLE_XAML_GENERATED_MAIN. Keep the wrapper at the process boundary so
// failures raised before App::App() still produce a visible diagnostic.
int __stdcall wXamlGeneratedMain(HINSTANCE, HINSTANCE, PWSTR, int);

namespace {

void show_startup_failure() noexcept {
  ::OutputDebugStringW(L"Azzs WinUI startup failed before the application was created.\n");
  auto const title = azzs::ui::winui::native_resources::load_string(
      AZZS_NATIVE_STRING_APP_STARTUP_FAILURE_TITLE);
  auto const message = azzs::ui::winui::native_resources::load_string(
      AZZS_NATIVE_STRING_STARTUP_FAILURE);
  if (!title.empty() && !message.empty()) {
    (void)::MessageBoxW(nullptr, message.c_str(), title.c_str(),
                        MB_OK | MB_ICONERROR | MB_TOPMOST);
  } else {
    ::OutputDebugStringW(L"WinUI startup failure resources unavailable.\n");
  }
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
