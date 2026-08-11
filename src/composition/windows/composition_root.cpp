#include "pch.h"

#include "composition_root.hpp"

#include <memory>

#include "../../adapters/ui/winui/DesignSystem/motion_preferences.hpp"
#include "../../adapters/ui/winui/MainWindow.xaml.h"
#include "azzs/adapters/windows/windows_platform_info.hpp"
#include "azzs/application/workbench.hpp"

namespace azzs::composition::windows {

winrt::Microsoft::UI::Xaml::Window create_main_window() {
  adapters::windows::WindowsPlatformInfo const platform_info;
  auto workbench = std::make_shared<application::Workbench>(platform_info);
  auto motion_preferences = ui::winui::MotionPreferences::create();
  auto window = winrt::make_self<winrt::Azzs::Ui::implementation::MainWindow>();
  window->bind(std::move(workbench), std::move(motion_preferences));
  return window.as<winrt::Microsoft::UI::Xaml::Window>();
}

}  // namespace azzs::composition::windows
