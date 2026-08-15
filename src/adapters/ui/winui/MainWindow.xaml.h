#pragma once

#include <memory>
#include <optional>

#include "MainWindow.g.h"
#include <winrt/Microsoft.UI.Windowing.h>
#include "azzs/application/page_id.hpp"
#include "azzs/application/workbench.hpp"

namespace azzs::ui::winui {
class MotionPreferences;
}

namespace azzs::application {
class AdvancedViewPreferences;
class SystemSettingsApplyService;
namespace software_catalog {
struct CatalogActionResult;
}
}

namespace winrt::Azzs::Ui::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  void bind(
      std::shared_ptr<azzs::application::Workbench> workbench,
      std::shared_ptr<azzs::ui::winui::MotionPreferences> motion_preferences,
      std::shared_ptr<azzs::application::SystemSettingsApplyService>
          system_settings,
      std::shared_ptr<azzs::application::AdvancedViewPreferences>
          advanced_view_preferences);
  void OnNavigationSelectionChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const&,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&
          args);
  void OnWindowClosing(
      Microsoft::UI::Windowing::AppWindow const&,
      Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args);

 private:
  [[nodiscard]] std::optional<azzs::application::PageId> page_for_item(
      Microsoft::UI::Xaml::Controls::NavigationViewItem const& item);
  void navigate_to(azzs::application::PageId page);
  void refresh_drivers_page();
  void begin_driver_handoff(
      azzs::application::driver_acquisition::DriverEntrypoint entrypoint);
  void driver_flow_returned();
  void decide_driver_handoff(
      azzs::application::driver_acquisition::DriverHandoffDecision decision);
  void project_drivers_page(
      azzs::application::driver_acquisition::DriverAcquisitionSnapshot const&
          driver_snapshot);
  winrt::fire_and_forget confirm_catalog_close();
  void restore_catalog_editor_after_close(
      azzs::application::software_catalog::CatalogActionResult const& result);
  [[nodiscard]] bool set_advanced_view(bool enabled);
  void project(azzs::application::WorkbenchSnapshot const& snapshot);

  std::shared_ptr<azzs::application::Workbench> workbench_;
  std::shared_ptr<azzs::ui::winui::MotionPreferences> motion_preferences_;
  std::shared_ptr<azzs::application::SystemSettingsApplyService>
      system_settings_;
  std::shared_ptr<azzs::application::AdvancedViewPreferences>
      advanced_view_preferences_;
  bool advanced_view_{false};
  bool catalog_close_dialog_open_{false};
  bool allow_window_close_{false};
};

}  // namespace winrt::Azzs::Ui::implementation

namespace winrt::Azzs::Ui::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::Azzs::Ui::factory_implementation
