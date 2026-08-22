#pragma once

#include "DesignSystem/Controls/PageHeader.g.h"

namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation {

struct PageHeader : PageHeaderT<PageHeader> {
  PageHeader();

  Windows::Foundation::IInspectable TitleContent();
  void TitleContent(Windows::Foundation::IInspectable const& value);
  Windows::Foundation::IInspectable SummaryContent();
  void SummaryContent(Windows::Foundation::IInspectable const& value);
  Windows::Foundation::IInspectable CommandContent();
  void CommandContent(Windows::Foundation::IInspectable const& value);

 private:
  void update_visibility();

  Windows::Foundation::IInspectable title_content_{nullptr};
  Windows::Foundation::IInspectable summary_content_{nullptr};
  Windows::Foundation::IInspectable command_content_{nullptr};
};

}  // namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation

namespace winrt::Azzs::Ui::DesignSystem::Controls::factory_implementation {

struct PageHeader : PageHeaderT<PageHeader, implementation::PageHeader> {};

}  // namespace winrt::Azzs::Ui::DesignSystem::Controls::factory_implementation
