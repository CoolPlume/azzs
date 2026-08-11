#include "pch.h"

#include "ApplicationSettingsPage.xaml.h"

#if __has_include("Pages/ApplicationSettingsPage.g.cpp")
#include "Pages/ApplicationSettingsPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {

ApplicationSettingsPage::ApplicationSettingsPage() {
  InitializeComponent();
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
