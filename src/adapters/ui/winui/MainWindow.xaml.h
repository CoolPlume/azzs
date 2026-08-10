#pragma once

#include <memory>
#include <optional>

#include "MainWindow.g.h"
#include "azzs/application/page_id.hpp"
#include "azzs/application/workbench.hpp"

namespace winrt::Azzs::Ui::implementation {

struct MainWindow : MainWindowT<MainWindow> {
  MainWindow();

  void bind(std::shared_ptr<azzs::application::Workbench> workbench);
  void OnNavigationSelectionChanged(
      Microsoft::UI::Xaml::Controls::NavigationView const&,
      Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&
          args);

 private:
  [[nodiscard]] std::optional<azzs::application::PageId> page_for_item(
      Microsoft::UI::Xaml::Controls::NavigationViewItem const& item) const;
  void navigate_to(azzs::application::PageId page);
  void project(azzs::application::WorkbenchSnapshot const& snapshot);

  std::shared_ptr<azzs::application::Workbench> workbench_;
};

}  // namespace winrt::Azzs::Ui::implementation

namespace winrt::Azzs::Ui::factory_implementation {

struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};

}  // namespace winrt::Azzs::Ui::factory_implementation
