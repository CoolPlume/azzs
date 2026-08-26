#include "pch.h"

#include "HardwareInformationPage.xaml.h"

#include <array>
#include <string>
#include <utility>

#include "../DesignSystem/hardware_information_presentation.hpp"

#if __has_include("Pages/HardwareInformationPage.g.cpp")
#include "Pages/HardwareInformationPage.g.cpp"
#endif

namespace winrt::Azzs::Ui::Pages::implementation {
namespace {

using azzs::ui::presentation::HardwareInformationField;
using PresentationText = azzs::ui::presentation::HardwareInformationPresentationText;
using PresentationMember = std::string PresentationText::*;

struct PresentationResource final {
  wchar_t const* key;
  PresentationMember member;
};

constexpr std::array kPresentationResources{
    PresentationResource{L"HardwareUnrecognizedValue", &PresentationText::unrecognized},
    PresentationResource{L"HardwareDisplayInternal", &PresentationText::display_internal},
    PresentationResource{L"HardwareDisplayExternal", &PresentationText::display_external},
    PresentationResource{L"HardwareKeyboardKeyCountSuffix", &PresentationText::keyboard_key_count_suffix},
};

[[nodiscard]] PresentationText presentation_text(
    winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader const&
        resources) {
  PresentationText text;
  for (auto const& resource : kPresentationResources) {
    text.*resource.member = winrt::to_string(resources.GetString(resource.key));
  }
  return text;
}

void set_row(winrt::Microsoft::UI::Xaml::FrameworkElement const& label,
             winrt::Microsoft::UI::Xaml::Controls::TextBlock const& value,
             bool visible, std::string const& text) {
  auto const visibility = visible ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                                  : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
  label.Visibility(visibility);
  value.Visibility(visibility);
  value.Text(winrt::to_hstring(text));
}

[[nodiscard]] wchar_t const* label_resource_key(HardwareInformationField field) {
  switch (field) {
    case HardwareInformationField::model: return L"HardwareModelLabel/Text";
    case HardwareInformationField::operating_system:
      return L"HardwareOperatingSystemLabel/Text";
    case HardwareInformationField::cpu: return L"HardwareCpuLabel/Text";
    case HardwareInformationField::motherboard:
      return L"HardwareMotherboardLabel/Text";
    case HardwareInformationField::memory: return L"HardwareMemoryLabel/Text";
    case HardwareInformationField::gpu: return L"HardwareGpuLabel/Text";
    case HardwareInformationField::display: return L"HardwareDisplayLabel/Text";
    case HardwareInformationField::solid_state_storage:
      return L"HardwareSolidStateStorageLabel/Text";
    case HardwareInformationField::hard_disk_storage:
      return L"HardwareHardDiskStorageLabel/Text";
    case HardwareInformationField::wired_network:
      return L"HardwareWiredNetworkLabel/Text";
    case HardwareInformationField::wireless_network:
      return L"HardwareWirelessNetworkLabel/Text";
    case HardwareInformationField::audio: return L"HardwareAudioLabel/Text";
    case HardwareInformationField::npu: return L"HardwareNpuLabel/Text";
    case HardwareInformationField::keyboard:
      return L"HardwareKeyboardLabel/Text";
    case HardwareInformationField::mouse: return L"HardwareMouseLabel/Text";
    case HardwareInformationField::touchpad:
      return L"HardwareTouchpadLabel/Text";
  }
  return L"";
}

}  // namespace

HardwareInformationPage::HardwareInformationPage() {
  InitializeComponent();
}

void HardwareInformationPage::bind(
    azzs::application::HardwareOverviewSnapshot const& snapshot,
    RefreshHandler refresh_handler) {
  refresh_handler_ = std::move(refresh_handler);
  project(snapshot);
}

void HardwareInformationPage::OnRefreshClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  if (refresh_handler_) {
    refresh_handler_();
  }
}

void HardwareInformationPage::OnCopyHardwareClicked(
    winrt::Windows::Foundation::IInspectable const&,
    winrt::Microsoft::UI::Xaml::RoutedEventArgs const&) {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;
  using winrt::Windows::ApplicationModel::DataTransfer::Clipboard;
  using winrt::Windows::ApplicationModel::DataTransfer::DataPackage;

  auto const resources = ResourceLoader{};
  std::wstring text{
      resources.GetString(L"HardwareInformationFieldHeader/Text").c_str()};
  text += L"\t";
  text += resources.GetString(L"HardwareInformationValueHeader/Text").c_str();
  for (auto const& [label, value] : copy_rows_) {
    text += L"\r\n";
    text += label.c_str();
    text += L"\t";
    text += value.c_str();
  }
  DataPackage package;
  package.SetText(winrt::hstring{text});
  Clipboard::SetContent(package);
}

void HardwareInformationPage::project(
    azzs::application::HardwareOverviewSnapshot const& snapshot) {
  using winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader;

  auto const resources = ResourceLoader{};
  auto const presentation = azzs::ui::presentation::make_hardware_information_presentation(
      snapshot, presentation_text(resources));
  HardwareStatus().IsOpen(presentation.unrecognized);
  if (presentation.unrecognized) {
    HardwareStatus().Title(resources.GetString(L"HardwareStatusTitle"));
    HardwareStatus().Message(resources.GetString(L"HardwareStatusMessage"));
  }

  copy_rows_.clear();
  for (auto const& row : presentation.rows) {
    switch (row.field) {
      case HardwareInformationField::model: set_row(ModelLabel(), ModelValue(), row.visible, row.value); break;
      case HardwareInformationField::operating_system: set_row(OperatingSystemLabel(), OperatingSystemValue(), row.visible, row.value); break;
      case HardwareInformationField::cpu: set_row(CpuLabel(), CpuValue(), row.visible, row.value); break;
      case HardwareInformationField::motherboard: set_row(MotherboardLabel(), MotherboardValue(), row.visible, row.value); break;
      case HardwareInformationField::memory: set_row(MemoryLabel(), MemoryValue(), row.visible, row.value); break;
      case HardwareInformationField::gpu: set_row(GpuLabel(), GpuValue(), row.visible, row.value); break;
      case HardwareInformationField::display: set_row(DisplayLabel(), DisplayValue(), row.visible, row.value); break;
      case HardwareInformationField::solid_state_storage: set_row(SolidStateStorageLabel(), SolidStateStorageValue(), row.visible, row.value); break;
      case HardwareInformationField::hard_disk_storage: set_row(HardDiskStorageLabel(), HardDiskStorageValue(), row.visible, row.value); break;
      case HardwareInformationField::wired_network: set_row(WiredNetworkLabel(), WiredNetworkValue(), row.visible, row.value); break;
      case HardwareInformationField::wireless_network: set_row(WirelessNetworkLabel(), WirelessNetworkValue(), row.visible, row.value); break;
      case HardwareInformationField::audio: set_row(AudioLabel(), AudioValue(), row.visible, row.value); break;
      case HardwareInformationField::npu: set_row(NpuLabel(), NpuValue(), row.visible, row.value); break;
      case HardwareInformationField::keyboard: set_row(KeyboardLabel(), KeyboardValue(), row.visible, row.value); break;
      case HardwareInformationField::mouse: set_row(MouseLabel(), MouseValue(), row.visible, row.value); break;
      case HardwareInformationField::touchpad: set_row(TouchpadLabel(), TouchpadValue(), row.visible, row.value); break;
    }
    if (row.visible) {
      copy_rows_.emplace_back(resources.GetString(label_resource_key(row.field)),
                              winrt::to_hstring(row.value));
    }
  }
}

}  // namespace winrt::Azzs::Ui::Pages::implementation
