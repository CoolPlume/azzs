#include "pch.h"

#include "ApplicationSettingsPage.xaml.h"

#if __has_include("ApplicationSettingsPage.g.cpp")
#include "ApplicationSettingsPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {

ApplicationSettingsPage::ApplicationSettingsPage() {
  InitializeComponent();
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
