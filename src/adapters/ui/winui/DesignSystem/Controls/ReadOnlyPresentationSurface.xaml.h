#pragma once

#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "../presentation_contract.hpp"

namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation {

struct ReadOnlyPresentationSurface
    : ReadOnlyPresentationSurfaceT<ReadOnlyPresentationSurface> {
  using IntentHandler =
      std::function<void(azzs::ui::presentation::PresentationIntent const&)>;

  ReadOnlyPresentationSurface();

  void project(
      std::shared_ptr<azzs::ui::presentation::PresentationSnapshot const>
          source,
      std::string_view component_id,
      azzs::ui::presentation::ViewMode mode,
      IntentHandler intent_handler,
      std::int32_t tab_index_start = 0,
      std::string automation_suffix = {});
  [[nodiscard]] bool focus_default_command();
  void OnLoaded(Windows::Foundation::IInspectable const&,
                Microsoft::UI::Xaml::RoutedEventArgs const&);

 private:
  void emit_intent(std::string const& component_id,
                   std::string const& command_id);

  std::optional<azzs::ui::presentation::ReadOnlyPresentation> presentation_;
  IntentHandler intent_handler_;
  Microsoft::UI::Xaml::Controls::Button default_command_{nullptr};
  std::string last_announcement_key_;
  std::string projected_component_id_;
  bool focus_default_when_loaded_{false};
};

}  // namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation

namespace winrt::Azzs::Ui::DesignSystem::Controls::factory_implementation {

struct ReadOnlyPresentationSurface
    : ReadOnlyPresentationSurfaceT<
          ReadOnlyPresentationSurface,
          implementation::ReadOnlyPresentationSurface> {};

}  // namespace winrt::Azzs::Ui::DesignSystem::Controls::factory_implementation
