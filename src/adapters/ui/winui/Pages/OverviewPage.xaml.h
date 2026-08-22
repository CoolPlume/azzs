#pragma once

#include <functional>
#include <memory>

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.h"
#include "Pages/OverviewPage.g.h"
#include "azzs/application/page_id.hpp"

namespace azzs::application {
class WorkbenchServices;
}

namespace azzs::ui::presentation {
struct PresentationIntent;
class PresentationSnapshot;
}

namespace winrt::Azzs::Ui::Pages::implementation {

struct OverviewPage : OverviewPageT<OverviewPage> {
  OverviewPage();

  void bind(std::shared_ptr<azzs::application::WorkbenchServices> services,
            bool advanced_view,
            std::function<void(azzs::application::PageId)> navigate);

 private:
  void project();
  void handle_intent(
      azzs::ui::presentation::PresentationIntent const& intent);

  std::shared_ptr<azzs::application::WorkbenchServices> services_;
  std::shared_ptr<azzs::ui::presentation::PresentationSnapshot const>
      presentation_;
  std::function<void(azzs::application::PageId)> navigate_;
  bool advanced_view_{false};
};

}  // namespace winrt::Azzs::Ui::Pages::implementation

namespace winrt::Azzs::Ui::Pages::factory_implementation {

struct OverviewPage : OverviewPageT<OverviewPage, implementation::OverviewPage> {};

}  // namespace winrt::Azzs::Ui::Pages::factory_implementation
