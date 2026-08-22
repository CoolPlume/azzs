#pragma once

#include <memory>
#include <optional>

#include "MainWindow.g.h"
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Windowing.h>
#include "azzs/application/page_id.hpp"
#include "azzs/application/workbench.hpp"

namespace azzs::ui::winui {
class MotionPreferences;
}

namespace azzs::application {
class AdvancedViewPreferences;
class SidebarWidthPreferences;
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
          advanced_view_preferences,
      std::shared_ptr<azzs::application::SidebarWidthPreferences>
          sidebar_width_preferences);
  [[nodiscard]] bool show_initial_page();
  void confirm_started_healthy();
  void OnNavigationSelectionChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const&,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&
          args);
  void OnWindowClosing(
      Microsoft::UI::Windowing::AppWindow const&,
      Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args);
  void OnContinueRecoveredCatalogEditorClick(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnNavigationDisplayModeChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const&,
      Microsoft::UI::Xaml::Controls::NavigationViewDisplayModeChangedEventArgs const&);
  void OnShellSizeChanged(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::SizeChangedEventArgs const&);
  void OnSidebarResizeDragStarted(
      Microsoft::UI::Xaml::Controls::Primitives::Thumb const&,
      Microsoft::UI::Xaml::Controls::Primitives::DragStartedEventArgs const&);
  void OnSidebarResizeDragDelta(
      Microsoft::UI::Xaml::Controls::Primitives::Thumb const&,
      Microsoft::UI::Xaml::Controls::Primitives::DragDeltaEventArgs const&);
  void OnSidebarResizeDragCompleted(
      Microsoft::UI::Xaml::Controls::Primitives::Thumb const&,
      Microsoft::UI::Xaml::Controls::Primitives::DragCompletedEventArgs const&);
  void OnSidebarResizeKeyDown(
      Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);

 private:
  [[nodiscard]] std::optional<azzs::application::PageId> page_for_item(
      Microsoft::UI::Xaml::Controls::NavigationViewItem const& item);
  [[nodiscard]] Microsoft::UI::Xaml::Controls::NavigationViewItem
  navigation_item_for_page(azzs::application::PageId page);
  [[nodiscard]] bool navigate_and_commit(azzs::application::PageId page);
  bool navigate_to(azzs::application::PageId page);
  void refresh_drivers_page();
  void begin_driver_handoff(
      azzs::application::driver_acquisition::DriverEntrypoint entrypoint);
  void begin_rescue_folder_handoff(
      azzs::application::driver_acquisition::RescueToolTarget target);
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
  void apply_sidebar_width(double width_dip, bool persist);
  void update_sidebar_resize_thumb();
  void project(azzs::application::WorkbenchSnapshot const& snapshot);

  std::shared_ptr<azzs::application::Workbench> workbench_;
  std::shared_ptr<azzs::ui::winui::MotionPreferences> motion_preferences_;
  std::shared_ptr<azzs::application::SystemSettingsApplyService>
      system_settings_;
  std::shared_ptr<azzs::application::AdvancedViewPreferences>
      advanced_view_preferences_;
  std::shared_ptr<azzs::application::SidebarWidthPreferences>
      sidebar_width_preferences_;
  double sidebar_width_dip_{248.0};
  double sidebar_drag_width_dip_{248.0};
  bool sidebar_drag_active_{false};
  bool advanced_view_{false};
  bool catalog_close_dialog_open_{false};
  bool allow_window_close_{false};
  bool restoring_navigation_selection_{false};
  std::optional<azzs::application::PageId> displayed_page_;
};

}  // namespace winrt::Azzs::Ui::implementation

namespace winrt::Azzs::Ui::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::Azzs::Ui::factory_implementation
