#include "hardware_information_presentation.hpp"

#include <initializer_list>
#include <optional>
#include <string_view>
#include <utility>

namespace azzs::ui::presentation {
namespace {

using application::HardwareDeviceKind;
using application::HardwareDeviceRecord;
using application::HardwareDisplayConnection;
using application::HardwareInputDeviceType;
using application::HardwareNetworkLink;
using application::HardwareStorageMedia;

[[nodiscard]] bool matches(HardwareDeviceRecord const& device,
                           HardwareDeviceKind kind,
                           std::optional<HardwareNetworkLink> link = std::nullopt,
                           std::optional<HardwareStorageMedia> storage = std::nullopt,
                           std::optional<HardwareInputDeviceType> input = std::nullopt) {
  return device.confirmed_physical() && device.kind == kind &&
         (!link.has_value() || device.network_link == *link) &&
         (!storage.has_value() || device.storage_media == *storage) &&
         (!input.has_value() || device.input_device_type == *input);
}

[[nodiscard]] std::string join(std::vector<std::string> const& values) {
  std::string result;
  for (auto const& value : values) {
    if (!result.empty()) {
      result += '\n';
    }
    result += value;
  }
  return result;
}

[[nodiscard]] std::string join_details(std::vector<std::string> const& values) {
  std::string result;
  for (auto const& value : values) {
    if (!result.empty()) {
      result += "; ";
    }
    result += value;
  }
  return result;
}

[[nodiscard]] std::string format_display(
    HardwareDeviceRecord const& device,
    HardwareInformationPresentationText const& text) {
  auto result = device.name;
  std::vector<std::string> details;
  if (device.display_width != 0 && device.display_height != 0) {
    details.push_back(std::to_string(device.display_width) + " \xC3\x97 " +
                      std::to_string(device.display_height));
  }
  if (device.display_refresh_rate_hz != 0) {
    details.push_back(std::to_string(device.display_refresh_rate_hz) + " Hz");
  }
  switch (device.display_connection) {
    case HardwareDisplayConnection::internal:
      details.push_back(text.display_internal);
      break;
    case HardwareDisplayConnection::external:
      details.push_back(text.display_external);
      break;
    case HardwareDisplayConnection::unknown:
      break;
  }
  if (!details.empty()) {
    result += " (";
    result += join_details(details);
    result += ")";
  }
  return result;
}

[[nodiscard]] std::string format_input(
    HardwareDeviceRecord const& device,
    HardwareInformationPresentationText const& text) {
  auto result = device.name;
  if (device.input_device_type == HardwareInputDeviceType::keyboard &&
      device.input_device_key_count != 0) {
    result += " (";
    result += std::to_string(device.input_device_key_count);
    result += text.keyboard_key_count_suffix;
    result += ")";
  }
  return result;
}

template <typename Formatter>
[[nodiscard]] std::vector<std::string> matching_values(
    application::HardwareObservation const& observation, HardwareDeviceKind kind,
    Formatter formatter, std::optional<HardwareNetworkLink> link = std::nullopt,
    std::optional<HardwareStorageMedia> storage = std::nullopt,
    std::optional<HardwareInputDeviceType> input = std::nullopt) {
  std::vector<std::string> values;
  for (auto const& device : observation.devices) {
    if (matches(device, kind, link, storage, input)) {
      values.push_back(formatter(device));
    }
  }
  return values;
}

void append_fixed_row(HardwareInformationPresentation& presentation,
                      HardwareInformationField field,
                      std::vector<std::string> values,
                      HardwareInformationPresentationText const& text) {
  presentation.rows.push_back({
      .field = field,
      .value = values.empty() ? text.unrecognized : join(values),
      .visible = true,
  });
}

void append_repeated_row(HardwareInformationPresentation& presentation,
                         HardwareInformationField field,
                         std::vector<std::string> values) {
  presentation.rows.push_back({
      .field = field,
      .value = join(values),
      .visible = !values.empty(),
  });
}

}  // namespace

HardwareInformationPresentation make_hardware_information_presentation(
    application::HardwareOverviewSnapshot const& snapshot,
    HardwareInformationPresentationText text) {
  HardwareInformationPresentation presentation{
      .unrecognized = snapshot.state == application::HardwareOverviewState::unrecognized,
  };
  auto const observation = snapshot.observation.value_or(
      application::HardwareObservation{});
  auto const direct = [](std::string const& value) {
    if (value.empty()) {
      return std::vector<std::string>{};
    }
    return std::vector<std::string>{value};
  };
  auto const devices = [&observation](HardwareDeviceKind kind) {
    return matching_values(observation, kind,
                           [](HardwareDeviceRecord const& device) {
                             return device.name;
                           });
  };

  append_fixed_row(presentation, HardwareInformationField::model,
                   direct(observation.oem_model), text);
  append_fixed_row(presentation, HardwareInformationField::operating_system,
                   direct(observation.operating_system), text);
  append_fixed_row(presentation, HardwareInformationField::cpu,
                   devices(HardwareDeviceKind::cpu), text);
  append_fixed_row(presentation, HardwareInformationField::motherboard,
                   devices(HardwareDeviceKind::motherboard), text);
  append_fixed_row(presentation, HardwareInformationField::memory,
                   devices(HardwareDeviceKind::memory), text);
  append_fixed_row(presentation, HardwareInformationField::gpu,
                   devices(HardwareDeviceKind::gpu), text);
  append_repeated_row(
      presentation, HardwareInformationField::display,
      matching_values(observation, HardwareDeviceKind::display,
                      [&text](HardwareDeviceRecord const& device) {
                        return format_display(device, text);
                      }));
  append_repeated_row(
      presentation, HardwareInformationField::solid_state_storage,
      matching_values(observation, HardwareDeviceKind::storage,
                      [](HardwareDeviceRecord const& device) {
                        return device.name;
                      }, std::nullopt, HardwareStorageMedia::solid_state));
  append_repeated_row(
      presentation, HardwareInformationField::hard_disk_storage,
      matching_values(observation, HardwareDeviceKind::storage,
                      [](HardwareDeviceRecord const& device) {
                        return device.name;
                      }, std::nullopt, HardwareStorageMedia::hard_disk));
  append_repeated_row(
      presentation, HardwareInformationField::wired_network,
      matching_values(observation, HardwareDeviceKind::network_adapter,
                      [](HardwareDeviceRecord const& device) {
                        return device.name;
                      }, HardwareNetworkLink::wired));
  append_repeated_row(
      presentation, HardwareInformationField::wireless_network,
      matching_values(observation, HardwareDeviceKind::network_adapter,
                      [](HardwareDeviceRecord const& device) {
                        return device.name;
                      }, HardwareNetworkLink::wireless));
  append_repeated_row(presentation, HardwareInformationField::audio,
                      devices(HardwareDeviceKind::audio));
  append_repeated_row(presentation, HardwareInformationField::npu,
                      devices(HardwareDeviceKind::npu));
  for (auto const [field, input_type] :
       std::initializer_list<std::pair<HardwareInformationField,
                                       HardwareInputDeviceType>>{
           {HardwareInformationField::keyboard, HardwareInputDeviceType::keyboard},
           {HardwareInformationField::mouse, HardwareInputDeviceType::mouse},
           {HardwareInformationField::touchpad, HardwareInputDeviceType::touchpad},
       }) {
    append_repeated_row(
        presentation, field,
        matching_values(observation, HardwareDeviceKind::input_device,
                        [&text](HardwareDeviceRecord const& device) {
                          return format_input(device, text);
                        }, std::nullopt, std::nullopt, input_type));
  }
  return presentation;
}

}  // namespace azzs::ui::presentation
