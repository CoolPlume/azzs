#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "hardware_information_presentation.hpp"

namespace {

using azzs::application::HardwareDeviceKind;
using azzs::application::HardwareDevicePhysicality;
using azzs::application::HardwareDisplayConnection;
using azzs::application::HardwareInputDeviceType;
using azzs::application::HardwareObservation;
using azzs::application::HardwareObservationConfidence;
using azzs::application::HardwareObservationSource;
using azzs::application::HardwareOverviewSnapshot;
using azzs::application::HardwareOverviewState;
using azzs::ui::presentation::HardwareInformationField;
using azzs::ui::presentation::HardwareInformationPresentation;
using azzs::ui::presentation::HardwareInformationPresentationText;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "hardware information presentation contract failed: " << message
              << '\n';
  }
  return condition;
}

[[nodiscard]] HardwareInformationPresentationText localized_text() {
  return {
      .unrecognized = "未识别",
      .display_internal = "内置",
      .display_external = "外接",
      .keyboard_key_count_suffix = " 键",
  };
}

[[nodiscard]] azzs::application::HardwareDeviceRecord physical_device(
    HardwareDeviceKind kind, std::string name) {
  return {
      .kind = kind,
      .name = std::move(name),
      .physicality = HardwareDevicePhysicality::confirmed_physical,
      .source = HardwareObservationSource::wmi,
      .confidence = HardwareObservationConfidence::confirmed,
      .physically_present = true,
  };
}

[[nodiscard]] auto const* row_for(HardwareInformationPresentation const& presentation,
                                  HardwareInformationField field) {
  auto const found = std::find_if(
      presentation.rows.begin(), presentation.rows.end(), [field](auto const& row) {
        return row.field == field;
      });
  return found == presentation.rows.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] bool fixed_rows_use_unrecognized_and_empty_repeated_rows_hide() {
  auto const presentation =
      azzs::ui::presentation::make_hardware_information_presentation(
          HardwareOverviewSnapshot{.state = HardwareOverviewState::ready,
                                   .observation = HardwareObservation{}},
          localized_text());

  bool passed = true;
  for (auto const field : {HardwareInformationField::model,
                           HardwareInformationField::operating_system,
                           HardwareInformationField::cpu,
                           HardwareInformationField::motherboard,
                           HardwareInformationField::memory,
                           HardwareInformationField::gpu}) {
    auto const* row = row_for(presentation, field);
    passed &= expect(row != nullptr && row->visible && row->value == "未识别",
                     "fixed summary fields must show the localized unrecognized value");
  }
  for (auto const field : {HardwareInformationField::display,
                           HardwareInformationField::solid_state_storage,
                           HardwareInformationField::hard_disk_storage,
                           HardwareInformationField::wired_network,
                           HardwareInformationField::wireless_network,
                           HardwareInformationField::audio,
                           HardwareInformationField::npu,
                           HardwareInformationField::keyboard,
                           HardwareInformationField::mouse,
                           HardwareInformationField::touchpad}) {
    auto const* row = row_for(presentation, field);
    passed &= expect(row != nullptr && !row->visible && row->value.empty(),
                     "empty repeated hardware categories must be hidden");
  }
  return passed;
}

[[nodiscard]] bool displays_preserve_order_and_omit_unknown_details() {
  HardwareObservation observation;
  auto internal = physical_device(HardwareDeviceKind::display, "内置显示器");
  internal.display_width = 1920;
  internal.display_height = 1080;
  internal.display_refresh_rate_hz = 60;
  internal.display_connection = HardwareDisplayConnection::internal;
  observation.devices.push_back(std::move(internal));
  observation.devices.push_back(
      physical_device(HardwareDeviceKind::display, "未知参数显示器"));
  auto external = physical_device(HardwareDeviceKind::display, "外接显示器");
  external.display_connection = HardwareDisplayConnection::external;
  observation.devices.push_back(std::move(external));

  auto const presentation =
      azzs::ui::presentation::make_hardware_information_presentation(
          HardwareOverviewSnapshot{.state = HardwareOverviewState::ready,
                                   .observation = std::move(observation)},
          localized_text());
  auto const* display = row_for(presentation, HardwareInformationField::display);
  return expect(display != nullptr && display->visible &&
                    display->value ==
                        "内置显示器 (1920 " "\xC3\x97" " 1080; 60 Hz; 内置)\n"
                        "未知参数显示器\n外接显示器 (外接)",
                "display rows must keep discovery order and omit unknown fields");
}

[[nodiscard]] bool only_confirmed_physical_devices_and_keyboard_suffix_are_projected() {
  HardwareObservation observation;
  auto keyboard = physical_device(HardwareDeviceKind::input_device, "标准键盘");
  keyboard.input_device_type = HardwareInputDeviceType::keyboard;
  keyboard.input_device_key_count = 104;
  observation.devices.push_back(std::move(keyboard));

  auto virtual_mouse = physical_device(HardwareDeviceKind::input_device, "虚拟鼠标");
  virtual_mouse.input_device_type = HardwareInputDeviceType::mouse;
  virtual_mouse.physicality = HardwareDevicePhysicality::virtual_device;
  observation.devices.push_back(std::move(virtual_mouse));

  auto unknown_gpu = physical_device(HardwareDeviceKind::gpu, "未确认显卡");
  unknown_gpu.confidence = HardwareObservationConfidence::unknown;
  observation.devices.push_back(std::move(unknown_gpu));

  auto const presentation =
      azzs::ui::presentation::make_hardware_information_presentation(
          HardwareOverviewSnapshot{.state = HardwareOverviewState::ready,
                                   .observation = std::move(observation)},
          localized_text());
  auto const* keyboard_row =
      row_for(presentation, HardwareInformationField::keyboard);
  auto const* mouse_row = row_for(presentation, HardwareInformationField::mouse);
  auto const* gpu_row = row_for(presentation, HardwareInformationField::gpu);
  return expect(keyboard_row != nullptr && keyboard_row->visible &&
                    keyboard_row->value == "标准键盘 (104 键)",
                "keyboard key counts must use the localized Chinese suffix") &&
         expect(mouse_row != nullptr && !mouse_row->visible,
                "virtual input devices must not be projected") &&
         expect(gpu_row != nullptr && gpu_row->value == "未识别",
                "unconfirmed physicality must not be reinterpreted by the presentation layer");
}

}  // namespace

int main() {
  auto const passed =
      fixed_rows_use_unrecognized_and_empty_repeated_rows_hide() &&
      displays_preserve_order_and_omit_unknown_details() &&
      only_confirmed_physical_devices_and_keyboard_suffix_are_projected();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
