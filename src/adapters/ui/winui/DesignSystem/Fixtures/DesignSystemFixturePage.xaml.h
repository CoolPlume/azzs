#pragma once

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.h"
#include "DesignSystem/Fixtures/DesignSystemFixturePage.g.h"

#include <memory>

#include "../presentation_contract.hpp"

namespace winrt::Azzs::Ui::DesignSystem::Fixtures::implementation {

struct DesignSystemFixturePage
    : DesignSystemFixturePageT<DesignSystemFixturePage> {
  DesignSystemFixturePage();

 private:
  void project_fixture(
      std::shared_ptr<azzs::ui::presentation::PresentationSnapshot const> const&
          snapshot);
  void project_intent(
      azzs::ui::presentation::PresentationIntent const& intent);
};

}  // namespace winrt::Azzs::Ui::DesignSystem::Fixtures::implementation

namespace winrt::Azzs::Ui::DesignSystem::Fixtures::factory_implementation {

struct DesignSystemFixturePage
    : DesignSystemFixturePageT<DesignSystemFixturePage,
                               implementation::DesignSystemFixturePage> {};

}  // namespace winrt::Azzs::Ui::DesignSystem::Fixtures::factory_implementation
