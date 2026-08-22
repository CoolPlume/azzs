#include "pch.h"

#include "PageHeader.xaml.h"
#include "../../native_resource_fallback.hpp"

#include <string>

#if __has_include("DesignSystem/Controls/PageHeader.g.cpp")
#include "DesignSystem/Controls/PageHeader.g.cpp"
#endif

namespace {

}  // namespace

namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation {

PageHeader::PageHeader() {
  InitializeComponent();
  if (TitlePresenter().Content() == nullptr) {
    auto fallback = Microsoft::UI::Xaml::Controls::TextBlock{};
    fallback.Style(winrt::Microsoft::UI::Xaml::Application::Current()
                       .Resources().Lookup(
                       box_value(L"AzzsPageTitleTextStyle"))
                       .as<Microsoft::UI::Xaml::Style>());
    auto const resource =
        winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader{}
            .GetString(L"PageHeaderFallbackTitle");
    auto const native_resource =
        azzs::ui::winui::native_resources::load_string(
            AZZS_NATIVE_STRING_PAGE_HEADER_FALLBACK_TITLE);
    fallback.Text(resource.empty() ? winrt::hstring{native_resource} : resource);
    fallback.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
    TitleContent(fallback);
  }
}

Windows::Foundation::IInspectable PageHeader::TitleContent() {
  return title_content_;
}

void PageHeader::TitleContent(Windows::Foundation::IInspectable const& value) {
  title_content_ = value;
  TitlePresenter().Content(value);
  update_visibility();
}

Windows::Foundation::IInspectable PageHeader::SummaryContent() {
  return summary_content_;
}

void PageHeader::SummaryContent(Windows::Foundation::IInspectable const& value) {
  summary_content_ = value;
  SummaryPresenter().Content(value);
  update_visibility();
}

Windows::Foundation::IInspectable PageHeader::CommandContent() {
  return command_content_;
}

void PageHeader::CommandContent(Windows::Foundation::IInspectable const& value) {
  command_content_ = value;
  CommandHost().Content(value);
  update_visibility();
}

void PageHeader::update_visibility() {
  SummaryPresenter().Visibility(summary_content_ == nullptr
                                    ? Microsoft::UI::Xaml::Visibility::Collapsed
                                    : Microsoft::UI::Xaml::Visibility::Visible);
  CommandHost().Visibility(command_content_ == nullptr
                               ? Microsoft::UI::Xaml::Visibility::Collapsed
                               : Microsoft::UI::Xaml::Visibility::Visible);
}

}  // namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation
