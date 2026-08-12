#include "pch.h"

#include "MainWindow.xaml.h"

#include <string>
#include <string_view>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include "DesignSystem/motion_preferences.hpp"
#include "Pages/ApplicationSettingsPage.xaml.h"
#include "Pages/DriversPage.xaml.h"
#include "Pages/HistoryAndLogsPage.xaml.h"
#include "Pages/OverviewPage.xaml.h"
#include "Pages/SoftwareInstallationPage.xaml.h"
#include "Pages/SoftwareOptimizationPage.xaml.h"
#include "Pages/SystemOptimizationPage.xaml.h"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/workbench_services.hpp"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace {

using azzs::application::PageId;
using azzs::domain::SystemVersion;
using winrt::Microsoft::UI::Xaml::Controls::NavigationViewItem;

[[nodiscard]] std::wstring version_text(SystemVersion const version) {
  return std::to_wstring(version.major) + L"." +
         std::to_wstring(version.minor) + L"." +
         std::to_wstring(version.build);
}

void replace_token(std::wstring& value,
                   std::wstring_view token,
                   std::wstring_view replacement) {
  auto position = value.find(token);
  while (position != std::wstring::npos) {
    value.replace(position, token.size(), replacement);
    position = value.find(token, position + replacement.size());
  }
}

}  // namespace

namespace winrt::Azzs::Ui::implementation {

MainWindow::MainWindow() {
  InitializeComponent();
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  Title(ResourceLoader{}.GetString(L"MainWindowTitle"));
}

void MainWindow::bind(
    std::shared_ptr<azzs::application::Workbench> workbench,
    std::shared_ptr<azzs::ui::winui::MotionPreferences> motion_preferences) {
  workbench_ = std::move(workbench);
  motion_preferences_ = std::move(motion_preferences);
  auto const snapshot = workbench_->snapshot();
  project(snapshot);
  navigate_to(snapshot.current_page);
}

void MainWindow::OnNavigationSelectionChanged(
    Microsoft::UI::Xaml::Controls::NavigationView const&,
    Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const&
        args) {
  if (!workbench_) {
    return;
  }

  auto const item =
      args.SelectedItemContainer().try_as<NavigationViewItem>();
  if (!item) {
    return;
  }

  auto const page = page_for_item(item);
  if (!page.has_value()) {
    return;
  }

  workbench_->navigate(*page);
  auto const snapshot = workbench_->snapshot();
  project(snapshot);
  navigate_to(snapshot.current_page);
}

std::optional<PageId> MainWindow::page_for_item(
    NavigationViewItem const& item) {
  if (item == OverviewItem()) {
    return PageId::overview;
  }
  if (item == DriversItem()) {
    return PageId::drivers;
  }
  if (item == SystemOptimizationItem()) {
    return PageId::system_optimization;
  }
  if (item == SoftwareInstallationItem()) {
    return PageId::software_installation;
  }
  if (item == SoftwareOptimizationItem()) {
    return PageId::software_optimization;
  }
  if (item == HistoryAndLogsItem()) {
    return PageId::history_and_logs;
  }
  if (item == ApplicationSettingsItem()) {
    return PageId::application_settings;
  }

  return std::nullopt;
}

void MainWindow::navigate_to(PageId page) {
  using winrt::Microsoft::UI::Xaml::Media::Animation::
      SuppressNavigationTransitionInfo;

  auto const transition = SuppressNavigationTransitionInfo{};
  switch (page) {
    case PageId::overview:
      ContentFrame().Navigate(xaml_typename<Pages::OverviewPage>(), nullptr,
                              transition);
      break;
    case PageId::drivers:
      ContentFrame().Navigate(xaml_typename<Pages::DriversPage>(), nullptr,
                              transition);
      if (auto const drivers_page =
              ContentFrame().Content().try_as<Pages::DriversPage>()) {
        auto const hardware =
            workbench_->observe_hardware(
                azzs::application::HardwareOverviewTrigger::page_entered);
        auto weak_this = get_weak();
        winrt::get_self<Pages::implementation::DriversPage>(drivers_page)
            ->bind(hardware, [weak_this] {
              if (auto self = weak_this.get()) {
                self->refresh_drivers_page();
              }
            });
      }
      break;
    case PageId::system_optimization:
      ContentFrame().Navigate(xaml_typename<Pages::SystemOptimizationPage>(),
                              nullptr, transition);
      break;
    case PageId::software_installation:
      ContentFrame().Navigate(xaml_typename<Pages::SoftwareInstallationPage>(),
                              nullptr, transition);
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame()
                              .Content()
                              .as<Pages::SoftwareInstallationPage>();
        winrt::get_self<Pages::implementation::SoftwareInstallationPage>(page)
            ->project(services->software_selection().snapshot());
      }
      break;
    case PageId::software_optimization:
      ContentFrame().Navigate(xaml_typename<Pages::SoftwareOptimizationPage>(),
                              nullptr, transition);
      break;
    case PageId::history_and_logs:
      ContentFrame().Navigate(xaml_typename<Pages::HistoryAndLogsPage>(), nullptr,
                              transition);
      break;
    case PageId::application_settings:
      ContentFrame().Navigate(xaml_typename<Pages::ApplicationSettingsPage>(),
                              nullptr, transition);
      break;
  }
}

void MainWindow::refresh_drivers_page() {
  if (!workbench_) {
    return;
  }
  auto const hardware = workbench_->refresh_hardware();
  if (auto const drivers_page =
          ContentFrame().Content().try_as<Pages::DriversPage>()) {
    winrt::get_self<Pages::implementation::DriversPage>(drivers_page)
        ->project(hardware);
  }
}

void MainWindow::project(
    azzs::application::WorkbenchSnapshot const& snapshot) {
  using azzs::domain::MinimumVersionRisk;
  using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

  auto const resources = ResourceLoader{};
  auto const risk_title = resources.GetString(L"VersionRiskTitle");
  AutomationProperties::SetName(VersionRiskInfoBar(), risk_title);

  if (snapshot.minimum_version_risk == MinimumVersionRisk::none) {
    VersionRiskInfoBar().IsOpen(false);
    return;
  }

  VersionRiskInfoBar().Title(risk_title);

  if (snapshot.minimum_version_risk ==
      MinimumVersionRisk::version_unavailable) {
    VersionRiskInfoBar().Message(
        resources.GetString(L"VersionRiskUnavailableMessage"));
    VersionRiskInfoBar().IsOpen(true);
    return;
  }

  auto message =
      std::wstring{resources.GetString(L"VersionRiskEarlierMessage")};
  auto const observed = snapshot.observed_windows_version.value();
  auto const observed_text = version_text(observed);
  auto const target_text = version_text(snapshot.target_windows_version);
  replace_token(message, L"{observed}", observed_text);
  replace_token(message, L"{target}", target_text);
  VersionRiskInfoBar().Message(winrt::hstring{message});
  VersionRiskInfoBar().IsOpen(true);
}

}  // namespace winrt::Azzs::Ui::implementation
