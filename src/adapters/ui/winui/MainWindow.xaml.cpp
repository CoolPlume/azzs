#include "pch.h"

#include "MainWindow.xaml.h"

#include <string>
#include <string_view>
#include <winrt/Microsoft.UI.Windowing.h>
#include <winrt/Windows.UI.Xaml.Interop.h>

#include "DesignSystem/motion_preferences.hpp"
#include "Pages/ApplicationSettingsPage.xaml.h"
#include "Pages/DriversPage.xaml.h"
#include "Pages/HistoryAndLogsPage.xaml.h"
#include "Pages/OverviewPage.xaml.h"
#include "Pages/SoftwareInstallationPage.xaml.h"
#include "Pages/SoftwareCatalogEditorPage.xaml.h"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/driver_acquisition.hpp"
#include "Pages/SoftwareOptimizationPage.xaml.h"
#include "Pages/SystemOptimizationPage.xaml.h"
#include "azzs/application/advanced_view_preferences.hpp"
#include "azzs/application/application_settings.hpp"
#include "azzs/application/debug_mode_catalog_editor.hpp"
#include "azzs/application/software_selection.hpp"
#include "azzs/application/system_settings_apply.hpp"
#include "azzs/application/workbench_services.hpp"

#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace {

using azzs::application::PageId;
using azzs::domain::SystemVersion;
using winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialog;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialogButton;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult;
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
  AppWindow().Closing({this, &MainWindow::OnWindowClosing});
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
  project(workbench_->snapshot());
}

void MainWindow::show_initial_page() {
  if (workbench_) {
    navigate_to(workbench_->snapshot().current_page);
  }
}

void MainWindow::confirm_started_healthy() {
  if (!workbench_) {
    return;
  }

  auto const snapshot = workbench_->snapshot();
  if (snapshot.update.state ==
          azzs::application::UpdateState::candidate_pending_start_health ||
      snapshot.update.state ==
          azzs::application::UpdateState::previous_pending_start_health) {
    static_cast<void>(workbench_->handle_update(
        azzs::application::UpdateUserIntent::confirm_started_healthy));
  }
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

void MainWindow::OnWindowClosing(
    winrt::Microsoft::UI::Windowing::AppWindow const&,
                                 AppWindowClosingEventArgs const& args) {
  if (allow_window_close_ || catalog_close_dialog_open_ || !workbench_) {
    if (catalog_close_dialog_open_) {
      args.Cancel(true);
    }
    return;
  }

  auto const services = workbench_->services();
  if (!services) {
    return;
  }
  auto const draft_state =
      services->debug_mode_catalog_editor().editor_snapshot().catalog.draft.state;
  if (draft_state != azzs::application::software_catalog::
                         DraftWorkState::unsaved_changes &&
      draft_state != azzs::application::software_catalog::
                         DraftWorkState::recovered_unsaved) {
    return;
  }

  args.Cancel(true);
  catalog_close_dialog_open_ = true;
  confirm_catalog_close();
}

winrt::fire_and_forget MainWindow::confirm_catalog_close() {
  try {
    auto lifetime = get_strong();
    using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

    ContentDialog dialog;
    dialog.XamlRoot(ContentFrame().XamlRoot());
    auto const resources = ResourceLoader{};
    dialog.Title(
        winrt::box_value(resources.GetString(L"CatalogCloseDialogTitle")));
    dialog.Content(
        winrt::box_value(resources.GetString(L"CatalogCloseDialogContent")));
    dialog.PrimaryButtonText(resources.GetString(L"CatalogCloseSaveAndClose"));
    dialog.SecondaryButtonText(
        resources.GetString(L"CatalogCloseDiscardAndClose"));
    dialog.CloseButtonText(resources.GetString(L"CatalogCloseReturnToEditor"));
    dialog.DefaultButton(ContentDialogButton::Close);

    auto const response = co_await dialog.ShowAsync();
    catalog_close_dialog_open_ = false;
    if (!workbench_) {
      co_return;
    }
    auto const services = workbench_->services();
    if (!services) {
      co_return;
    }

    auto& editor = services->debug_mode_catalog_editor();
    auto choice = azzs::application::software_catalog::
        CatalogCloseChoice::return_to_editor;
    if (response == ContentDialogResult::Primary) {
      choice = azzs::application::software_catalog::
          CatalogCloseChoice::save_draft_and_close;
    } else if (response == ContentDialogResult::Secondary) {
      choice = azzs::application::software_catalog::
          CatalogCloseChoice::discard_unsaved_and_close;
    }

    auto const debug = editor.editor_snapshot().settings;
    if (!debug.enabled) {
      static_cast<void>(editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::close_return));
    }
    auto const result = editor.handle_close(choice);
    if (choice != azzs::application::software_catalog::
                      CatalogCloseChoice::return_to_editor &&
        result.succeeded()) {
      editor.end_temporary_close_recovery();
      allow_window_close_ = true;
      Close();
      co_return;
    }
    restore_catalog_editor_after_close(result);
  } catch (...) {
    catalog_close_dialog_open_ = false;
    ::OutputDebugStringW(L"WinUI catalog close dialog failed.\n");
  }
}

void MainWindow::restore_catalog_editor_after_close(
    azzs::application::software_catalog::CatalogActionResult const& result) {
  if (!workbench_) {
    return;
  }
  auto const services = workbench_->services();
  if (!services) {
    return;
  }

  auto& editor = services->debug_mode_catalog_editor();
  if (!editor.editor_snapshot().settings.enabled &&
      !editor.begin_temporary_close_recovery(
          azzs::application::CatalogEditorTemporaryAccessReason::close_return)) {
    project(workbench_->snapshot());
    return;
  }
  workbench_->navigate(PageId::software_catalog_editor);
  auto const snapshot = workbench_->snapshot();
  project(snapshot);
  navigate_to(snapshot.current_page);
  if (auto page = ContentFrame().Content().try_as<
          Pages::SoftwareCatalogEditorPage>()) {
    winrt::get_self<Pages::implementation::SoftwareCatalogEditorPage>(page)
        ->show_action_result(result);
  }
}

void MainWindow::OnContinueRecoveredCatalogEditorClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (!workbench_) {
    return;
  }
  auto const services = workbench_->services();
  if (!services || !services->debug_mode_catalog_editor()
                        .begin_temporary_close_recovery(
                            azzs::application::
                                CatalogEditorTemporaryAccessReason::
                                    recovered_unsaved_continue)) {
    project(workbench_->snapshot());
    return;
  }

  workbench_->navigate(PageId::software_catalog_editor);
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
  if (item == SoftwareCatalogEditorItem()) {
    return PageId::software_catalog_editor;
  }

  return std::nullopt;
}

void MainWindow::navigate_to(PageId page) {
  using winrt::Microsoft::UI::Xaml::Media::Animation::
      SuppressNavigationTransitionInfo;

  auto const transition = SuppressNavigationTransitionInfo{};
  if (displayed_page_ == PageId::software_catalog_editor &&
      page != PageId::software_catalog_editor) {
    if (auto const services = workbench_->services()) {
      services->debug_mode_catalog_editor().end_temporary_close_recovery();
      project(workbench_->snapshot());
    }
  }
  switch (page) {
    case PageId::overview:
      ContentFrame().Navigate(xaml_typename<Pages::OverviewPage>(), nullptr,
                              transition);
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame().Content().as<Pages::OverviewPage>();
        winrt::get_self<Pages::implementation::OverviewPage>(page)->bind(
            services, advanced_view_, [weak_this = get_weak()](PageId target) {
              if (auto self = weak_this.get(); self && self->workbench_) {
                self->workbench_->navigate(target);
                auto const snapshot = self->workbench_->snapshot();
                self->project(snapshot);
                self->navigate_to(snapshot.current_page);
              }
            });
      }
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
            }, [weak_this](auto target) {
              if (auto self = weak_this.get()) {
                self->begin_rescue_folder_handoff(target);
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
      if (auto const services = workbench_->services()) {
        auto const history_page =
            ContentFrame().Content().as<Pages::HistoryAndLogsPage>();
        winrt::get_self<Pages::implementation::HistoryAndLogsPage>(history_page)
            ->bind(services->history_and_logs());
      }
      break;
    case PageId::application_settings:
      ContentFrame().Navigate(xaml_typename<Pages::ApplicationSettingsPage>(),
                              nullptr, transition);
      if (auto settings = ContentFrame().Content().try_as<
              Pages::ApplicationSettingsPage>();
          settings && workbench_->services()) {
        winrt::get_self<Pages::implementation::ApplicationSettingsPage>(
            settings)
            ->bind(workbench_, workbench_->services()->application_settings(),
                   advanced_view_, [weak_this = get_weak()](bool enabled) {
               if (auto self = weak_this.get()) {
                 return self->set_advanced_view(enabled);
               }
              return false;
            }, [weak_this = get_weak()] {
              if (auto self = weak_this.get(); self && self->workbench_) {
                self->workbench_->navigate(PageId::software_catalog_editor);
                auto const snapshot = self->workbench_->snapshot();
                self->project(snapshot);
                self->navigate_to(snapshot.current_page);
              }
            });
      }
      break;
    case PageId::software_catalog_editor:
      ContentFrame().Navigate(xaml_typename<Pages::SoftwareCatalogEditorPage>(),
                              nullptr, transition);
      if (auto page = ContentFrame().Content().try_as<
              Pages::SoftwareCatalogEditorPage>();
          page && workbench_->services()) {
        winrt::get_self<Pages::implementation::SoftwareCatalogEditorPage>(page)
            ->bind(workbench_->services()->debug_mode_catalog_editor());
      }
      break;
  }
  displayed_page_ = page;
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

void MainWindow::begin_rescue_folder_handoff(
    azzs::application::driver_acquisition::RescueToolTarget target) {
  if (!workbench_) {
    return;
  }
  auto const result = workbench_->begin_rescue_folder_handoff(target);
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
  if (auto const services = workbench_ ? workbench_->services() : nullptr) {
    auto const editor_snapshot =
        services->debug_mode_catalog_editor().editor_snapshot();
    auto const debug = editor_snapshot.settings;
    SoftwareCatalogEditorItem().Visibility(
        debug.catalog_editor_available
            ? winrt::Microsoft::UI::Xaml::Visibility::Visible
            : winrt::Microsoft::UI::Xaml::Visibility::Collapsed);
    auto const recovered_editor_available =
        !debug.enabled && !debug.temporary_close_recovery &&
        editor_snapshot.catalog.draft.state ==
            azzs::application::software_catalog::
                DraftWorkState::recovered_unsaved;
    RecoveredCatalogEditorInfoBar().IsOpen(recovered_editor_available);
    ContinueRecoveredCatalogEditorButton().IsEnabled(
        recovered_editor_available);
  } else {
    RecoveredCatalogEditorInfoBar().IsOpen(false);
    ContinueRecoveredCatalogEditorButton().IsEnabled(false);
  }
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
