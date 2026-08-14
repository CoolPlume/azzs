#include "pch.h"

#include "SoftwareOptimizationPage.xaml.h"

#include <string>
#include <string_view>

#include "azzs/application/software_optimization_discovery.hpp"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

#if __has_include("Pages/SoftwareOptimizationPage.g.cpp")
#include "Pages/SoftwareOptimizationPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {
namespace {

using azzs::application::software_optimization_discovery::DiscoveryActionCode;
using azzs::application::software_optimization_discovery::
    SoftwareOptimizationDiscoverySnapshot;
using azzs::domain::software_optimization_discovery::OptionState;
using azzs::domain::software_optimization_discovery::SchemeDiscovery;
using azzs::domain::software_optimization_discovery::SchemeState;
using winrt::Microsoft::UI::Xaml::Automation::AutomationProperties;
using winrt::Microsoft::UI::Xaml::Controls::Border;
using winrt::Microsoft::UI::Xaml::Controls::Button;
using winrt::Microsoft::UI::Xaml::Controls::CheckBox;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialog;
using winrt::Microsoft::UI::Xaml::Controls::ContentDialogResult;
using winrt::Microsoft::UI::Xaml::Controls::InfoBarSeverity;
using winrt::Microsoft::UI::Xaml::Controls::StackPanel;
using winrt::Microsoft::UI::Xaml::Controls::TextBlock;

[[nodiscard]] winrt::hstring display_name(SchemeDiscovery const& scheme) {
  if (scheme.scheme.id.value == "sogou-input-recommended-v1") {
    return L"搜狗输入法推荐优化";
  }
  return winrt::to_hstring(scheme.scheme.id.value);
}

[[nodiscard]] winrt::hstring option_tag(SchemeDiscovery const& scheme,
                                         std::string_view option_id) {
  return winrt::to_hstring(scheme.scheme.id.value + "\n" +
                           std::string{option_id});
}

[[nodiscard]] bool parse_option_tag(winrt::hstring const& value,
                                    std::string& scheme_id,
                                    std::string& option_id) {
  auto const text = std::wstring{value.c_str(), value.size()};
  auto const separator = text.find(L'\n');
  if (separator == std::wstring::npos || separator == 0 ||
      separator + 1 >= text.size()) {
    return false;
  }
  scheme_id = winrt::to_string(winrt::hstring{text.substr(0, separator)});
  option_id =
      winrt::to_string(winrt::hstring{text.substr(separator + 1)});
  return true;
}

[[nodiscard]] bool needs_attention(SchemeState state) noexcept {
  return state == SchemeState::needs_attention ||
         state == SchemeState::version_not_applicable ||
         state == SchemeState::emergency_withdrawn ||
         state == SchemeState::configuration_error ||
         state == SchemeState::manual_only;
}

}  // namespace

SoftwareOptimizationPage::SoftwareOptimizationPage() {
  InitializeComponent();
}

void SoftwareOptimizationPage::bind(
    azzs::application::software_optimization_discovery::
        SoftwareOptimizationDiscoveryService& service,
    bool advanced_view) {
  service_ = &service;
  advanced_view_ = advanced_view;
  auto result = service_->refresh();
  project(result.snapshot);
}

void SoftwareOptimizationPage::OnRefresh(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (service_ == nullptr) {
    return;
  }
  auto result = service_->refresh();
  project(result.snapshot);
}

void SoftwareOptimizationPage::OnPrepareSelected(
    winrt::Windows::Foundation::IInspectable const&,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (service_ == nullptr) {
    return;
  }
  auto result = service_->prepare_submission();
  project(result.snapshot);
  if (result.code == DiscoveryActionCode::no_executable_selection) {
    set_status(L"没有可提交的已选优化项。", InfoBarSeverity::Warning);
    return;
  }
  auto const count = result.submission.has_value()
                         ? result.submission->selected_options.size()
                         : 0;
  set_status(
      winrt::hstring{L"已准备 " + std::to_wstring(count) +
                     L" 个所选优化项；尚未创建优化批次。"},
      InfoBarSeverity::Informational);
}

winrt::fire_and_forget SoftwareOptimizationPage::OnOptionSelectionChanged(
    winrt::Windows::Foundation::IInspectable const& sender,
    Microsoft::UI::Xaml::RoutedEventArgs const&) {
  auto lifetime = get_strong();
  if (service_ == nullptr) {
    co_return;
  }
  auto const check_box = sender.try_as<CheckBox>();
  if (!check_box) {
    co_return;
  }
  std::string scheme_id;
  std::string option_id;
  if (!parse_option_tag(winrt::unbox_value<winrt::hstring>(check_box.Tag()),
                        scheme_id, option_id)) {
    co_return;
  }
  auto mutation = azzs::domain::software_optimization_discovery::SelectionMutation{
      .scheme_id = {std::move(scheme_id)},
      .option_id = {std::move(option_id)},
      .selected = check_box.IsChecked() && check_box.IsChecked().Value(),
  };
  auto result = service_->change_selection(mutation);
  if (result.code == DiscoveryActionCode::adjustment_confirmation_required) {
    ContentDialog dialog;
    dialog.XamlRoot(XamlRoot());
    dialog.Title(winrt::box_value(L"确认选择调整"));
    dialog.Content(winrt::box_value(L"此选择会补选必需项或取消冲突项。确认后才会更新选择。"));
    dialog.PrimaryButtonText(L"确认调整");
    dialog.CloseButtonText(L"取消");
    if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
      mutation.accept_adjustments = true;
      result = service_->change_selection(std::move(mutation));
    }
  }
  project(result.snapshot);
  if (result.code == DiscoveryActionCode::selection_rejected) {
    set_status(winrt::to_hstring(result.message), InfoBarSeverity::Warning);
  }
}

void SoftwareOptimizationPage::project(
    SoftwareOptimizationDiscoverySnapshot const& snapshot) {
  CanOptimizeItems().Children().Clear();
  NeedsAttentionItems().Children().Clear();
  OptimizedItems().Children().Clear();
  NoAvailableItems().Children().Clear();
  if (!snapshot.has_current_catalog) {
    set_status(snapshot.error.empty() ? L"当前没有可用的软件优化目录。"
                                      : winrt::to_hstring(snapshot.error),
               InfoBarSeverity::Error);
    PrepareSelectedButton().IsEnabled(false);
    return;
  }

  StatusInfoBar().IsOpen(false);
  PrepareSelectedButton().IsEnabled(false);
  for (auto const& target : snapshot.discovery.targets) {
    for (auto const& scheme : target.schemes) {
      auto card = Border{};
      card.BorderThickness({1, 1, 1, 1});
      card.CornerRadius({4, 4, 4, 4});
      card.Padding({12, 12, 12, 12});
      auto content = StackPanel{};
      content.Spacing(6);
      auto title = TextBlock{};
      title.Text(display_name(scheme));
      content.Children().Append(title);
      auto impact = TextBlock{};
      impact.Text(winrt::to_hstring(scheme.scheme.impact));
      impact.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      content.Children().Append(impact);
      auto status = TextBlock{};
      status.Text(winrt::to_hstring(scheme.detail));
      status.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      content.Children().Append(status);
      if (advanced_view_) {
        auto details = TextBlock{};
        auto version = scheme.installed_version.has_value()
                           ? winrt::to_hstring(*scheme.installed_version)
                           : winrt::hstring{L"未识别"};
        details.Text(L"版本：" + version + L"；适用范围：" +
                     winrt::to_hstring(scheme.scheme.supported_versions.minimum) +
                     L" - " +
                     winrt::to_hstring(scheme.scheme.supported_versions.maximum));
        details.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        content.Children().Append(details);
      }
      for (auto const& option : scheme.options) {
        auto check_box = CheckBox{};
        auto label = winrt::to_hstring(option.option.impact);
        if (option.option.required) {
          label = label + L"（必需）";
        }
        check_box.Content(winrt::box_value(label));
        check_box.IsChecked(option.selected);
        check_box.IsEnabled(scheme.state == SchemeState::can_optimize &&
                            option.state == OptionState::needs_optimization &&
                            !option.option.required);
        check_box.Tag(winrt::box_value(option_tag(scheme, option.option.id.value)));
        AutomationProperties::SetName(check_box, label);
        check_box.Checked({this, &SoftwareOptimizationPage::OnOptionSelectionChanged});
        check_box.Unchecked({this, &SoftwareOptimizationPage::OnOptionSelectionChanged});
        content.Children().Append(check_box);
      }
      if (advanced_view_ &&
          (scheme.state == SchemeState::version_not_applicable ||
           scheme.state == SchemeState::needs_attention)) {
        auto force = TextBlock{};
        force.Text(L"高级视图会在执行前单独披露强制执行或强制关闭的不可撤销风险。当前页面不会创建批次。\n");
        force.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
        content.Children().Append(force);
      }
      card.Child(content);
      if (scheme.state == SchemeState::can_optimize) {
        CanOptimizeItems().Children().Append(card);
        PrepareSelectedButton().IsEnabled(true);
      } else if (scheme.state == SchemeState::optimized) {
        OptimizedItems().Children().Append(card);
      } else if (target.no_available_optimization &&
                 scheme.state == SchemeState::manual_only) {
        NoAvailableItems().Children().Append(card);
      } else if (needs_attention(scheme.state)) {
        NeedsAttentionItems().Children().Append(card);
      }
    }
    if (target.no_available_optimization && target.schemes.empty()) {
      auto text = TextBlock{};
      text.Text(winrt::to_hstring(target.detail));
      text.TextWrapping(Microsoft::UI::Xaml::TextWrapping::Wrap);
      NoAvailableItems().Children().Append(text);
    }
  }
  CanOptimizeGroup().Visibility(CanOptimizeItems().Children().Size() == 0
                                    ? Microsoft::UI::Xaml::Visibility::Collapsed
                                    : Microsoft::UI::Xaml::Visibility::Visible);
  NeedsAttentionGroup().Visibility(NeedsAttentionItems().Children().Size() == 0
                                      ? Microsoft::UI::Xaml::Visibility::Collapsed
                                      : Microsoft::UI::Xaml::Visibility::Visible);
  OptimizedGroup().Visibility(OptimizedItems().Children().Size() == 0 &&
                                        NoAvailableItems().Children().Size() == 0
                                    ? Microsoft::UI::Xaml::Visibility::Collapsed
                                    : Microsoft::UI::Xaml::Visibility::Visible);
  NoAvailableOptimizationExpander().Visibility(
      NoAvailableItems().Children().Size() == 0
          ? Microsoft::UI::Xaml::Visibility::Collapsed
          : Microsoft::UI::Xaml::Visibility::Visible);
}

void SoftwareOptimizationPage::set_status(
    winrt::hstring const& message,
    Microsoft::UI::Xaml::Controls::InfoBarSeverity severity) {
  StatusInfoBar().Severity(severity);
  StatusInfoBar().Message(message);
  StatusInfoBar().IsOpen(true);
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
