#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "Pages/SystemOptimizationPage.g.h"

namespace azzs::application {
class SystemSettingsApplyService;
struct SystemSettingsApplySnapshot;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct SystemOptimizationPage : SystemOptimizationPageT<SystemOptimizationPage> {
  SystemOptimizationPage();
  void bind(
      std::shared_ptr<azzs::application::SystemSettingsApplyService> service);
  void OnSelectRecommended(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnApplySelected(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnRestartExplorerNow(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnUndoRecoveryRecord(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnDeleteRecoveryRecord(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void OnRestoreWindows11Default(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void OnSettingSelectionChanged(
      winrt::Windows::Foundation::IInspectable const&,
      Microsoft::UI::Xaml::RoutedEventArgs const&);
  void project(azzs::application::SystemSettingsApplySnapshot const& snapshot);

  std::shared_ptr<azzs::application::SystemSettingsApplyService> service_;
  std::optional<std::uint64_t> pending_recovery_record_deletion_;
  std::wstring recovery_deletion_notice_;
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct SystemOptimizationPage
    : SystemOptimizationPageT<SystemOptimizationPage,
                              implementation::SystemOptimizationPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
