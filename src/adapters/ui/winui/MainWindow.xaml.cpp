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
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/driver_acquisition.hpp"
#include "Pages/SoftwareOptimizationPage.xaml.h"
#include "Pages/SystemOptimizationPage.xaml.h"
#include "azzs/application/advanced_view_preferences.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/system_settings_apply.hpp"
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
    std::shared_ptr<azzs::ui::winui::MotionPreferences> motion_preferences,
    std::shared_ptr<azzs::application::SystemSettingsApplyService>
        system_settings,
    std::shared_ptr<azzs::application::AdvancedViewPreferences>
        advanced_view_preferences) {
  workbench_ = std::move(workbench);
  motion_preferences_ = std::move(motion_preferences);
  system_settings_ = std::move(system_settings);
  advanced_view_preferences_ = std::move(advanced_view_preferences);
  advanced_view_ = advanced_view_preferences_
                       ? advanced_view_preferences_->enabled()
                       : false;
  auto snapshot = workbench_->snapshot();
  if (snapshot.update.state ==
          azzs::application::UpdateState::candidate_pending_start_health ||
      snapshot.update.state ==
          azzs::application::UpdateState::previous_pending_start_health) {
    static_cast<void>(workbench_->handle_update(
        azzs::application::UpdateUserIntent::confirm_started_healthy));
    snapshot = workbench_->snapshot();
  }
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
            ->bind(hardware, workbench_->snapshot().driver_acquisition,
                   [weak_this] {
              if (auto self = weak_this.get()) {
                self->refresh_drivers_page();
              }
            }, [weak_this](auto entrypoint) {
              if (auto self = weak_this.get()) {
                self->begin_driver_handoff(entrypoint);
              }
            }, [weak_this] {
              if (auto self = weak_this.get()) {
                self->driver_flow_returned();
              }
            }, [weak_this](auto decision) {
              if (auto self = weak_this.get()) {
                self->decide_driver_handoff(decision);
              }
            });
      }
      break;
    case PageId::system_optimization:
      ContentFrame().Navigate(xaml_typename<Pages::SystemOptimizationPage>(),
                              nullptr, transition);
      if (auto page =
              ContentFrame().Content().try_as<Pages::SystemOptimizationPage>();
          page && system_settings_) {
        winrt::get_self<Pages::implementation::SystemOptimizationPage>(page)
            ->bind(system_settings_, advanced_view_);
      }
      break;
    case PageId::software_installation:
      ContentFrame().Navigate(xaml_typename<Pages::SoftwareInstallationPage>(),
                              nullptr, transition);
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame()
                              .Content()
                              .as<Pages::SoftwareInstallationPage>();
        winrt::get_self<Pages::implementation::SoftwareInstallationPage>(page)
            ->bind(services);
      }
      break;
    case PageId::software_optimization:
      ContentFrame().Navigate(xaml_typename<Pages::SoftwareOptimizationPage>(),
                              nullptr, transition);
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame()
                              .Content()
                              .as<Pages::SoftwareOptimizationPage>();
        winrt::get_self<Pages::implementation::SoftwareOptimizationPage>(page)
            ->bind(services->software_optimization_discovery(), advanced_view_);
      }
      break;
    case PageId::history_and_logs:
      ContentFrame().Navigate(xaml_typename<Pages::HistoryAndLogsPage>(), nullptr,
                              transition);
      break;
    case PageId::application_settings:
      ContentFrame().Navigate(xaml_typename<Pages::ApplicationSettingsPage>(),
                              nullptr, transition);
      if (auto settings = ContentFrame().Content().try_as<
              Pages::ApplicationSettingsPage>()) {
        winrt::get_self<Pages::implementation::ApplicationSettingsPage>(
            settings)
            ->bind(workbench_, advanced_view_, [weak_this = get_weak()](
                                              bool enabled) {
              if (auto self = weak_this.get()) {
                return self->set_advanced_view(enabled);
              }
              return false;
            });
      }
      break;
  }
}

bool MainWindow::set_advanced_view(bool enabled) {
  if (advanced_view_preferences_) {
    advanced_view_ = advanced_view_preferences_->set_enabled(enabled);
  }
  return advanced_view_;
}

void MainWindow::refresh_drivers_page() {
  if (!workbench_) {
    return;
  }
  auto const hardware = workbench_->refresh_hardware();
  if (auto const drivers_page =
          ContentFrame().Content().try_as<Pages::DriversPage>()) {
    winrt::get_self<Pages::implementation::DriversPage>(drivers_page)
        ->project(hardware, workbench_->snapshot().driver_acquisition);
  }
}

void MainWindow::begin_driver_handoff(
    azzs::application::driver_acquisition::DriverEntrypoint entrypoint) {
  if (!workbench_) {
    return;
  }
  auto const result = workbench_->begin_driver_handoff(entrypoint);
  project_drivers_page(result.snapshot);
}

void MainWindow::driver_flow_returned() {
  if (!workbench_) {
    return;
  }
  auto const result = workbench_->driver_flow_returned();
  project_drivers_page(result.snapshot);
}

void MainWindow::decide_driver_handoff(
    azzs::application::driver_acquisition::DriverHandoffDecision decision) {
  if (!workbench_) {
    return;
  }
  auto const result = workbench_->decide_driver_handoff(decision);
  project_drivers_page(result.snapshot);
}

void MainWindow::project_drivers_page(
    azzs::application::driver_acquisition::DriverAcquisitionSnapshot const&
        driver_snapshot) {
  if (!workbench_) {
    return;
  }
  if (auto const drivers_page =
          ContentFrame().Content().try_as<Pages::DriversPage>()) {
    auto const snapshot = workbench_->snapshot();
    winrt::get_self<Pages::implementation::DriversPage>(drivers_page)
        ->project(snapshot.hardware_overview, driver_snapshot);
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
