#pragma once

#include "Pages/HistoryAndLogsPage.g.h"

namespace winrt::Azzs::Ui::Pages::implementation {

struct HistoryAndLogsPage : HistoryAndLogsPageT<HistoryAndLogsPage> {
  HistoryAndLogsPage();
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct HistoryAndLogsPage
    : HistoryAndLogsPageT<HistoryAndLogsPage,
                          implementation::HistoryAndLogsPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
