#include "pch.h"

#include "MainWindow.xaml.h"

#include <string>
#include <string_view>
#include <vector>
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
#include "azzs/application/execution_log.hpp"
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

struct SettingsNavigationPreparationError final {
  azzs::ui::presentation::SettingsNavigationFailureStage stage{
      azzs::ui::presentation::SettingsNavigationFailureStage::unknown};
  char const* detail{"settings page preparation failed"};
};

[[nodiscard]] std::string_view settings_navigation_stage_name(
    azzs::ui::presentation::SettingsNavigationFailureStage stage) noexcept {
  using Stage = azzs::ui::presentation::SettingsNavigationFailureStage;
  switch (stage) {
    case Stage::optional_value_missing:
      return "optional-value";
    case Stage::snapshot_read:
      return "snapshot-read";
    case Stage::page_binding:
      return "page-binding";
    case Stage::resource_projection:
      return "resource-projection";
    case Stage::commit:
      return "commit";
    case Stage::unknown:
      return "unknown";
  }
  return "unknown";
}

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

bool MainWindow::show_initial_page() {
  if (!workbench_) {
    return false;
  }
  // Startup restoration uses the same settings transaction as user navigation,
  // but a recoverable settings failure must still leave an activatable window.
  auto const initial_page = workbench_->snapshot().current_page;
  if (navigate_and_commit(initial_page)) {
    return true;
  }
  if (initial_page != PageId::application_settings) {
    return false;
  }

  // There is no prior visible page during startup. Keep the workbench usable
  // on the overview while the settings failure InfoBar remains actionable.
  if (!navigate_to(PageId::overview)) {
    return false;
  }
  workbench_->navigate(PageId::overview);
  project(workbench_->snapshot());
  auto const navigation_item = navigation_item_for_page(PageId::overview);
  if (navigation_item) {
    restoring_navigation_selection_ = true;
    PrimaryNavigation().SelectedItem(navigation_item);
    restoring_navigation_selection_ = false;
  }
  handle_settings_navigation_failure();
  return true;
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
  if (restoring_navigation_selection_ || !workbench_) {
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

  static_cast<void>(navigate_and_commit(*page));
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
  project(workbench_->snapshot());
  if (!navigate_and_commit(PageId::software_catalog_editor)) {
    return;
  }
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

  project(workbench_->snapshot());
  if (!navigate_and_commit(PageId::software_catalog_editor)) {
    return;
  }
}

void MainWindow::OnRetrySettingsNavigationClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  static_cast<void>(settings_navigation_bridge_.retry());
}

void MainWindow::OnReturnToCurrentPageClick(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  settings_navigation_bridge_.return_to_current();
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

NavigationViewItem MainWindow::navigation_item_for_page(PageId page) {
  switch (page) {
    case PageId::overview:
      return OverviewItem();
    case PageId::drivers:
      return DriversItem();
    case PageId::system_optimization:
      return SystemOptimizationItem();
    case PageId::software_installation:
      return SoftwareInstallationItem();
    case PageId::software_optimization:
      return SoftwareOptimizationItem();
    case PageId::history_and_logs:
      return HistoryAndLogsItem();
    case PageId::application_settings:
      return ApplicationSettingsItem();
    case PageId::software_catalog_editor:
      return SoftwareCatalogEditorItem();
  }
  return nullptr;
}

bool MainWindow::navigate_and_commit(PageId page) {
  if (page == PageId::application_settings) {
    try {
      return settings_navigation_bridge_.navigate({
          .already_visible = [this] {
            try {
              auto const content = ContentFrame().Content();
              return displayed_page_ == PageId::application_settings &&
                     content.try_as<Pages::ApplicationSettingsPage>();
            } catch (...) {
              return false;
            }
          },
          .prepare =
              [this] {
                // Capture recovery context for every attempt, including a
                // retry. No callback retains an earlier page snapshot.
                auto const previous_page = displayed_page_;
                auto previous_core_page =
                    previous_page.value_or(PageId::overview);
                Windows::Foundation::IInspectable previous_content{nullptr};
                try {
                  previous_content = ContentFrame().Content();
                } catch (...) {
                  ::OutputDebugStringW(
                      L"WinUI application-settings previous content snapshot failed.\n");
                }
                try {
                  if (workbench_) {
                    previous_core_page = workbench_->snapshot().current_page;
                  }
                } catch (...) {
                  ::OutputDebugStringW(
                      L"WinUI application-settings core page snapshot failed.\n");
                }
                auto recover =
                    [this, previous_page, previous_core_page,
                     previous_content]() noexcept {
                      restore_settings_navigation_state(
                          previous_page, previous_core_page, previous_content);
                    };
                try {
                  auto const prepared = prepare_application_settings_page();
                  return azzs::ui::presentation::
                      SettingsNavigationPreparation{
                          .commit = [this, prepared, previous_page] {
                            commit_application_settings_page(prepared,
                                                             previous_page);
                          },
                          .recover = std::move(recover)};
                } catch (SettingsNavigationPreparationError const& error) {
                  return azzs::ui::presentation::
                      SettingsNavigationPreparation{
                          .failure = azzs::ui::presentation::
                              SettingsNavigationFailure{
                                  .stage = error.stage,
                                  .detail = error.detail},
                          .recover = std::move(recover)};
                } catch (...) {
                  return azzs::ui::presentation::
                      SettingsNavigationPreparation{
                          .failure = azzs::ui::presentation::
                              SettingsNavigationFailure{
                                  .stage = azzs::ui::presentation::
                                      SettingsNavigationFailureStage::unknown,
                                  .detail =
                                      "settings page preparation failed"},
                          .recover = std::move(recover)};
                }
              },
          .present_failure =
              [this](azzs::ui::presentation::SettingsNavigationFailure const& failure) {
                record_settings_navigation_failure(failure);
                handle_settings_navigation_failure();
              },
          .clear_failure = [this] { clear_settings_navigation_failure(); },
      });
    } catch (...) {
      record_settings_navigation_failure(
          {.stage = azzs::ui::presentation::SettingsNavigationFailureStage::unknown,
           .detail = "settings navigation transaction threw"});
      settings_navigation_bridge_.return_to_current();
      handle_settings_navigation_failure();
      return false;
    }
  }

  if (!workbench_ || !navigate_to(page)) {
    return false;
  }
  workbench_->navigate(page);
  project(workbench_->snapshot());

  auto const navigation_item = navigation_item_for_page(page);
  if (navigation_item) {
    restoring_navigation_selection_ = true;
    PrimaryNavigation().SelectedItem(navigation_item);
    restoring_navigation_selection_ = false;
  }
  clear_settings_navigation_failure();
  settings_navigation_bridge_.invalidate();
  return true;
}

Windows::Foundation::IInspectable
MainWindow::prepare_application_settings_page() {
  if (!workbench_) {
    throw SettingsNavigationPreparationError{
        .stage = azzs::ui::presentation::SettingsNavigationFailureStage::
            optional_value_missing,
        .detail = "workbench is unavailable"};
  }
  auto const services = workbench_->services();
  if (!services) {
    throw SettingsNavigationPreparationError{
        .stage = azzs::ui::presentation::SettingsNavigationFailureStage::
            optional_value_missing,
        .detail = "settings services are unavailable"};
  }

  // Snapshot and bind the candidate while the existing Frame content remains
  // visible. Any resource, persistence, or projection exception therefore
  // leaves both the old page and the core page untouched.
  auto& settings = services->application_settings();
  azzs::application::WorkbenchSnapshot workbench_snapshot;
  azzs::application::ApplicationSettingsSnapshot settings_snapshot;
  try {
    workbench_snapshot = workbench_->snapshot();
    settings_snapshot = settings.snapshot();
  } catch (...) {
    throw SettingsNavigationPreparationError{
        .stage = azzs::ui::presentation::SettingsNavigationFailureStage::
            snapshot_read,
        .detail = "settings snapshot read failed"};
  }

  Pages::ApplicationSettingsPage page{nullptr};
  try {
    auto const page_owner =
        winrt::make_self<Pages::implementation::ApplicationSettingsPage>();
    page = page_owner.as<Pages::ApplicationSettingsPage>();
  } catch (...) {
    throw SettingsNavigationPreparationError{
        .stage = azzs::ui::presentation::SettingsNavigationFailureStage::
            page_binding,
        .detail = "settings page construction failed"};
  }

  try {
    winrt::get_self<Pages::implementation::ApplicationSettingsPage>(page)
        ->bind(
            workbench_, settings, workbench_snapshot.update, settings_snapshot,
            advanced_view_, [weak_this = get_weak()](bool enabled) {
              if (auto self = weak_this.get()) {
                return self->set_advanced_view(enabled);
              }
              return false;
            },
            [weak_this = get_weak()] {
              if (auto self = weak_this.get(); self && self->workbench_) {
                static_cast<void>(self->navigate_and_commit(
                    PageId::software_catalog_editor));
              }
            });
  } catch (...) {
    throw SettingsNavigationPreparationError{
        .stage = azzs::ui::presentation::SettingsNavigationFailureStage::
            resource_projection,
        .detail = "settings page binding projection failed"};
  }

  return page;
}

void MainWindow::commit_application_settings_page(
    Windows::Foundation::IInspectable const& page,
    std::optional<PageId> const previous_page) {
  auto const prepared = page.try_as<Pages::ApplicationSettingsPage>();
  if (!prepared || !workbench_) {
    throw winrt::hresult_error(E_FAIL);
  }
  auto const navigation_item = navigation_item_for_page(
      PageId::application_settings);
  if (!navigation_item) {
    throw winrt::hresult_error(E_FAIL);
  }

  restoring_navigation_selection_ = true;
  try {
    PrimaryNavigation().SelectedItem(navigation_item);
  } catch (...) {
    restoring_navigation_selection_ = false;
    throw;
  }
  restoring_navigation_selection_ = false;

  ContentFrame().Content(prepared);
  displayed_page_ = PageId::application_settings;
  workbench_->navigate(PageId::application_settings);

  if (previous_page == PageId::software_catalog_editor) {
    if (auto const services = workbench_->services()) {
      services->debug_mode_catalog_editor().end_temporary_close_recovery();
    }
  }
  // A post-commit projection is part of the transaction boundary. Let any
  // exception escape so SettingsNavigationBridge performs the single recovery
  // path; swallowing it would report navigation success with a partial shell.
  project(workbench_->snapshot());
}

void MainWindow::restore_settings_navigation_state(
    std::optional<PageId> previous_page, PageId previous_core_page,
    Windows::Foundation::IInspectable const& previous_content) noexcept {
  restoring_navigation_selection_ = false;
  try {
    ContentFrame().Content(previous_content);
  } catch (...) {
    ::OutputDebugStringW(
        L"WinUI application-settings previous content restore failed.\n");
  }

  displayed_page_ = previous_page;
  if (workbench_) {
    try {
      workbench_->navigate(previous_core_page);
    } catch (...) {
      ::OutputDebugStringW(
          L"WinUI application-settings previous core-page restore failed.\n");
    }
    try {
      project(workbench_->snapshot());
    } catch (...) {
      ::OutputDebugStringW(
          L"WinUI application-settings previous projection restore failed.\n");
    }
  }

  try {
    if (previous_page.has_value()) {
      auto const previous_item = navigation_item_for_page(*previous_page);
      if (previous_item) {
        restoring_navigation_selection_ = true;
        PrimaryNavigation().SelectedItem(previous_item);
        restoring_navigation_selection_ = false;
      }
    }
  } catch (...) {
    restoring_navigation_selection_ = false;
    ::OutputDebugStringW(
        L"WinUI application-settings previous selection restore failed.\n");
  }
}

bool MainWindow::navigate_to(PageId page) {
  using winrt::Microsoft::UI::Xaml::Media::Animation::
      SuppressNavigationTransitionInfo;

  auto const transition = SuppressNavigationTransitionInfo{};
  switch (page) {
    case PageId::overview:
      if (!ContentFrame().Navigate(xaml_typename<Pages::OverviewPage>(), nullptr,
                                   transition)) {
        return false;
      }
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame().Content().as<Pages::OverviewPage>();
        winrt::get_self<Pages::implementation::OverviewPage>(page)->bind(
            services, advanced_view_, [weak_this = get_weak()](PageId target) {
              if (auto self = weak_this.get(); self && self->workbench_) {
                if (!self->navigate_and_commit(target)) {
                  return;
                }
              }
            });
      }
      break;
    case PageId::drivers:
      if (!ContentFrame().Navigate(xaml_typename<Pages::DriversPage>(), nullptr,
                                   transition)) {
        return false;
      }
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
      if (!ContentFrame().Navigate(
              xaml_typename<Pages::SystemOptimizationPage>(), nullptr,
              transition)) {
        return false;
      }
      if (auto page =
              ContentFrame().Content().try_as<Pages::SystemOptimizationPage>();
          page && system_settings_) {
        winrt::get_self<Pages::implementation::SystemOptimizationPage>(page)
            ->bind(system_settings_, advanced_view_);
      }
      break;
    case PageId::software_installation:
      if (!ContentFrame().Navigate(
              xaml_typename<Pages::SoftwareInstallationPage>(), nullptr,
              transition)) {
        return false;
      }
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame()
                              .Content()
                              .as<Pages::SoftwareInstallationPage>();
        winrt::get_self<Pages::implementation::SoftwareInstallationPage>(page)
            ->bind(services);
      }
      break;
    case PageId::software_optimization:
      if (!ContentFrame().Navigate(
              xaml_typename<Pages::SoftwareOptimizationPage>(), nullptr,
              transition)) {
        return false;
      }
      if (auto const services = workbench_->services()) {
        auto const page = ContentFrame()
                              .Content()
                              .as<Pages::SoftwareOptimizationPage>();
        winrt::get_self<Pages::implementation::SoftwareOptimizationPage>(page)
            ->bind(services->software_optimization_discovery(), advanced_view_);
      }
      break;
    case PageId::history_and_logs:
      if (!ContentFrame().Navigate(xaml_typename<Pages::HistoryAndLogsPage>(),
                                   nullptr, transition)) {
        return false;
      }
      if (auto const services = workbench_->services()) {
        auto const history_page =
            ContentFrame().Content().as<Pages::HistoryAndLogsPage>();
        winrt::get_self<Pages::implementation::HistoryAndLogsPage>(history_page)
            ->bind(services->history_and_logs());
      }
      break;
    case PageId::application_settings:
      // Application settings owns a prepare/commit/recovery transaction and
      // must never be entered through the generic Navigate path.
      return false;
    case PageId::software_catalog_editor:
      if (!ContentFrame().Navigate(
              xaml_typename<Pages::SoftwareCatalogEditorPage>(), nullptr,
              transition)) {
        return false;
      }
      if (auto page = ContentFrame().Content().try_as<
              Pages::SoftwareCatalogEditorPage>();
          page && workbench_->services()) {
        winrt::get_self<Pages::implementation::SoftwareCatalogEditorPage>(page)
            ->bind(workbench_->services()->debug_mode_catalog_editor());
      }
      break;
  }
  if (displayed_page_ == PageId::software_catalog_editor &&
      page != PageId::software_catalog_editor) {
    if (auto const services = workbench_->services()) {
      services->debug_mode_catalog_editor().end_temporary_close_recovery();
      project(workbench_->snapshot());
    }
  }
  displayed_page_ = page;
  return true;
}

void MainWindow::record_settings_navigation_failure(
    azzs::ui::presentation::SettingsNavigationFailure const& failure) noexcept {
  if (!workbench_) {
    return;
  }

  try {
    auto const services = workbench_->services();
    if (!services) {
      return;
    }

    auto& log = services->execution_log();
    auto const correlation = log.begin_correlation();
    if (correlation.value.empty()) {
      return;
    }

    auto const stage = settings_navigation_stage_name(failure.stage);
    std::vector<azzs::application::DiagnosticField> fields;
    fields.push_back({"detail", failure.detail,
                      azzs::application::DiagnosticValueDisposition::retain});
    static_cast<void>(log.append(
        correlation,
        azzs::application::ExecutionEvent{
            .kind = azzs::application::ExecutionEventKind::adapter_result,
            .component = "winui-settings-navigation",
            .stage = std::string{stage},
            .result = azzs::application::ExecutionResult::failed,
            .error = azzs::application::ExecutionError{
                .source = "winui",
                .message = failure.detail,
            },
            .fields = std::move(fields),
        }));
  } catch (...) {
    ::OutputDebugStringW(
        L"WinUI application-settings structured failure logging failed.\n");
  }
}

void MainWindow::handle_settings_navigation_failure() noexcept {
  restoring_navigation_selection_ = false;
  try {
    using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
    using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
    auto const resources = ResourceLoader{};
    auto const title =
        resources.GetString(L"MainWindowSettingsNavigationFailed.Title");
    SettingsNavigationFailureInfoBar().Title(title);
    SettingsNavigationFailureInfoBar().Message(
        resources.GetString(L"MainWindowSettingsNavigationFailed.Message"));
    AutomationProperties::SetName(SettingsNavigationFailureInfoBar(), title);
  } catch (...) {
    try {
      SettingsNavigationFailureInfoBar().Title(L"应用设置暂时无法打开");
      SettingsNavigationFailureInfoBar().Message(
          L"设置数据或页面资源读取失败。现有页面已保留，请重试或返回当前页面。");
    } catch (...) {
      ::OutputDebugStringW(
          L"WinUI application-settings fallback message projection failed.\n");
    }
    ::OutputDebugStringW(L"WinUI application-settings navigation recovery failed.\n");
  }

  try {
    SettingsNavigationFailureInfoBar().IsOpen(true);
  } catch (...) {
    ::OutputDebugStringW(L"WinUI application-settings failure state projection failed.\n");
  }
}

void MainWindow::clear_settings_navigation_failure() noexcept {
  try {
    SettingsNavigationFailureInfoBar().IsOpen(false);
  } catch (...) {
    ::OutputDebugStringW(L"WinUI application-settings failure state clear failed.\n");
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
