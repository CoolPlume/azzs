#pragma once

#include "Pages/HistoryAndLogsPage.g.h"

namespace azzs::application {
class HistoryAndLogsService;
struct HistoryAndLogsSnapshot;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct HistoryAndLogsPage : HistoryAndLogsPageT<HistoryAndLogsPage> {
  HistoryAndLogsPage();
  void bind(azzs::application::HistoryAndLogsService& service);
  void OnRefresh(winrt::Windows::Foundation::IInspectable const& sender,
                 Microsoft::UI::Xaml::RoutedEventArgs const& args);
  void OnFilterChanged(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::Controls::TextChangedEventArgs const& args);
  void OnExportDiagnostic(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);
  winrt::fire_and_forget OnClearLogs(
      winrt::Windows::Foundation::IInspectable const& sender,
      Microsoft::UI::Xaml::RoutedEventArgs const& args);

 private:
  void project(azzs::application::HistoryAndLogsSnapshot const& snapshot);
  void set_status(winrt::hstring const& message,
                  Microsoft::UI::Xaml::Controls::InfoBarSeverity severity);

  azzs::application::HistoryAndLogsService* service_{};
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct HistoryAndLogsPage
    : HistoryAndLogsPageT<HistoryAndLogsPage,
                          implementation::HistoryAndLogsPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
