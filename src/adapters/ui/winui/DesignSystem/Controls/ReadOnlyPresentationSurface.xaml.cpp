#include "pch.h"

#include "ReadOnlyPresentationSurface.xaml.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#if __has_include("DesignSystem/Controls/ReadOnlyPresentationSurface.g.cpp")
#include "DesignSystem/Controls/ReadOnlyPresentationSurface.g.cpp"
#endif

namespace {

using azzs::ui::presentation::CommandProjection;
using azzs::ui::presentation::CommandRole;
using azzs::ui::presentation::ComponentKind;
using azzs::ui::presentation::ComponentProjection;
using azzs::ui::presentation::IntentKind;
using azzs::ui::presentation::PresentationState;
using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
using winrt::Microsoft::UI::Xaml::Automation::Peers::AutomationLiveSetting;
using winrt::Microsoft::UI::Xaml::Controls::Button;
using winrt::Microsoft::UI::Xaml::Controls::FontIcon;
using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
using winrt::Microsoft::UI::Xaml::Controls::TextBlock;
using winrt::Microsoft::UI::Xaml::Controls::UserControl;
using winrt::Microsoft::UI::Xaml::Style;
using XamlVisibility = winrt::Microsoft::UI::Xaml::Visibility;

[[nodiscard]] winrt::Windows::Foundation::IInspectable shared_resource(
    std::wstring_view key) {
  return winrt::Microsoft::UI::Xaml::Application::Current().Resources().Lookup(
      winrt::box_value(winrt::hstring{key}));
}

[[nodiscard]] Style shared_style(std::wstring_view key) {
  return shared_resource(key).as<Style>();
}

[[nodiscard]] bool is_status(ComponentKind kind) noexcept {
  switch (kind) {
    case ComponentKind::status_band:
    case ComponentKind::inline_error:
    case ComponentKind::waiting:
    case ComponentKind::failure:
    case ComponentKind::pending_confirmation:
    case ComponentKind::emergency_withdrawal:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] InfoBarSeverity severity_for(
    PresentationState state) noexcept {
  switch (state) {
    case PresentationState::failed:
      return InfoBarSeverity::Error;
    case PresentationState::pending_confirmation:
    case PresentationState::withdrawn:
    case PresentationState::local_trial:
    case PresentationState::recovered_unsaved:
      return InfoBarSeverity::Warning;
    default:
      return InfoBarSeverity::Informational;
  }
}

[[nodiscard]] AutomationLiveSetting live_setting_for(
    azzs::ui::presentation::AnnouncementMode mode) noexcept {
  switch (mode) {
    case azzs::ui::presentation::AnnouncementMode::polite:
      return AutomationLiveSetting::Polite;
    case azzs::ui::presentation::AnnouncementMode::assertive:
      return AutomationLiveSetting::Assertive;
    case azzs::ui::presentation::AnnouncementMode::none:
      return AutomationLiveSetting::Off;
  }
  return AutomationLiveSetting::Off;
}

[[nodiscard]] std::wstring_view surface_style_for(
    ComponentProjection const& component) noexcept {
  if (component.kind == ComponentKind::risk_confirmation) {
    return L"AzzsRiskConfirmationSurfaceStyle";
  }
  switch (component.kind) {
    case ComponentKind::list:
      return L"AzzsListRowSurfaceStyle";
    case ComponentKind::detail:
      return L"AzzsDetailSurfaceStyle";
    case ComponentKind::summary:
    case ComponentKind::stage_summary:
      return L"AzzsSummarySurfaceStyle";
    default:
      return L"AzzsSectionSurfaceStyle";
  }
}

[[nodiscard]] std::wstring_view command_style_for(
    CommandProjection const& command) noexcept {
  if (command.intent.kind == IntentKind::locate_result) {
    return L"AzzsResultLocatorButtonStyle";
  }
  switch (command.role) {
    case CommandRole::primary:
      return L"AzzsPrimaryCommandButtonStyle";
    case CommandRole::danger:
      return L"AzzsDangerCommandButtonStyle";
    case CommandRole::secondary:
    case CommandRole::navigation:
      return L"AzzsSecondaryCommandButtonStyle";
  }
  return L"AzzsSecondaryCommandButtonStyle";
}

[[nodiscard]] std::wstring_view icon_key_for(IntentKind kind) noexcept {
  switch (kind) {
    case IntentKind::toggle_item_selection:
    case IntentKind::continue_workflow:
      return L"AzzsIconSuccess";
    case IntentKind::retry:
      return L"AzzsIconProgress";
    case IntentKind::confirm_risk:
      return L"AzzsIconRisk";
    case IntentKind::stop_safely:
      return L"AzzsIconWarning";
    case IntentKind::open_emergency_withdrawal:
      return L"AzzsIconEmergencyWithdrawal";
    case IntentKind::locate_result:
    case IntentKind::open_source:
      return L"AzzsIconResultLocator";
    case IntentKind::open_details:
    case IntentKind::expand_summary:
      return L"AzzsIconInformation";
  }
  return L"AzzsIconInformation";
}

[[nodiscard]] StackPanel command_content(CommandProjection const& command) {
  StackPanel content;
  content.Orientation(
      winrt::Microsoft::UI::Xaml::Controls::Orientation::Horizontal);
  content.Spacing(winrt::unbox_value<double>(
      shared_resource(L"AzzsSpaceSmall")));

  FontIcon icon;
  icon.FontFamily(shared_resource(L"AzzsIconFontFamily")
                      .as<winrt::Microsoft::UI::Xaml::Media::FontFamily>());
  icon.Glyph(winrt::unbox_value<winrt::hstring>(
      shared_resource(icon_key_for(command.intent.kind))));
  content.Children().Append(icon);

  TextBlock label;
  label.Text(winrt::to_hstring(command.label));
  label.TextWrapping(winrt::Microsoft::UI::Xaml::TextWrapping::Wrap);
  content.Children().Append(label);
  return content;
}

[[nodiscard]] std::string automation_id_for(
    ComponentProjection const& component,
    std::string const& automation_suffix) {
  if (automation_suffix.empty()) {
    return component.automation_id;
  }
  return component.automation_id + "." + automation_suffix;
}

}  // namespace

namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation {

ReadOnlyPresentationSurface::ReadOnlyPresentationSurface() {
  InitializeComponent();
}

void ReadOnlyPresentationSurface::project(
    std::shared_ptr<azzs::ui::presentation::PresentationSnapshot const> source,
    std::string_view component_id,
    azzs::ui::presentation::ViewMode mode,
    IntentHandler intent_handler,
    std::int32_t tab_index_start,
    std::string automation_suffix) {
  auto const component_changed = component_id != projected_component_id_;
  projected_component_id_ = component_id;
  presentation_.emplace(std::move(source), mode);
  intent_handler_ = std::move(intent_handler);
  default_command_ = nullptr;
  CardCommandHost().Children().Clear();
  StatusCommandHost().Children().Clear();

  auto const* component = presentation_->source().find_component(component_id);
  if (component == nullptr || !presentation_->visible(*component)) {
    last_announcement_key_.clear();
    focus_default_when_loaded_ = false;
    Visibility(XamlVisibility::Collapsed);
    return;
  }

  Visibility(XamlVisibility::Visible);
  auto const status = is_status(component->kind);
  CardSurface().Visibility(status ? XamlVisibility::Collapsed
                                  : XamlVisibility::Visible);
  StatusSurface().Visibility(status ? XamlVisibility::Visible
                                    : XamlVisibility::Collapsed);

  auto const automation_id = automation_id_for(*component, automation_suffix);
  auto const automation_name = winrt::to_hstring(component->accessible_name);
  auto const root =
      static_cast<winrt::Windows::Foundation::IInspectable>(*this)
          .as<UserControl>();
  AutomationProperties::SetAutomationId(root,
                                        winrt::to_hstring(automation_id));
  auto const announcement_key =
      component->id + ":" +
      std::to_string(static_cast<int>(component->state)) + ":" +
      component->title + ":" + component->body;
  auto const announce = component->announcement !=
                            azzs::ui::presentation::AnnouncementMode::none &&
                        announcement_key != last_announcement_key_;
  auto const live_setting =
      announce ? live_setting_for(component->announcement)
               : AutomationLiveSetting::Off;
  if (announce) {
    last_announcement_key_ = announcement_key;
  } else if (component->announcement ==
             azzs::ui::presentation::AnnouncementMode::none) {
    last_announcement_key_.clear();
  }
  AutomationProperties::SetLiveSetting(root, live_setting);
  AutomationProperties::SetName(
      root, winrt::to_hstring(component->accessible_name + ": " +
                              component->title + ". " + component->body));
  AutomationProperties::SetLiveSetting(StatusBand(),
                                       AutomationLiveSetting::Off);
  AutomationProperties::SetLiveSetting(CardSurface(),
                                       AutomationLiveSetting::Off);

  if (status) {
    StatusBand().Title(winrt::to_hstring(component->title));
    StatusBand().Message(winrt::to_hstring(component->body));
    StatusBand().Severity(severity_for(component->state));
    AutomationProperties::SetAutomationId(
        StatusBand(), winrt::to_hstring(automation_id + ".StatusBand"));
    AutomationProperties::SetName(StatusBand(), automation_name);
  } else {
    CardSurface().Style(shared_style(surface_style_for(*component)));
    TitleText().Text(winrt::to_hstring(component->title));
    BodyText().Text(winrt::to_hstring(component->body));
    BodyText().Style(shared_style(
        component->kind == ComponentKind::disabled_reason
            ? L"AzzsDisabledReasonTextStyle"
            : L"AzzsBodyTextStyle"));
    AutomationProperties::SetAutomationId(
        CardSurface(), winrt::to_hstring(automation_id + ".Card"));
    AutomationProperties::SetName(CardSurface(), automation_name);

    auto const show_advanced =
        mode == azzs::ui::presentation::ViewMode::advanced &&
        !component->advanced_detail.empty();
    AdvancedDetailText().Text(winrt::to_hstring(component->advanced_detail));
    AdvancedDetailText().Visibility(show_advanced ? XamlVisibility::Visible
                                                  : XamlVisibility::Collapsed);
  }
  auto command_host = status ? StatusCommandHost() : CardCommandHost();
  auto const has_default = std::ranges::any_of(
      component->commands, [this](CommandProjection const& command) {
        return presentation_->visible(command) && command.enabled &&
               command.default_focus && command.role != CommandRole::danger;
      });
  auto next_tab_index = tab_index_start + (has_default ? 1 : 0);
  auto weak_this = get_weak();
  for (auto const& command : component->commands) {
    if (!presentation_->visible(command)) {
      continue;
    }

    Button button;
    button.Content(command_content(command));
    button.IsEnabled(command.enabled);
    button.Style(shared_style(command_style_for(command)));
    AutomationProperties::SetAutomationId(
        button,
        winrt::to_hstring(automation_id + "." + command.id));
    AutomationProperties::SetName(button, winrt::to_hstring(command.label));
    if (!command.disabled_reason.empty()) {
      AutomationProperties::SetHelpText(
          button, winrt::to_hstring(command.disabled_reason));
    } else if (command.role == CommandRole::danger) {
      AutomationProperties::SetHelpText(
          button, winrt::to_hstring(component->body));
    }

    auto const is_default = command.default_focus && command.enabled &&
                            command.role != CommandRole::danger;
    button.TabIndex(is_default ? tab_index_start : next_tab_index++);
    if (is_default && !default_command_) {
      default_command_ = button;
    }

    auto command_id = command.id;
    auto owner_id = component->id;
    button.Click([weak_this, owner_id = std::move(owner_id),
                  command_id = std::move(command_id)](auto const&,
                                                       auto const&) {
      if (auto self = weak_this.get()) {
        self->emit_intent(owner_id, command_id);
      }
    });
    command_host.Children().Append(button);
  }

  command_host.Visibility(command_host.Children().Size() == 0
                              ? XamlVisibility::Collapsed
                              : XamlVisibility::Visible);
  if (component_changed) {
    focus_default_when_loaded_ =
        component->kind == ComponentKind::risk_confirmation &&
        static_cast<bool>(default_command_);
  } else if (!default_command_) {
    focus_default_when_loaded_ = false;
  }
  if (focus_default_when_loaded_ && IsLoaded() && focus_default_command()) {
    focus_default_when_loaded_ = false;
  }
}

bool ReadOnlyPresentationSurface::focus_default_command() {
  return default_command_ && default_command_.Focus(
                                 Microsoft::UI::Xaml::FocusState::Programmatic);
}

void ReadOnlyPresentationSurface::OnLoaded(
    Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (focus_default_when_loaded_ && focus_default_command()) {
    focus_default_when_loaded_ = false;
  }
}

void ReadOnlyPresentationSurface::emit_intent(
    std::string const& component_id,
    std::string const& command_id) {
  if (!presentation_ || !intent_handler_) {
    return;
  }
  auto const intent = presentation_->intent_for(component_id, command_id);
  if (intent.has_value()) {
    intent_handler_(*intent);
  }
}

}  // namespace winrt::Azzs::Ui::DesignSystem::Controls::implementation
