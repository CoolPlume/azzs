#include "azzs/adapters/windows/windows_hardware_observer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>
#include <setupapi.h>
#include <wingdi.h>

#include <dxgi1_6.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::adapters::windows {
namespace {

using Microsoft::WRL::ComPtr;

class ComApartment final {
 public:
  ComApartment() noexcept : result_(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
  ~ComApartment() {
    if (result_ == S_OK || result_ == S_FALSE) {
      ::CoUninitialize();
    }
  }

  [[nodiscard]] bool usable() const noexcept {
    return result_ == S_OK || result_ == S_FALSE;
  }

 private:
  HRESULT result_;
};

class RegistryKey final {
 public:
  explicit RegistryKey(HKEY key = nullptr) noexcept : key_(key) {}
  ~RegistryKey() {
    if (key_ != nullptr) {
      ::RegCloseKey(key_);
    }
  }
  RegistryKey(RegistryKey const&) = delete;
  RegistryKey& operator=(RegistryKey const&) = delete;

  [[nodiscard]] HKEY get() const noexcept { return key_; }

 private:
  HKEY key_;
};

class DeviceInfoSet final {
 public:
  explicit DeviceInfoSet(HDEVINFO handle = INVALID_HANDLE_VALUE) noexcept
      : handle_(handle) {}
  ~DeviceInfoSet() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::SetupDiDestroyDeviceInfoList(handle_);
    }
  }
  DeviceInfoSet(DeviceInfoSet const&) = delete;
  DeviceInfoSet& operator=(DeviceInfoSet const&) = delete;

  [[nodiscard]] HDEVINFO get() const noexcept { return handle_; }
  [[nodiscard]] bool usable() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE;
  }

 private:
  HDEVINFO handle_;
};

[[nodiscard]] std::string utf8_from_wide(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  auto const size = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
    return {};
  }
  return result;
}

[[nodiscard]] std::wstring wide_from_utf8(std::string_view value) {
  if (value.empty()) {
    return {};
  }
  auto const size = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size) !=
      size) {
    return {};
  }
  return result;
}

[[nodiscard]] std::string setup_device_instance_id(
    HDEVINFO device_info_set, SP_DEVINFO_DATA& device_info) {
  DWORD required_characters = 0;
  if (::SetupDiGetDeviceInstanceIdW(device_info_set, &device_info, nullptr, 0,
                                    &required_characters) != FALSE ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      required_characters == 0) {
    return {};
  }
  std::vector<wchar_t> buffer(required_characters);
  if (::SetupDiGetDeviceInstanceIdW(
          device_info_set, &device_info, buffer.data(),
          static_cast<DWORD>(buffer.size()), &required_characters) == FALSE) {
    return {};
  }
  auto length = buffer.size();
  while (length != 0 && buffer[length - 1] == L'\0') {
    --length;
  }
  return utf8_from_wide(std::wstring_view{buffer.data(), length});
}

[[nodiscard]] std::string setup_device_property_string(
    HDEVINFO device_info_set, SP_DEVINFO_DATA& device_info,
    DEVPROPKEY const& property_key) {
  DEVPROPTYPE property_type = 0;
  DWORD required_bytes = 0;
  if (::SetupDiGetDevicePropertyW(device_info_set, &device_info, &property_key,
                                  &property_type, nullptr, 0, &required_bytes,
                                  0) != FALSE ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
      required_bytes == 0 ||
      required_bytes > 32 * 1024) {
    return {};
  }
  auto const character_count =
      (static_cast<std::size_t>(required_bytes) + sizeof(wchar_t) - 1) /
      sizeof(wchar_t);
  std::vector<wchar_t> buffer(character_count, L'\0');
  if (::SetupDiGetDevicePropertyW(
          device_info_set, &device_info, &property_key, &property_type,
          reinterpret_cast<PBYTE>(buffer.data()), required_bytes,
          &required_bytes, 0) == FALSE ||
      property_type != DEVPROP_TYPE_STRING) {
    return {};
  }
  auto length = buffer.size();
  while (length != 0 && buffer[length - 1] == L'\0') {
    --length;
  }
  return utf8_from_wide(std::wstring_view{buffer.data(), length});
}

[[nodiscard]] std::string setup_device_container_id(
    HDEVINFO device_info_set, SP_DEVINFO_DATA& device_info) {
  GUID value{};
  DEVPROPTYPE property_type = 0;
  DWORD required_bytes = 0;
  if (::SetupDiGetDevicePropertyW(
          device_info_set, &device_info, &DEVPKEY_Device_ContainerId,
          &property_type, reinterpret_cast<PBYTE>(&value), sizeof(value),
          &required_bytes, 0) == FALSE ||
      property_type != DEVPROP_TYPE_GUID || required_bytes != sizeof(value)) {
    return {};
  }
  std::array<wchar_t, 40> buffer{};
  auto const characters = ::StringFromGUID2(
      value, buffer.data(), static_cast<int>(buffer.size()));
  if (characters <= 1) {
    return {};
  }
  return utf8_from_wide(
      std::wstring_view{buffer.data(), static_cast<std::size_t>(characters - 1)});
}

[[nodiscard]] std::string join_error(std::string current,
                                     std::string_view prefix,
                                     std::string_view detail) {
  if (!current.empty()) {
    current.append("; ");
  }
  current.append(prefix);
  if (!detail.empty()) {
    current.append(": ");
    current.append(detail);
  }
  return current;
}

[[nodiscard]] std::string pair_value(
    WindowsHardwareQueryResult const& result) {
  if (result.rows.empty()) {
    return {};
  }
  std::string combined;
  for (auto const& value : result.rows.front()) {
    if (!value.empty()) {
      if (!combined.empty()) {
        combined.push_back(' ');
      }
      combined.append(value);
    }
  }
  return combined;
}

[[nodiscard]] std::string lower_ascii(std::string_view value) {
  std::string result{value};
  for (auto& character : result) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return result;
}

[[nodiscard]] bool contains_ascii(std::string_view value,
                                  std::string_view needle) {
  return lower_ascii(value).find(lower_ascii(needle)) != std::string::npos;
}

[[nodiscard]] bool starts_with_ascii(std::string_view value,
                                     std::string_view prefix) {
  auto const lowered = lower_ascii(value);
  auto const lowered_prefix = lower_ascii(prefix);
  return lowered.size() >= lowered_prefix.size() &&
         lowered.compare(0, lowered_prefix.size(), lowered_prefix) == 0;
}

[[nodiscard]] std::string trim_ascii(std::string_view value) {
  auto first = std::size_t{0};
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }
  auto last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1])) != 0) {
    --last;
  }
  return std::string{value.substr(first, last - first)};
}

[[nodiscard]] std::string strip_trademark_markers(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    if (value[index] == '(') {
      auto const close = value.find(')', index + 1);
      if (close != std::string_view::npos && close - index <= 5) {
        auto const marker = lower_ascii(
            trim_ascii(value.substr(index + 1, close - index - 1)));
        if (marker == "r" || marker == "tm" || marker == "c" ||
            marker == "sm") {
          index = close + 1;
          continue;
        }
      }
    }
    constexpr std::array<std::string_view, 3> symbol_markers{
        "\xC2\xAE",  // registered sign
        "\xE2\x84\xA2",  // trademark sign
        "\xC2\xA9",  // copyright sign
    };
    bool skipped_symbol = false;
    for (auto const marker : symbol_markers) {
      if (value.compare(index, marker.size(), marker) == 0) {
        index += marker.size();
        skipped_symbol = true;
        break;
      }
    }
    if (skipped_symbol) {
      continue;
    }
    result.push_back(value[index++]);
  }
  while (result.find("  ") != std::string::npos) {
    result.replace(result.find("  "), 2, " ");
  }
  return trim_ascii(result);
}

struct LocalizedBrand final {
  std::string_view marker;
  std::string_view display;
};

// Keep the original model text and replace only a confirmed vendor token.
// This gives people who recognize a Chinese vendor name a useful cue without
// turning a hardware observation into a guessed model translation.
constexpr std::array<LocalizedBrand, 30> kLocalizedBrands{
    LocalizedBrand{"titan army", "泰坦军团 TITAN ARMY"},
    LocalizedBrand{"p275mv", "泰坦军团 TITAN ARMY P275MV"},
    LocalizedBrand{"sk hynix", "SK 海力士"},
    LocalizedBrand{"hynix", "SK 海力士"},
    LocalizedBrand{"samsung", "三星 Samsung"},
    LocalizedBrand{"micron", "美光 Micron"},
    LocalizedBrand{"crucial", "英睿达 Crucial"},
    LocalizedBrand{"colorful", "七彩虹 COLORFUL"},
    LocalizedBrand{"七彩虹", "七彩虹 COLORFUL"},
    LocalizedBrand{"micro-star", "微星 MSI"},
    LocalizedBrand{"msi", "微星 MSI"},
    LocalizedBrand{"gigabyte", "技嘉 GIGABYTE"},
    LocalizedBrand{"zotac", "索泰 ZOTAC"},
    LocalizedBrand{"sapphire", "蓝宝石 SAPPHIRE"},
    LocalizedBrand{"realtek", "瑞昱 Realtek"},
    LocalizedBrand{"boe", "京东方 BOE"},
    LocalizedBrand{"nvidia", "英伟达 NVIDIA"},
    LocalizedBrand{"amd", "AMD"},
    LocalizedBrand{"qualcomm", "高通 Qualcomm"},
    LocalizedBrand{"matrox", "Matrox"},
    LocalizedBrand{"aspeed", "ASPEED"},
    LocalizedBrand{"powercolor", "撼讯 PowerColor"},
    LocalizedBrand{"xfx", "讯景 XFX"},
    LocalizedBrand{"evga", "EVGA"},
    LocalizedBrand{"intel", "英特尔 Intel"},
    LocalizedBrand{"hewlett", "惠普 HP"},
    LocalizedBrand{"hp", "惠普 HP"},
    LocalizedBrand{"asus", "华硕 ASUS"},
    LocalizedBrand{"seagate", "希捷 Seagate"},
    LocalizedBrand{"western digital", "西部数据 WD"},
};

[[nodiscard]] LocalizedBrand const* localized_brand_for(
    std::string_view value) noexcept {
  for (auto const& brand : kLocalizedBrands) {
    if (contains_ascii(value, brand.marker)) {
      return &brand;
    }
  }
  return nullptr;
}

[[nodiscard]] std::string replace_first_ascii_case_insensitive(
    std::string value, std::string_view marker, std::string_view replacement) {
  auto const position = lower_ascii(value).find(lower_ascii(marker));
  if (position != std::string::npos) {
    value.replace(position, marker.size(), replacement);
  }
  return value;
}

[[nodiscard]] std::string localized_brand_model(
    std::string_view model, std::string_view manufacturer) {
  auto result = strip_trademark_markers(trim_ascii(model));
  if (result.empty()) {
    return {};
  }
  auto const* brand = localized_brand_for(result);
  if (brand == nullptr) {
    brand = localized_brand_for(manufacturer);
  }
  if (brand == nullptr) {
    return result;
  }

  for (auto const& candidate : kLocalizedBrands) {
    if (candidate.display == brand->display &&
        contains_ascii(result, candidate.marker)) {
      return strip_trademark_markers(replace_first_ascii_case_insensitive(
          std::move(result), candidate.marker, candidate.display));
    }
  }
  return strip_trademark_markers(std::string{brand->display} + " " + result);
}

[[nodiscard]] std::string localized_memory_model(
    std::string_view manufacturer, std::string_view part_number) {
  // Micron also manufactures non-Crucial DIMMs.  Only its CT-prefixed part
  // numbers identify the Crucial retail line when WMI does not name it.
  if (contains_ascii(manufacturer, "crucial") ||
      (contains_ascii(manufacturer, "micron") &&
       starts_with_ascii(part_number, "ct"))) {
    return "英睿达 Crucial";
  }
  if (contains_ascii(manufacturer, "micron")) {
    return "美光 Micron";
  }
  return localized_brand_model(manufacturer, manufacturer);
}

// Win32_DiskDrive can prefix a model with a localized device-class label,
// for example "(标准磁盘驱动器) Samsung SSD 990 PRO".  That label is not part
// of the hardware model and must not leak into the compact overview summary.
[[nodiscard]] std::string strip_storage_class_prefix(std::string_view model) {
  auto value = trim_ascii(model);
  if (value.size() >= 2 && value.front() == '(') {
    auto const close = value.find(')');
    if (close != std::string::npos && close + 1 < value.size()) {
      value = trim_ascii(value.substr(close + 1));
    }
  }
  return value;
}

// DISPLAY PNP ids contain an instance suffix after the second '\\'.  The
// first model token is stable across Win32_DesktopMonitor and Win32_PnPEntity
// projections and is sufficient for in-session de-duplication.
[[nodiscard]] std::string display_model_token(std::string_view pnp_id) {
  auto const first_separator = pnp_id.find('\\');
  if (first_separator == std::string_view::npos) {
    return {};
  }
  auto const token_begin = first_separator + 1;
  auto const second_separator = pnp_id.find('\\', token_begin);
  auto const token_end = second_separator == std::string_view::npos
                             ? pnp_id.size()
                             : second_separator;
  return trim_ascii(pnp_id.substr(token_begin, token_end - token_begin));
}

[[nodiscard]] std::string display_model_key(std::string_view pnp_id) {
  return lower_ascii(display_model_token(pnp_id));
}

// Full PNP ids are the only stable identity available to the adapter for
// device-instance matching. Keep the normalization deliberately narrow:
// case and surrounding ASCII whitespace are benign, while every other token
// remains part of the identity.
[[nodiscard]] std::string normalized_instance_key(std::string_view pnp_id) {
  return lower_ascii(trim_ascii(pnp_id));
}

// PNP identifiers are needed only while the Windows adapter gathers one
// observation. Public adapter facts retain this ephemeral ordinal instead.
[[nodiscard]] std::uint32_t display_instance_ordinal_for(
    std::span<std::string const> pnp_device_ids,
    std::string_view stable_instance_key) {
  auto const normalized_key = normalized_instance_key(stable_instance_key);
  if (normalized_key.empty()) {
    return 0;
  }
  for (std::size_t index = 0; index < pnp_device_ids.size(); ++index) {
    if (normalized_instance_key(pnp_device_ids[index]) == normalized_key) {
      return static_cast<std::uint32_t>(index + 1);
    }
  }
  return 0;
}

[[nodiscard]] std::size_t display_model_instance_count(
    std::span<std::string const> pnp_device_ids, std::string_view model_key) {
  auto const normalized_key = lower_ascii(trim_ascii(model_key));
  if (normalized_key.empty()) {
    return 0;
  }
  return static_cast<std::size_t>(std::ranges::count_if(
      pnp_device_ids, [&](std::string const& pnp_device_id) {
        return display_model_key(pnp_device_id) == normalized_key;
      }));
}

[[nodiscard]] std::string display_model_key_from_monitor_device_path(
    std::wstring_view device_path) {
  constexpr std::wstring_view kDisplayMarker = L"DISPLAY#";
  auto ascii_equal = [](wchar_t left, wchar_t right) noexcept {
    if (left >= L'a' && left <= L'z') {
      left = static_cast<wchar_t>(left - (L'a' - L'A'));
    }
    if (right >= L'a' && right <= L'z') {
      right = static_cast<wchar_t>(right - (L'a' - L'A'));
    }
    return left == right;
  };
  for (std::size_t offset = 0;
       offset + kDisplayMarker.size() <= device_path.size(); ++offset) {
    bool matches = true;
    for (std::size_t marker_index = 0; marker_index < kDisplayMarker.size();
         ++marker_index) {
      if (!ascii_equal(device_path[offset + marker_index],
                       kDisplayMarker[marker_index])) {
        matches = false;
        break;
      }
    }
    if (!matches) {
      continue;
    }
    auto const token_begin = offset + kDisplayMarker.size();
    auto const token_end = device_path.find(L'#', token_begin);
    auto const token = device_path.substr(
        token_begin, token_end == std::wstring_view::npos
                        ? device_path.size() - token_begin
                        : token_end - token_begin);
    return lower_ascii(trim_ascii(utf8_from_wide(token)));
  }
  return {};
}

[[nodiscard]] std::string display_instance_key_from_monitor_device_path(
    std::wstring_view device_path) {
  constexpr std::wstring_view kDisplayMarker = L"DISPLAY#";
  auto ascii_equal = [](wchar_t left, wchar_t right) noexcept {
    if (left >= L'a' && left <= L'z') {
      left = static_cast<wchar_t>(left - (L'a' - L'A'));
    }
    if (right >= L'a' && right <= L'z') {
      right = static_cast<wchar_t>(right - (L'a' - L'A'));
    }
    return left == right;
  };
  for (std::size_t offset = 0;
       offset + kDisplayMarker.size() <= device_path.size(); ++offset) {
    bool matches = true;
    for (std::size_t marker_index = 0; marker_index < kDisplayMarker.size();
         ++marker_index) {
      if (!ascii_equal(device_path[offset + marker_index],
                       kDisplayMarker[marker_index])) {
        matches = false;
        break;
      }
    }
    if (!matches) {
      continue;
    }
    auto const model_begin = offset + kDisplayMarker.size();
    auto const instance_begin = device_path.find(L'#', model_begin);
    if (instance_begin == std::wstring_view::npos) {
      return {};
    }
    auto const instance_end = device_path.find(L'#', instance_begin + 1);
    if (instance_end == std::wstring_view::npos ||
        instance_end <= instance_begin + 1) {
      return {};
    }
    auto const model = lower_ascii(trim_ascii(utf8_from_wide(
        device_path.substr(model_begin, instance_begin - model_begin))));
    auto const instance = lower_ascii(trim_ascii(utf8_from_wide(
        device_path.substr(instance_begin + 1,
                           instance_end - instance_begin - 1))));
    if (model.empty() || instance.empty()) {
      return {};
    }
    return normalized_instance_key("display\\" + model + "\\" + instance);
  }
  return {};
}

[[nodiscard]] bool generic_display_name(std::string_view name) {
  return contains_ascii(name, "generic pnp monitor") ||
         contains_ascii(name, "generic monitor") ||
         contains_ascii(name, "generic plug and play monitor") ||
         contains_ascii(name, "integrated monitor") ||
         name.find("\xE9\x80\x9A\xE7\x94\xA8\xE5\x8D\xB3\xE6\x8F\x92\xE5\x8D\xB3\xE7\x94\xA8\xE7\x9B\x91\xE8\xA7\x86\xE5\x99\xA8") !=
             std::string_view::npos ||
         name.find("\xE9\xBB\x98\xE8\xAE\xA4\xE7\x9B\x91\xE8\xA7\x86\xE5\x99\xA8") !=
             std::string_view::npos ||
         name.find("\xE9\x9B\x86\xE6\x88\x90\xE7\x9B\x91\xE8\xA7\x86\xE5\x99\xA8") !=
             std::string_view::npos;
}

[[nodiscard]] std::string parenthetical_display_model(
    std::string_view name) {
  auto const open = name.rfind('(');
  auto const close = name.rfind(')');
  if (open == std::string_view::npos || close == std::string_view::npos ||
      close <= open + 1) {
    return {};
  }
  auto const candidate = trim_ascii(name.substr(open + 1, close - open - 1));
  if (candidate.empty() || contains_ascii(candidate, "monitor")) {
    return {};
  }
  return candidate;
}

[[nodiscard]] std::string format_display_name(std::string_view name,
                                               std::string_view pnp_id,
                                               bool include_model_token) {
  auto const generic_name = generic_display_name(name);
  auto model = parenthetical_display_model(name);
  if (model.empty() && !generic_name) {
    model = trim_ascii(name);
  }
  auto const model_token = display_model_token(pnp_id);
  if (model.empty()) {
    model = model_token;
  }
  if (model.empty()) {
    model = trim_ascii(name);
  }
  if (include_model_token && generic_name && !model_token.empty() &&
      lower_ascii(model).find(lower_ascii(model_token)) == std::string::npos) {
    model += " [";
    model += model_token;
    model += "]";
  }
  return model;
}

[[nodiscard]] application::HardwareDisplayConnection
display_connection_from_technology(
    DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) noexcept {
  using application::HardwareDisplayConnection;
  switch (technology) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
      return HardwareDisplayConnection::internal;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDTVDONGLE:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL:
      return HardwareDisplayConnection::external;
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL:
    default:
      return HardwareDisplayConnection::unknown;
  }
}

[[nodiscard]] std::optional<std::uint32_t> refresh_rate_from_rational(
    DISPLAYCONFIG_RATIONAL rational) noexcept {
  if (rational.Numerator == 0 || rational.Denominator == 0) {
    return std::nullopt;
  }
  auto const rounded =
      (static_cast<std::uint64_t>(rational.Numerator) +
       static_cast<std::uint64_t>(rational.Denominator) / 2) /
      static_cast<std::uint64_t>(rational.Denominator);
  // A rational outside the range of a monitor mode is not a usable fact.
  if (rounded == 0 || rounded > 1000) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(rounded);
}

[[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>>
dimensions_from_target_mode(DISPLAYCONFIG_PATH_INFO const& path,
                            std::span<DISPLAYCONFIG_MODE_INFO const> modes) {
  auto same_luid = [](LUID left, LUID right) noexcept {
    return left.HighPart == right.HighPart && left.LowPart == right.LowPart;
  };
  auto inspect = [&](std::uint32_t index)
      -> std::optional<std::pair<std::uint32_t, std::uint32_t>> {
    if (index >= modes.size()) {
      return std::nullopt;
    }
    auto const& mode = modes[index];
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
        !same_luid(mode.adapterId, path.targetInfo.adapterId)) {
      return std::nullopt;
    }
    auto const width = mode.targetMode.targetVideoSignalInfo.activeSize.cx;
    auto const height = mode.targetMode.targetVideoSignalInfo.activeSize.cy;
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
      return std::nullopt;
    }
    return std::pair<std::uint32_t, std::uint32_t>{width, height};
  };

  // QDC without QDC_VIRTUAL_MODE_AWARE returns a direct modeInfoIdx.  On
  // systems that expose virtual modes, the target-mode half of the union is
  // the useful fallback; both candidates are validated against the mode type
  // and adapter LUID before being trusted.
  if (auto dimensions = inspect(path.targetInfo.modeInfoIdx);
      dimensions.has_value()) {
    return dimensions;
  }
  auto const target_mode_index =
      static_cast<std::uint32_t>(path.targetInfo.targetModeInfoIdx);
  if (target_mode_index != DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID) {
    return inspect(target_mode_index);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<WindowsDisplayConnection>
display_connection_for(std::span<WindowsDisplayConnection const> facts,
                        std::string_view model_key,
                        std::uint32_t instance_ordinal,
                        std::size_t model_instance_count) {
  auto const normalized_key = lower_ascii(trim_ascii(model_key));
  if (normalized_key.empty()) {
    return std::nullopt;
  }

  // Prefer facts tied to this exact instance. If only model-scoped facts are
  // available, aggregate them and keep a field only when every valid source
  // agrees; this is safe for same-model multi-monitor setups and fails closed
  // on conflicting paths.
  auto exact_fact = [&](WindowsDisplayConnection const& fact) {
    return instance_ordinal != 0 &&
           fact.instance_ordinal == instance_ordinal &&
           lower_ascii(trim_ascii(fact.model_key)) == normalized_key;
  };
  bool has_exact = false;
  for (auto const& fact : facts) {
    if (exact_fact(fact)) {
      has_exact = true;
      break;
    }
  }
  if (!has_exact && model_instance_count != 1) {
    // A model-scoped path cannot identify one of several identical monitors.
    return std::nullopt;
  }

  std::optional<application::HardwareDisplayConnection> connection;
  std::optional<std::pair<std::uint32_t, std::uint32_t>> dimensions;
  std::optional<std::uint32_t> refresh_rate;
  bool connection_ambiguous = false;
  bool dimensions_ambiguous = false;
  bool refresh_ambiguous = false;
  bool found = false;
  for (auto const& fact : facts) {
    if (lower_ascii(trim_ascii(fact.model_key)) != normalized_key) {
      continue;
    }
    if (has_exact ? !exact_fact(fact) : fact.instance_ordinal != 0) {
      continue;
    }
    found = true;
    if (fact.connection == application::HardwareDisplayConnection::unknown) {
      connection_ambiguous = true;
    } else if (!connection.has_value()) {
      connection = fact.connection;
    } else if (*connection != fact.connection) {
      connection_ambiguous = true;
    }

    if (fact.width == 0 || fact.height == 0) {
      dimensions_ambiguous = true;
    } else if (!dimensions.has_value()) {
      dimensions = std::pair{fact.width, fact.height};
    } else if (*dimensions != std::pair{fact.width, fact.height}) {
      dimensions_ambiguous = true;
    }

    if (fact.refresh_rate_hz == 0) {
      refresh_ambiguous = true;
    } else if (!refresh_rate.has_value()) {
      refresh_rate = fact.refresh_rate_hz;
    } else if (*refresh_rate != fact.refresh_rate_hz) {
      refresh_ambiguous = true;
    }
  }
  if (!found) {
    return std::nullopt;
  }

  WindowsDisplayConnection result{.model_key = normalized_key,
                                  .instance_ordinal =
                                      has_exact ? instance_ordinal : 0};
  if (!connection_ambiguous && connection.has_value()) {
    result.connection = *connection;
  }
  if (!dimensions_ambiguous && dimensions.has_value()) {
    result.width = dimensions->first;
    result.height = dimensions->second;
  }
  if (!refresh_ambiguous && refresh_rate.has_value()) {
    result.refresh_rate_hz = *refresh_rate;
  }
  return result;
}

[[nodiscard]] std::string presented_display_name(
    std::string_view name, std::string_view manufacturer,
    std::string_view pnp_id) {
  return localized_brand_model(format_display_name(name, pnp_id, false),
                               manufacturer);
}

[[nodiscard]] std::optional<std::uint32_t>
physical_refresh_rate_limit_from_edid(
    std::span<std::uint8_t const> edid) noexcept {
  constexpr std::array<std::uint8_t, 8> kEdidHeader{
      0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  if (edid.size() < 128 ||
      !std::ranges::equal(kEdidHeader, edid.first(kEdidHeader.size()))) {
    return std::nullopt;
  }
  std::uint32_t checksum = 0;
  for (std::size_t index = 0; index < 128; ++index) {
    checksum += edid[index];
  }
  if (checksum % 256 != 0 || edid[18] != 1) {
    return std::nullopt;
  }

  std::optional<std::uint32_t> limit;
  // VESA E-EDID A2, monitor range limits descriptor (pp. 38-40):
  // https://glenwing.github.io/docs/VESA-EEDID-A2.pdf
  for (auto const descriptor : {54u, 72u, 90u, 108u}) {
    if (edid[descriptor] != 0 || edid[descriptor + 1] != 0 ||
        edid[descriptor + 2] != 0 || edid[descriptor + 3] != 0xfd) {
      continue;
    }
    auto const flags = edid[descriptor + 4];
    // EDID 1.4 defines only the low four range-offset bits. Earlier
    // revisions do not define offsets; reserved combinations fail closed.
    if ((flags & 0xf0) != 0 || (edid[19] < 4 && flags != 0)) {
      return std::nullopt;
    }
    auto minimum = static_cast<std::uint32_t>(edid[descriptor + 5]);
    auto maximum = static_cast<std::uint32_t>(edid[descriptor + 6]);
    if ((flags & 0x01) != 0) {
      minimum += 255;
    }
    if ((flags & 0x02) != 0) {
      maximum += 255;
    }
    if (minimum == 0 || maximum == 0 || maximum < minimum ||
        (limit.has_value() && *limit != maximum)) {
      return std::nullopt;
    }
    limit = maximum;
  }
  return limit;
}

[[nodiscard]] std::optional<std::uint32_t> physical_refresh_rate_limit_for(
    std::span<WindowsDisplayEdid const> edids,
    std::string_view model_key,
    std::uint32_t instance_ordinal,
    std::size_t model_instance_count) noexcept {
  std::optional<std::uint32_t> limit;
  auto const normalized_key = lower_ascii(trim_ascii(model_key));
  bool has_exact = false;
  for (auto const& edid : edids) {
    if (lower_ascii(trim_ascii(edid.model_key)) != normalized_key) {
      continue;
    }
    if (instance_ordinal != 0 && edid.instance_ordinal == instance_ordinal) {
      has_exact = true;
      break;
    }
  }
  if (!has_exact && model_instance_count != 1) {
    // A model-scoped EDID cannot identify one of several identical monitors.
    return std::nullopt;
  }
  bool found = false;
  for (auto const& edid : edids) {
    if (lower_ascii(trim_ascii(edid.model_key)) != normalized_key) {
      continue;
    }
    if (has_exact ? edid.instance_ordinal != instance_ordinal
                  : edid.instance_ordinal != 0) {
      continue;
    }
    found = true;
    auto const candidate = physical_refresh_rate_limit_from_edid(edid.bytes);
    if (!candidate.has_value() ||
        (limit.has_value() && *limit != *candidate)) {
      return std::nullopt;
    }
    limit = candidate;
  }
  return found ? limit : std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> display_size_tenths_from_centimeters(
    std::uint32_t horizontal_cm, std::uint32_t vertical_cm) noexcept {
  if (horizontal_cm == 0 || vertical_cm == 0) {
    return std::nullopt;
  }
  auto const horizontal = static_cast<double>(horizontal_cm);
  auto const vertical = static_cast<double>(vertical_cm);
  auto const diagonal_cm = std::sqrt(horizontal * horizontal + vertical * vertical);
  auto const tenths = static_cast<long>(std::lround(diagonal_cm * 10.0 / 2.54));
  if (tenths < 10 || tenths > 1000) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(tenths);
}

[[nodiscard]] std::optional<std::uint32_t> display_size_tenths_from_edid(
    std::span<std::uint8_t const> edid) noexcept {
  constexpr std::array<std::uint8_t, 8> kEdidHeader{
      0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
  if (edid.size() < 128 ||
      !std::ranges::equal(kEdidHeader, edid.first(kEdidHeader.size())) ||
      edid[18] != 1) {
    return std::nullopt;
  }
  std::uint32_t checksum = 0;
  for (std::size_t index = 0; index < 128; ++index) {
    checksum += edid[index];
  }
  if (checksum % 256 != 0) {
    return std::nullopt;
  }
  return display_size_tenths_from_centimeters(
      static_cast<std::uint32_t>(edid[21]),
      static_cast<std::uint32_t>(edid[22]));
}

[[nodiscard]] std::optional<std::uint32_t> display_size_tenths_for(
    std::span<WindowsDisplayEdid const> edids,
    std::string_view model_key,
    std::uint32_t instance_ordinal,
    std::size_t model_instance_count) noexcept {
  std::optional<std::uint32_t> size;
  auto const normalized_key = lower_ascii(trim_ascii(model_key));
  bool has_exact = false;
  for (auto const& edid : edids) {
    if (lower_ascii(trim_ascii(edid.model_key)) != normalized_key) {
      continue;
    }
    if (instance_ordinal != 0 && edid.instance_ordinal == instance_ordinal) {
      has_exact = true;
      break;
    }
  }
  if (!has_exact && model_instance_count != 1) {
    // A model-scoped EDID cannot identify one of several identical monitors.
    return std::nullopt;
  }
  bool found = false;
  for (auto const& edid : edids) {
    if (lower_ascii(trim_ascii(edid.model_key)) != normalized_key) {
      continue;
    }
    if (has_exact ? edid.instance_ordinal != instance_ordinal
                  : edid.instance_ordinal != 0) {
      continue;
    }
    found = true;
    auto const candidate = display_size_tenths_from_edid(edid.bytes);
    if (!candidate.has_value() ||
        (size.has_value() && *size != *candidate)) {
      return std::nullopt;
    }
    size = candidate;
  }
  return found ? size : std::nullopt;
}

[[nodiscard]] std::optional<std::uint32_t> display_size_tenths_for(
    std::span<WindowsDisplayPhysicalSize const> physical_sizes,
    std::string_view model_key,
    std::uint32_t instance_ordinal) noexcept {
  auto const normalized_key = lower_ascii(trim_ascii(model_key));
  if (normalized_key.empty() || instance_ordinal == 0) {
    return std::nullopt;
  }

  std::optional<std::uint32_t> size;
  bool found = false;
  for (auto const& physical_size : physical_sizes) {
    if (physical_size.instance_ordinal != instance_ordinal ||
        lower_ascii(trim_ascii(physical_size.model_key)) != normalized_key) {
      continue;
    }
    found = true;
    auto const candidate = display_size_tenths_from_centimeters(
        physical_size.horizontal_centimeters,
        physical_size.vertical_centimeters);
    if (!candidate.has_value() ||
        (size.has_value() && *size != *candidate)) {
      return std::nullopt;
    }
    size = candidate;
  }
  return found ? size : std::nullopt;
}

[[nodiscard]] std::string row_value(std::vector<std::string> const& row,
                                    std::size_t index) {
  return index < row.size() ? row[index] : std::string{};
}

[[nodiscard]] bool parse_bool(std::string_view value, bool& result) {
  auto const lowered = lower_ascii(value);
  if (lowered == "true" || lowered == "1" || lowered == "-1" ||
      lowered == "yes") {
    result = true;
    return true;
  }
  if (lowered == "false" || lowered == "0" || lowered == "no") {
    result = false;
    return true;
  }
  return false;
}

[[nodiscard]] bool parse_integer(std::string_view value, std::uint32_t& result) {
  if (value.empty()) {
    return false;
  }
  auto const* first = value.data();
  auto const* last = first + value.size();
  auto const parsed = std::from_chars(first, last, result);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] bool parse_unsigned(std::string_view value,
                                  std::uint64_t& result) {
  if (value.empty()) {
    return false;
  }
  auto const* first = value.data();
  auto const* last = first + value.size();
  auto const parsed = std::from_chars(first, last, result);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

[[nodiscard]] std::string decimal_gigabytes(std::uint64_t bytes) {
  // Windows exposes memory/disk capacities as bytes; report the conventional
  // rounded binary GB value so a 24 GiB DIMM is not shown as 25 GB.
  constexpr std::uint64_t kGigabyte = 1024ull * 1024ull * 1024ull;
  auto gigabytes = (bytes + kGigabyte / 2) / kGigabyte;
  if (gigabytes == 0 && bytes != 0) {
    gigabytes = 1;
  }
  return std::to_string(gigabytes) + "GB";
}

// Firmware model strings are inconsistent: a drive may include its marketed
// capacity ("2TB"), a binary conversion ("1863GB"), or both.  Capacity is a
// measured fact in Win32_DiskDrive::Size, so presentation strips every unit
// token from the model and appends exactly one value derived from that field.
[[nodiscard]] std::string strip_capacity_tokens(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size();) {
    auto const character = static_cast<unsigned char>(value[index]);
    if (std::isdigit(character) == 0) {
      result.push_back(value[index++]);
      continue;
    }

    auto const number_begin = index;
    while (index < value.size() &&
           (std::isdigit(static_cast<unsigned char>(value[index])) ||
            value[index] == '.')) {
      ++index;
    }
    auto unit_begin = index;
    while (unit_begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[unit_begin])) != 0) {
      ++unit_begin;
    }
    auto unit_length = std::size_t{0};
    constexpr std::array<std::string_view, 6> units{
        "tb", "gb", "mb", "tib", "gib", "mib"};
    auto const lowered = lower_ascii(value.substr(unit_begin));
    for (auto const unit : units) {
      if (lowered.size() >= unit.size() &&
          lowered.compare(0, unit.size(), unit) == 0) {
        unit_length = unit.size();
        break;
      }
    }
    if (unit_length == 0) {
      // The digits are part of the actual model (for example "990 PRO").
      result.append(value.substr(number_begin, index - number_begin));
      continue;
    }

    index = unit_begin + unit_length;
    while (index < value.size() &&
           std::isspace(static_cast<unsigned char>(value[index])) != 0) {
      ++index;
    }
    while (!result.empty() &&
           std::isspace(static_cast<unsigned char>(result.back())) != 0) {
      result.pop_back();
    }
    if (!result.empty()) {
      result.push_back(' ');
    }
  }
  return trim_ascii(result);
}

[[nodiscard]] std::string storage_capacity_text(std::uint64_t bytes) {
  constexpr std::uint64_t kDecimalGigabyte = 1'000'000'000ull;
  if (bytes >= 900ull * kDecimalGigabyte) {
    auto const tenths = (bytes + 50ull * kDecimalGigabyte) /
                        (100ull * kDecimalGigabyte);
    if (tenths % 10 == 0) {
      return std::to_string(tenths / 10) + "TB";
    }
    return std::to_string(tenths / 10) + "." +
           std::to_string(tenths % 10) + "TB";
  }
  auto gigabytes = (bytes + kDecimalGigabyte / 2) / kDecimalGigabyte;
  if (gigabytes == 0 && bytes != 0) {
    gigabytes = 1;
  }
  return std::to_string(gigabytes) + "GB";
}

[[nodiscard]] std::string gpu_memory_size(std::uint64_t bytes,
                                          bool whole_gigabytes) {
  constexpr std::uint64_t kMegabyte = 1024ull * 1024ull;
  constexpr std::uint64_t kGigabyte = 1024ull * 1024ull * 1024ull;
  if (bytes == 0) {
    return {};
  }
  if (bytes < kGigabyte) {
    auto const megabytes = (bytes + kMegabyte / 2) / kMegabyte;
    return std::to_string(megabytes == 0 ? 1 : megabytes) + " MB";
  }
  if (whole_gigabytes) {
    auto const gigabytes = (bytes + kGigabyte / 2) / kGigabyte;
    return std::to_string(gigabytes) + " GB";
  }
  auto const tenths = (bytes + kGigabyte / 20) / (kGigabyte / 10);
  if (tenths % 10 == 0) {
    return std::to_string(tenths / 10) + " GB";
  }
  return std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + " GB";
}

[[nodiscard]] std::string normalized_gpu_model_name(std::string_view value) {
  auto const without_trademark_markers = strip_trademark_markers(value);
  std::string result;
  result.reserve(without_trademark_markers.size());
  for (auto const character : without_trademark_markers) {
    auto const unsigned_character = static_cast<unsigned char>(character);
    if (std::isalnum(unsigned_character) != 0) {
      result.push_back(static_cast<char>(std::tolower(unsigned_character)));
    }
  }
  return result;
}

[[nodiscard]] bool generic_gpu_model_name(std::string_view value) {
  constexpr std::array<std::string_view, 4> kGenericGpuModelNames{
      "amdradeon", "amdradeongraphics", "radeongraphics", "intelgraphics"};
  auto const normalized = normalized_gpu_model_name(value);
  return std::ranges::find(kGenericGpuModelNames, normalized) !=
         kGenericGpuModelNames.end();
}

[[nodiscard]] std::string normalized_cpu_model_name(std::string_view value) {
  return lower_ascii(strip_trademark_markers(trim_ascii(value)));
}

[[nodiscard]] WindowsGpuMemory const* reliable_gpu_memory(
    std::string_view wmi_name,
    std::span<WindowsGpuMemory const> dxgi_adapters) noexcept {
  auto const normalized_wmi_name = normalized_gpu_model_name(wmi_name);
  if (normalized_wmi_name.empty()) {
    return nullptr;
  }
  WindowsGpuMemory const* matched = nullptr;
  for (auto const& adapter : dxgi_adapters) {
    if (normalized_gpu_model_name(adapter.model_name) != normalized_wmi_name) {
      continue;
    }
    if (matched != nullptr &&
        (matched->dedicated_video_memory != adapter.dedicated_video_memory ||
         matched->shared_system_memory != adapter.shared_system_memory)) {
      return nullptr;
    }
    matched = &adapter;
  }
  return matched;
}

struct PciDeviceId final {
  std::string vendor;
  std::string device;
  std::string subsystem;
};

[[nodiscard]] std::optional<PciDeviceId> pci_device_id(
    std::string_view pnp_id) {
  auto const lowered = lower_ascii(pnp_id);
  auto const vendor_marker = lowered.find("ven_");
  auto const device_marker = lowered.find("dev_");
  if (vendor_marker == std::string::npos || device_marker == std::string::npos ||
      vendor_marker + 8 > lowered.size() || device_marker + 8 > lowered.size()) {
    return std::nullopt;
  }
  auto vendor = lowered.substr(vendor_marker + 4, 4);
  auto device = lowered.substr(device_marker + 4, 4);
  if (!std::ranges::all_of(vendor, [](unsigned char value) {
        return std::isxdigit(value) != 0;
      }) ||
      !std::ranges::all_of(device, [](unsigned char value) {
        return std::isxdigit(value) != 0;
      })) {
    return std::nullopt;
  }
  std::string subsystem;
  auto const subsystem_marker = lowered.find("subsys_");
  if (subsystem_marker != std::string::npos &&
      subsystem_marker + 15 <= lowered.size()) {
    auto const candidate = lowered.substr(subsystem_marker + 7, 8);
    if (std::ranges::all_of(candidate, [](unsigned char value) {
          return std::isxdigit(value) != 0;
        })) {
      subsystem = candidate;
    }
  }
  return PciDeviceId{std::string{vendor}, std::string{device},
                     std::move(subsystem)};
}

struct PciGpuIdentity final {
  std::string_view vendor;
  std::string_view device;
  std::string_view model;
  application::HardwareGpuType gpu_type;
  application::HardwareGpuComputeUnit compute_unit;
  std::uint32_t compute_unit_count;
  // These optional constraints make a device-id entry board- and CPU-specific.
  // A bare device ID must never inherit this presentation fact.
  std::string_view required_subsystem{};
  std::string_view required_cpu_model{};
};

// These entries are conservative driver/PCI identities. Unknown device IDs
// intentionally remain unknown instead of borrowing a neighbouring model.
constexpr std::array<PciGpuIdentity, 8> kPciGpuIdentities{
    PciGpuIdentity{"8086", "3e91", "Intel UHD Graphics 630",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::eu, 24},
    PciGpuIdentity{"8086", "3e92", "Intel UHD Graphics 630",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::eu, 24},
    PciGpuIdentity{"8086", "3e9b", "Intel UHD Graphics 630",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::eu, 24},
    PciGpuIdentity{"8086", "7d55", "Intel Arc Graphics",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::xe, 8},
    PciGpuIdentity{"8086", "7d51", "Intel Arc Graphics",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::unknown, 0},
    PciGpuIdentity{"8086", "64a0", "Intel Arc Graphics",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::xe, 8},
    // Intel documents this exact CPU and HP subsystem combination as the
    // Core Ultra 9 275HX integrated GPU with four Xe cores. Do not generalize
    // the 7d67 device ID.
    PciGpuIdentity{"8086", "7d67", "Intel Core Ultra 9 275HX 集成显卡",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::xe, 4,
                   "8d41103c", "intel core ultra 9 275hx"},
    PciGpuIdentity{"1002", "164e", "AMD Radeon 780M Graphics",
                   application::HardwareGpuType::integrated,
                   application::HardwareGpuComputeUnit::cu, 12},
};

[[nodiscard]] PciGpuIdentity const* pci_gpu_identity(
    std::optional<PciDeviceId> const& id,
    std::span<std::string const> cpu_models) noexcept {
  if (!id.has_value()) {
    return nullptr;
  }
  for (auto const& identity : kPciGpuIdentities) {
    if (identity.vendor != id->vendor || identity.device != id->device) {
      continue;
    }
    if (!identity.required_subsystem.empty() &&
        identity.required_subsystem != id->subsystem) {
      continue;
    }
    if (!identity.required_cpu_model.empty() &&
        std::ranges::find(cpu_models, identity.required_cpu_model) ==
            cpu_models.end()) {
      continue;
    }
    return &identity;
  }
  return nullptr;
}

struct GpuPresentationFacts final {
  std::string model;
  application::HardwareGpuType gpu_type{application::HardwareGpuType::unknown};
  application::HardwareGpuComputeUnit compute_unit{
      application::HardwareGpuComputeUnit::unknown};
  std::uint32_t compute_unit_count{0};
};

[[nodiscard]] GpuPresentationFacts gpu_presentation_facts(
    std::string_view name, std::string_view compatibility,
    std::string_view video_processor, std::string_view pnp_id,
    std::span<std::string const> cpu_models, WindowsGpuMemory const* memory) {
  auto const pci_id = pci_device_id(pnp_id);
  auto const identity = pci_gpu_identity(pci_id, cpu_models);
  auto const normalized_name = strip_trademark_markers(trim_ascii(name));
  auto const normalized_video_processor =
      strip_trademark_markers(trim_ascii(video_processor));
  auto model_source = normalized_name;
  auto const generic_name = generic_gpu_model_name(normalized_name);
  if (generic_name && !normalized_video_processor.empty() &&
      !contains_ascii(normalized_video_processor, "family")) {
    model_source = normalized_video_processor;
  }

  GpuPresentationFacts facts{
      .model = identity == nullptr || !generic_name
                   ? localized_brand_model(model_source, compatibility)
                   : localized_brand_model(identity->model, compatibility),
      .gpu_type = memory == nullptr ? application::HardwareGpuType::unknown
                                     : memory->gpu_type,
      .compute_unit = memory == nullptr
                          ? application::HardwareGpuComputeUnit::unknown
                          : memory->compute_unit,
      .compute_unit_count = memory == nullptr ? 0 : memory->compute_unit_count,
  };
  if (identity != nullptr) {
    if (facts.gpu_type == application::HardwareGpuType::unknown) {
      facts.gpu_type = identity->gpu_type;
    }
    if (facts.compute_unit == application::HardwareGpuComputeUnit::unknown) {
      facts.compute_unit = identity->compute_unit;
      facts.compute_unit_count = identity->compute_unit_count;
    }
    if (generic_name) {
      facts.model = localized_brand_model(identity->model, compatibility);
    }
  }

  // Intel's WMI name is currently generic on some OEM systems. The installed
  // adapter identity is still authoritative for the model family, so retain
  // an ID-resolved Arc/UHD name even when DXGI memory is unavailable.
  if (identity != nullptr && contains_ascii(identity->model, "arc") &&
      contains_ascii(normalized_name, "intel graphics")) {
    facts.model = localized_brand_model(identity->model, compatibility);
  }

  auto const lowered_model = lower_ascii(model_source);
  if (facts.gpu_type == application::HardwareGpuType::unknown) {
    if (contains_ascii(lowered_model, "nvidia") ||
        contains_ascii(lowered_model, "geforce") ||
        contains_ascii(lowered_model, "radeon rx")) {
      facts.gpu_type = application::HardwareGpuType::discrete;
    } else if (pci_id.has_value() && pci_id->vendor == "10de") {
      // NVIDIA's PCI vendor is authoritative for discrete adapters even when
      // a firmware/WMI name is generic or localized.
      facts.gpu_type = application::HardwareGpuType::discrete;
    } else if (contains_ascii(lowered_model, "uhd") ||
               contains_ascii(lowered_model, "iris") ||
               contains_ascii(lowered_model, "radeon 890m") ||
               contains_ascii(lowered_model, "radeon 880m") ||
               contains_ascii(lowered_model, "radeon 860m") ||
               contains_ascii(lowered_model, "radeon 840m") ||
               contains_ascii(lowered_model, "radeon 780m") ||
               contains_ascii(lowered_model, "radeon 760m") ||
               contains_ascii(lowered_model, "radeon 740m") ||
               contains_ascii(lowered_model, "radeon 680m") ||
               contains_ascii(lowered_model, "radeon 660m") ||
               contains_ascii(lowered_model, "radeon vega 8") ||
               contains_ascii(lowered_model, "radeon vega 7") ||
               contains_ascii(lowered_model, "radeon vega 6") ||
               contains_ascii(lowered_model, "radeon vega 3")) {
      facts.gpu_type = application::HardwareGpuType::integrated;
    }
  }

  if (facts.compute_unit == application::HardwareGpuComputeUnit::unknown) {
    struct ModelUnit final {
      std::string_view marker;
      application::HardwareGpuComputeUnit unit;
      std::uint32_t count;
    };
    constexpr std::array<ModelUnit, 20> kModelUnits{
        ModelUnit{"uhd graphics 620", application::HardwareGpuComputeUnit::eu,
                  24},
        ModelUnit{"uhd 620", application::HardwareGpuComputeUnit::eu, 24},
        ModelUnit{"uhd graphics 630", application::HardwareGpuComputeUnit::eu,
                  24},
        ModelUnit{"uhd 630", application::HardwareGpuComputeUnit::eu, 24},
        ModelUnit{"uhd graphics 730", application::HardwareGpuComputeUnit::eu,
                  24},
        ModelUnit{"uhd graphics 750", application::HardwareGpuComputeUnit::eu,
                  32},
        ModelUnit{"uhd graphics 770", application::HardwareGpuComputeUnit::eu,
                  32},
        ModelUnit{"radeon 890m", application::HardwareGpuComputeUnit::cu, 16},
        ModelUnit{"radeon 880m", application::HardwareGpuComputeUnit::cu, 12},
        ModelUnit{"radeon 860m", application::HardwareGpuComputeUnit::cu, 8},
        ModelUnit{"radeon 840m", application::HardwareGpuComputeUnit::cu, 4},
        ModelUnit{"radeon 780m", application::HardwareGpuComputeUnit::cu, 12},
        ModelUnit{"radeon 760m", application::HardwareGpuComputeUnit::cu, 8},
        ModelUnit{"radeon 740m", application::HardwareGpuComputeUnit::cu, 4},
        ModelUnit{"radeon 680m", application::HardwareGpuComputeUnit::cu, 12},
        ModelUnit{"radeon 660m", application::HardwareGpuComputeUnit::cu, 6},
        ModelUnit{"radeon vega 8", application::HardwareGpuComputeUnit::cu, 8},
        ModelUnit{"radeon vega 7", application::HardwareGpuComputeUnit::cu, 7},
        ModelUnit{"radeon vega 6", application::HardwareGpuComputeUnit::cu, 6},
        ModelUnit{"radeon vega 3", application::HardwareGpuComputeUnit::cu, 3},
    };
    for (auto const& candidate : kModelUnits) {
      if (contains_ascii(model_source, candidate.marker)) {
        facts.compute_unit = candidate.unit;
        facts.compute_unit_count = candidate.count;
        break;
      }
    }
  }
  return facts;
}

[[nodiscard]] std::string gpu_memory_description(
    WindowsGpuMemory const* memory, GpuPresentationFacts const& facts) {
  std::string result;
  if (facts.gpu_type == application::HardwareGpuType::integrated) {
    result = "核显；";
  } else if (facts.gpu_type == application::HardwareGpuType::discrete) {
    result = "独立显卡；";
  }
  auto const memory_known =
      memory != nullptr &&
      (memory->dedicated_video_memory != 0 || memory->shared_system_memory != 0);
  if (!memory_known) {
    result += "显存：未读取";
  } else {
    if (memory->dedicated_video_memory != 0) {
      result += "专用显存 ";
      result += gpu_memory_size(memory->dedicated_video_memory, true);
    } else {
      result += "专用显存 未读取";
    }
    if (facts.gpu_type == application::HardwareGpuType::integrated &&
        memory->shared_system_memory != 0) {
      result += "；共享内存 ";
      result += gpu_memory_size(memory->shared_system_memory, false);
    }
  }
  // Compute-unit terminology is meaningful for an integrated GPU only. A
  // discrete adapter must expose only dedicated VRAM and reliable board
  // identity; shared-memory or CU/EU/Xe text would be misleading there.
  if (facts.gpu_type == application::HardwareGpuType::integrated) {
    result += "；";
    if (facts.compute_unit != application::HardwareGpuComputeUnit::unknown &&
        facts.compute_unit_count != 0) {
      result += std::to_string(facts.compute_unit_count);
      switch (facts.compute_unit) {
        case application::HardwareGpuComputeUnit::cu: result += " CU"; break;
        case application::HardwareGpuComputeUnit::eu: result += " EU"; break;
        case application::HardwareGpuComputeUnit::xe: result += " Xe 核"; break;
        case application::HardwareGpuComputeUnit::unknown: break;
      }
    } else {
      result += "计算单元未读取";
    }
  }
  return result;
}

struct PciSubsystemVendor final {
  std::string_view id;
  std::string_view display;
};

constexpr std::array<PciSubsystemVendor, 12> kPciSubsystemVendors{
    PciSubsystemVendor{"1025", "宏碁 Acer"},
    PciSubsystemVendor{"1028", "戴尔 Dell"},
    PciSubsystemVendor{"103c", "惠普 HP"},
    PciSubsystemVendor{"1043", "华硕 ASUS"},
    PciSubsystemVendor{"1458", "技嘉 GIGABYTE"},
    PciSubsystemVendor{"1462", "微星 MSI"},
    PciSubsystemVendor{"1569", "柏能 PALIT"},
    PciSubsystemVendor{"174b", "蓝宝石 SAPPHIRE"},
    PciSubsystemVendor{"19da", "索泰 ZOTAC"},
    PciSubsystemVendor{"1b4c", "影驰 GALAX"},
    PciSubsystemVendor{"7377", "七彩虹 COLORFUL"},
    PciSubsystemVendor{"10b0", "耕升 GAINWARD"},
};

[[nodiscard]] std::optional<std::string_view> pci_subsystem_vendor_display(
    std::string_view pnp_id) noexcept {
  auto const lowered = lower_ascii(pnp_id);
  auto const marker = lowered.find("subsys_");
  if (marker == std::string::npos || marker + 15 > lowered.size()) {
    return std::nullopt;
  }
  auto const vendor_id = lowered.substr(marker + 7, 8).substr(4, 4);
  if (!std::ranges::all_of(vendor_id, [](unsigned char value) {
        return std::isxdigit(value) != 0;
      })) {
    return std::nullopt;
  }
  for (auto const& vendor : kPciSubsystemVendors) {
    if (vendor.id == vendor_id) {
      return vendor.display;
    }
  }
  return std::nullopt;
}

[[nodiscard]] application::HardwareVendor vendor_from_text(
    std::string_view value) noexcept {
  auto const lowered = lower_ascii(value);
  if (lowered.find("amd") != std::string::npos ||
      lowered.find("advanced micro") != std::string::npos) {
    return application::HardwareVendor::amd;
  }
  if (lowered.find("intel") != std::string::npos) {
    return application::HardwareVendor::intel;
  }
  if (lowered.find("nvidia") != std::string::npos) {
    return application::HardwareVendor::nvidia;
  }
  if (lowered.find("dell") != std::string::npos) {
    return application::HardwareVendor::dell;
  }
  if (lowered.find("hewlett") != std::string::npos ||
      lowered.find(" hp") != std::string::npos || lowered == "hp") {
    return application::HardwareVendor::hp;
  }
  if (lowered.find("lenovo") != std::string::npos) {
    return application::HardwareVendor::lenovo;
  }
  if (lowered.find("asus") != std::string::npos) {
    return application::HardwareVendor::asus;
  }
  return application::HardwareVendor::unknown;
}

[[nodiscard]] bool virtualization_marker(std::string_view value) noexcept {
  auto const lowered = lower_ascii(value);
  constexpr std::array<std::string_view, 16> markers{
      "hyper-v", "hyperv", "vmware", "virtualbox", "qemu", "kvm",
      "xen", "parallels", "bochs", "virtual machine", "hvm dom", "bhyve",
      "remote display", "rdp", "indirect display", "miracast",
  };
  for (auto const marker : markers) {
    if (lowered.find(marker) != std::string::npos) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool virtual_pnp_id(std::string_view value) noexcept {
  auto const lowered = lower_ascii(value);
  constexpr std::array<std::string_view, 8> prefixes{
      "root\\", "swd\\", "vmbus\\", "htree\\", "virtual\\", "vpn\\",
      "tap\\", "tun\\",
  };
  for (auto const prefix : prefixes) {
    if (starts_with_ascii(lowered, prefix)) {
      return true;
    }
  }
  constexpr std::array<std::string_view, 6> virtual_pci_ids{
      "pci\\ven_15ad",  // VMware
      "pci\\ven_80ee",  // VirtualBox
      "pci\\ven_1af4",  // virtio/QEMU
      "pci\\ven_1414",  // Hyper-V
      "pci\\ven_5853",  // Xen
      "pci\\ven_1ab8",  // Parallels
  };
  for (auto const id : virtual_pci_ids) {
    if (starts_with_ascii(lowered, id)) {
      return true;
    }
  }
  return virtualization_marker(lowered);
}

[[nodiscard]] bool physical_bus(std::string_view value,
                                 bool network) noexcept {
  auto const lowered = lower_ascii(value);
  if (network) {
    constexpr std::array<std::string_view, 4> buses{
        "pci\\", "usb\\", "sd\\", "acpi\\",
    };
    return std::ranges::any_of(buses, [&](auto const bus) {
      return starts_with_ascii(lowered, bus);
    });
  }
  constexpr std::array<std::string_view, 8> buses{
      "pci\\", "usb\\", "sd\\", "acpi\\", "hdaudio\\", "scsi\\",
      "sas\\", "ide\\",
  };
  return std::ranges::any_of(buses, [&](auto const bus) {
    return starts_with_ascii(lowered, bus);
  });
}

[[nodiscard]] bool physical_storage_bus(std::string_view value) noexcept {
  auto const lowered = lower_ascii(value);
  constexpr std::array<std::string_view, 8> buses{
      "pci\\", "usb\\", "sd\\", "acpi\\", "scsi\\", "sas\\", "ide\\",
      "ata\\",
  };
  return std::ranges::any_of(buses, [&](auto const bus) {
    return starts_with_ascii(lowered, bus);
  });
}

[[nodiscard]] application::HardwareDeviceStatus device_status(
    std::string_view status_text, std::string_view error_code) noexcept {
  std::uint32_t code{};
  if (parse_integer(error_code, code)) {
    if (code == 22) {
      return application::HardwareDeviceStatus::disabled;
    }
    if (code == 28) {
      return application::HardwareDeviceStatus::no_driver;
    }
    if (code != 0) {
      return application::HardwareDeviceStatus::error;
    }
  }
  auto const status = lower_ascii(status_text);
  if (status == "disabled" || status == "degraded") {
    return application::HardwareDeviceStatus::disabled;
  }
  if (status == "error" || status == "failed" || status == "critical") {
    return application::HardwareDeviceStatus::error;
  }
  if (status == "ok" || status == "enabled" || status == "running" ||
      status == "online") {
    return application::HardwareDeviceStatus::enabled;
  }
  return application::HardwareDeviceStatus::unknown;
}

[[nodiscard]] bool obvious_nonphysical_network(std::string_view name,
                                                std::string_view service,
                                                std::string_view adapter_type) noexcept {
  constexpr std::array<std::string_view, 12> markers{
      "vpn", "tunnel", "loopback", "virtual", "hyper-v", "hyperv",
      "vmware", "virtualbox", "tap", "tun", "teredo", "wireguard",
  };
  for (auto const marker : markers) {
    if (contains_ascii(name, marker) || contains_ascii(service, marker) ||
        contains_ascii(adapter_type, marker)) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_cpu(
    std::vector<std::string> const& row, bool virtual_host,
    std::optional<WindowsCpuTopology> const& topology) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id)) {
    return std::nullopt;
  }
  // Some OEM firmware leaves PNPDeviceID blank for Win32_Processor. The
  // class itself is a physical processor inventory and the non-virtual host
  // gate remains mandatory in that fallback case.
  if (!pnp_id.empty() && !starts_with_ascii(pnp_id, "acpi\\") &&
      !starts_with_ascii(pnp_id, "processor\\")) {
    return std::nullopt;
  }
  auto display_name = localized_brand_model(name, manufacturer);
  std::uint32_t cores{};
  std::uint32_t threads{};
  auto const cores_valid = parse_integer(row_value(row, 5), cores) && cores > 0;
  auto const threads_valid =
      parse_integer(row_value(row, 6), threads) && threads > 0;
  auto topology_cores = std::uint32_t{0};
  auto topology_reliable = false;
  if (topology.has_value() && topology->split_known) {
    topology_cores = topology->performance_cores +
                     topology->efficiency_cores +
                     topology->low_power_efficiency_cores;
    // Use a split only when its totals agree with the independently reported
    // WMI counts. An unreliable split leaves every class unknown instead of
    // guessing any numeric class count.
    topology_reliable = topology_cores != 0 &&
                        topology->logical_processors != 0 &&
                        (!cores_valid || cores == topology_cores) &&
                        (!threads_valid ||
                         threads == topology->logical_processors);
  }
  auto const reported_cores = cores_valid ? cores : topology_cores;
  auto const reported_threads =
      threads_valid ? threads
                    : (topology.has_value() ? topology->logical_processors : 0);
  display_name += "（";
  if (reported_cores != 0) {
    display_name += std::to_string(reported_cores);
    display_name += "核心";
  } else {
    display_name += "核心未读取";
  }
  display_name += reported_threads != 0 ? std::to_string(reported_threads)
                                        : "线程未读取";
  if (reported_threads != 0) {
    display_name += "线程";
  }
  display_name += "；";
  if (topology_reliable) {
    display_name += std::to_string(topology->performance_cores);
    display_name += "性能核+";
    display_name += std::to_string(topology->efficiency_cores);
    display_name += "能效核+";
    display_name += std::to_string(topology->low_power_efficiency_cores);
    display_name += "低功耗能效核";
  } else {
    display_name += "性能核未识别+能效核未识别+低功耗能效核未识别";
  }
  display_name += "）";
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::cpu,
      .name = std::move(display_name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = pnp_id.empty()
                           ? "Win32_Processor on a non-virtual host (PNP id absent)"
                           : "ACPI/processor PNP id on a non-virtual host",
      .core_count = reported_cores,
      .thread_count = reported_threads,
      .performance_core_count = topology_reliable
                                    ? topology->performance_cores
                                    : 0,
      .efficiency_core_count = topology_reliable
                                   ? topology->efficiency_cores
                                   : 0,
      .low_power_efficiency_core_count = topology_reliable
                                             ? topology->low_power_efficiency_cores
                                             : 0,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_gpu(
    std::vector<std::string> const& row, bool virtual_host,
    std::span<WindowsGpuMemory const> dxgi_adapters,
    std::span<std::string const> cpu_models) {
  auto const name = row_value(row, 0);
  auto const compatibility = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !physical_bus(pnp_id, false)) {
    return std::nullopt;
  }
  auto const video_processor = row_value(row, 5);
  auto const* memory = reliable_gpu_memory(name, dxgi_adapters);
  auto facts = gpu_presentation_facts(name, compatibility, video_processor,
                                      pnp_id, cpu_models, memory);
  auto display_name = facts.model;
  if (facts.gpu_type != application::HardwareGpuType::integrated) {
    if (auto const board_vendor = pci_subsystem_vendor_display(pnp_id);
        board_vendor.has_value() && display_name.find(*board_vendor) ==
                                        std::string::npos) {
    display_name = std::string{*board_vendor} + " " + display_name;
    }
  }
  display_name += "（";
  display_name += gpu_memory_description(memory, facts);
  display_name += "）";
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::gpu,
      .name = std::move(display_name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(compatibility.empty() ? name : compatibility),
      .physically_present = true,
      .filter_reason = "PCI PNP id on a non-virtual host",
      .capacity_bytes = memory == nullptr ? 0 : memory->dedicated_video_memory,
      .gpu_type = facts.gpu_type,
      .gpu_compute_unit = facts.compute_unit,
      .gpu_compute_unit_count = facts.compute_unit_count,
      .gpu_shared_memory_bytes =
          facts.gpu_type == application::HardwareGpuType::integrated &&
                  memory != nullptr
              ? memory->shared_system_memory
              : 0,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_board(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const manufacturer = row_value(row, 0);
  auto const product = row_value(row, 1);
  auto const hosting_text = row_value(row, 2);
  bool hosting_board = false;
  auto const hosting_known = parse_bool(hosting_text, hosting_board);
  if (manufacturer.empty() || product.empty() || virtual_host ||
      !hosting_known || !hosting_board) {
    return std::nullopt;
  }
  auto display_name = localized_brand_model(product, manufacturer);
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::motherboard,
      .name = std::move(display_name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), {}),
      .vendor = vendor_from_text(manufacturer),
      .physically_present = true,
      .filter_reason =
          "Win32_BaseBoard concrete manufacturer/product with HostingBoard=true",
  };
}

[[nodiscard]] application::HardwareNetworkLink network_link_from_text(
    std::string_view name, std::string_view adapter_type) noexcept {
  if (contains_ascii(name, "wi-fi") || contains_ascii(name, "wifi") ||
      contains_ascii(name, "wireless") || contains_ascii(name, "802.11") ||
      contains_ascii(adapter_type, "wireless") ||
      contains_ascii(adapter_type, "802.11")) {
    return application::HardwareNetworkLink::wireless;
  }
  if (contains_ascii(name, "ethernet") || contains_ascii(name, "gigabit") ||
      contains_ascii(name, "2.5gbe") || contains_ascii(name, "5gbe") ||
      contains_ascii(name, "10gbe") || contains_ascii(adapter_type, "ethernet") ||
      contains_ascii(adapter_type, "802.3")) {
    return application::HardwareNetworkLink::wired;
  }
  return application::HardwareNetworkLink::unknown;
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_network(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const adapter_type = row_value(row, 2);
  auto const physical_adapter = row_value(row, 3);
  auto const pnp_id = row_value(row, 4);
  auto const service = row_value(row, 7);
  auto const link = network_link_from_text(name, adapter_type);
  bool is_physical = false;
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !parse_bool(physical_adapter, is_physical) || !is_physical ||
      !physical_bus(pnp_id, true) ||
      obvious_nonphysical_network(name, service, adapter_type)) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::network_adapter,
      .name = localized_brand_model(name, manufacturer),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 5), row_value(row, 6)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "PhysicalAdapter=true with PCI/USB/SD/ACPI PNP id",
      .network_link = link,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_memory(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const manufacturer = row_value(row, 0);
  auto const capacity_text = row_value(row, 1);
  auto const speed_text = row_value(row, 2);
  auto const part_number = trim_ascii(row_value(row, 3));
  auto const locator = row_value(row, 4);
  auto const tag = row_value(row, 5);
  auto const memory_type = row_value(row, 6);
  auto const configured_speed = row_value(row, 7);
  std::uint64_t capacity{};
  std::uint32_t speed{};
  std::uint32_t configured{};
  if (virtual_host || manufacturer.empty() || locator.empty() ||
      !parse_unsigned(capacity_text, capacity) || capacity == 0 ||
      !starts_with_ascii(tag, "physical memory")) {
    return std::nullopt;
  }
  if (!parse_integer(configured_speed, configured) || configured == 0) {
    static_cast<void>(parse_integer(speed_text, speed));
  } else {
    speed = configured;
  }

  std::string name = localized_memory_model(manufacturer, part_number);
  if (memory_type == "24") {
    name += " DDR3";
  } else if (memory_type == "26") {
    name += " DDR4";
  } else if (memory_type == "34") {
    name += " DDR5";
  }
  name += " ";
  name += decimal_gigabytes(capacity);
  if (speed != 0) {
    name += " ";
    name += std::to_string(speed);
    name += "MHz";
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::memory,
      .name = std::move(name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = application::HardwareDeviceStatus::enabled,
      .vendor = vendor_from_text(manufacturer),
      .physically_present = true,
      .filter_reason = "Win32_PhysicalMemory module with a positive capacity",
      .model_detail = part_number,
      .capacity_bytes = capacity,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_display(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const pnp_entity_row = row.size() >= 6 &&
                              !starts_with_ascii(row_value(row, 1), "display\\");
  auto const manufacturer = pnp_entity_row ? row_value(row, 1) : std::string{};
  auto const pnp_id = row_value(row, pnp_entity_row ? 2 : 1);
  auto const pnp_class = row_value(row, pnp_entity_row ? 5 : 0);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !starts_with_ascii(pnp_id, "display\\") ||
      (pnp_entity_row && !contains_ascii(pnp_class, "monitor")) ||
      contains_ascii(name, "remote display") ||
      contains_ascii(name, "virtual")) {
    return std::nullopt;
  }
  auto display_name = presented_display_name(name, manufacturer, pnp_id);
  if (display_name.empty() || generic_display_name(display_name)) {
    return std::nullopt;
  }
  std::uint32_t width{};
  std::uint32_t height{};
  if (!pnp_entity_row) {
    auto const width_valid = parse_integer(row_value(row, 4), width) && width > 0;
    auto const height_valid =
        parse_integer(row_value(row, 5), height) && height > 0;
    if (!width_valid || !height_valid) {
      width = 0;
      height = 0;
    }
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::display,
      .name = std::move(display_name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, pnp_entity_row ? 3 : 2),
                              row_value(row, pnp_entity_row ? 4 : 3)),
      .vendor = vendor_from_text(name),
      .physically_present = true,
      .filter_reason = "DISPLAY PNP id on a non-virtual host",
      .model_detail = display_model_key(pnp_id),
      .display_width = width,
      .display_height = height,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord>
classify_display_pnp(std::vector<std::string> const& row,
                     bool virtual_host) {
  // Win32_PnPEntity is the useful source for EDID-derived model names on
  // laptops; Win32_DesktopMonitor often reports only "Generic PnP Monitor".
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  auto const pnp_class = row_value(row, 5);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !starts_with_ascii(pnp_id, "display\\") ||
      !contains_ascii(pnp_class, "monitor") ||
      contains_ascii(name, "remote display") ||
      contains_ascii(name, "virtual")) {
    return std::nullopt;
  }
  auto const display_name = presented_display_name(name, manufacturer, pnp_id);
  if (display_name.empty() || generic_display_name(display_name)) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::display,
      .name = display_name,
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "Win32_PnPEntity monitor with DISPLAY PNP id",
      .model_detail = display_model_key(pnp_id),
  };
}

[[nodiscard]] application::HardwareStorageMedia storage_media_from_text(
    std::string_view model, std::string_view manufacturer,
    std::string_view interface_type, std::string_view media_type,
    std::string_view pnp_id) noexcept {
  if (contains_ascii(model, "nvme") || contains_ascii(model, "ssd") ||
      contains_ascii(model, "solid state") || contains_ascii(model, "flash") ||
      contains_ascii(interface_type, "nvme") ||
      contains_ascii(interface_type, "solid state") ||
      contains_ascii(pnp_id, "nvme")) {
    return application::HardwareStorageMedia::solid_state;
  }
  if (contains_ascii(model, "hdd") || contains_ascii(model, "hard disk") ||
      contains_ascii(interface_type, "ide") || contains_ascii(interface_type, "ata") ||
      contains_ascii(media_type, "hard disk") ||
      contains_ascii(manufacturer, "seagate") || contains_ascii(manufacturer, "wd")) {
    return application::HardwareStorageMedia::hard_disk;
  }
  return application::HardwareStorageMedia::unknown;
}

[[nodiscard]] std::string windows_release_name(std::string_view build) {
  std::uint32_t build_number{};
  if (!parse_integer(build, build_number)) {
    return {};
  }
  if (build_number >= 26200 && build_number < 27000) {
    return "25H2";
  }
  if (build_number >= 26100 && build_number < 26200) {
    return "24H2";
  }
  if (build_number >= 22631 && build_number < 26100) {
    return "23H2";
  }
  if (build_number >= 22621 && build_number < 22631) {
    return "22H2";
  }
  if (build_number >= 22000 && build_number < 22621) {
    return "21H2";
  }
  return {};
}

[[nodiscard]] std::string localized_windows_caption(std::string_view caption) {
  if (contains_ascii(caption, "windows 11 pro")) {
    return "Windows 11 专业版（Windows 11 Pro）";
  }
  if (contains_ascii(caption, "windows 11 home")) {
    return "Windows 11 家庭版（Windows 11 Home）";
  }
  if (contains_ascii(caption, "windows 10 pro")) {
    return "Windows 10 专业版（Windows 10 Pro）";
  }
  if (contains_ascii(caption, "windows 10 home")) {
    return "Windows 10 家庭版（Windows 10 Home）";
  }
  return trim_ascii(caption);
}

[[nodiscard]] std::string localized_architecture(std::string_view value) {
  if (contains_ascii(value, "64")) {
    return "64 位";
  }
  if (contains_ascii(value, "32")) {
    return "32 位";
  }
  return trim_ascii(value);
}

[[nodiscard]] std::string presented_operating_system(
    std::string_view caption, std::string_view version,
    std::string_view build, std::string_view architecture) {
  auto result = localized_windows_caption(caption);
  if (result.empty()) {
    return {};
  }
  auto const release = windows_release_name(build);
  if (!release.empty()) {
    result += " · ";
    result += release;
  }
  if (!version.empty()) {
    result += " · 版本 ";
    result += version;
  }
  if (!build.empty()) {
    result += "（内部版本 ";
    result += build;
    result += "）";
  }
  auto const localized_arch = localized_architecture(architecture);
  if (!localized_arch.empty()) {
    result += " · ";
    result += localized_arch;
  }
  return result;
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_storage(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const model = strip_storage_class_prefix(row_value(row, 0));
  auto const manufacturer = row_value(row, 1);
  auto const size_text = row_value(row, 2);
  auto const pnp_id = row_value(row, 3);
  auto const interface_type = row_value(row, 6);
  auto const media_type = row_value(row, 7);
  std::uint64_t size{};
  if (model.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !parse_unsigned(size_text, size) || size == 0 ||
      !physical_storage_bus(pnp_id)) {
    return std::nullopt;
  }
  auto name = strip_capacity_tokens(
      localized_brand_model(model, manufacturer));
  if (name.empty()) {
    name = localized_brand_model(manufacturer, manufacturer);
  }
  name += " ";
  name += storage_capacity_text(size);
  auto const media = storage_media_from_text(model, manufacturer, interface_type,
                                             media_type, pnp_id);
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::storage,
      .name = std::move(name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 4), row_value(row, 5)),
      .vendor = vendor_from_text(manufacturer.empty() ? model : manufacturer),
      .physically_present = true,
      .filter_reason = "physical storage PNP id with a positive capacity",
      .capacity_bytes = size,
      .storage_media = media,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_audio(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  auto const pnp_entity_row = row.size() >= 6;
  auto const pnp_class = row_value(row, pnp_entity_row ? 5 : 0);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !physical_bus(pnp_id, false) ||
      (pnp_entity_row && !contains_ascii(pnp_class, "media")) ||
      contains_ascii(name, "virtual") || contains_ascii(name, "remote")) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::audio,
      .name = localized_brand_model(name, manufacturer),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, pnp_entity_row ? 3 : 3),
                              row_value(row, pnp_entity_row ? 4 : 4)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "physical audio PNP id on a non-virtual host",
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_npu(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  auto const pnp_class = row_value(row, 5);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !physical_bus(pnp_id, false) ||
      !(contains_ascii(pnp_class, "npu") ||
        contains_ascii(pnp_class, "compute") ||
        contains_ascii(pnp_class, "system")) ||
      !(contains_ascii(name, "npu") || contains_ascii(name, "ai boost") ||
        contains_ascii(name, "neural") ||
        contains_ascii(name, "compute accelerator"))) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::npu,
      .name = localized_brand_model(name, manufacturer),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "physical compute-device PNP id with an NPU marker",
  };
}

enum class KeyboardPresentationType {
  unknown,
  magic,
  butterfly,
};

[[nodiscard]] bool internal_keyboard_description(
    std::string_view description) {
  return contains_ascii(description, "internal") ||
         contains_ascii(description, "built-in") ||
         contains_ascii(description, "built in") ||
         description.find("内建") != std::string_view::npos ||
         description.find("内置") != std::string_view::npos;
}

[[nodiscard]] bool generic_keyboard_description(
    std::string_view description) {
  constexpr std::array<std::string_view, 5> kGenericDescriptions{
      "hid keyboard device", "generic", "standard ps/2 keyboard",
      "standard 101/102-key", "standard keyboard",
  };
  return std::ranges::any_of(kGenericDescriptions, [&](auto const marker) {
    return contains_ascii(description, marker);
  });
}

[[nodiscard]] KeyboardPresentationType keyboard_presentation_type(
    std::string_view description) {
  if (generic_keyboard_description(description)) {
    return KeyboardPresentationType::unknown;
  }
  if (contains_ascii(description, "magic keyboard")) {
    return KeyboardPresentationType::magic;
  }
  if (contains_ascii(description, "butterfly keyboard")) {
    return KeyboardPresentationType::butterfly;
  }
  return KeyboardPresentationType::unknown;
}

[[nodiscard]] std::string_view keyboard_presentation_name(
    KeyboardPresentationType value) {
  switch (value) {
    case KeyboardPresentationType::magic: return "妙控键盘";
    case KeyboardPresentationType::butterfly: return "蝶式键盘";
    case KeyboardPresentationType::unknown: return {};
  }
  return {};
}

[[nodiscard]] bool input_device_is_virtual(
    std::string_view name, std::string_view pnp_id,
    std::span<std::string const> driver_descriptions) {
  auto const normalized_pnp_id = trim_ascii(pnp_id);
  return virtual_pnp_id(normalized_pnp_id) || contains_ascii(name, "virtual") ||
         contains_ascii(name, "remote") || contains_ascii(name, "rdp") ||
         std::ranges::any_of(
             driver_descriptions, [](std::string const& description) {
               return contains_ascii(description, "virtual") ||
                      contains_ascii(description, "remote") ||
                      contains_ascii(description, "rdp");
             });
}

[[nodiscard]] bool input_pnp_id_is_physical(std::string_view pnp_id) {
  auto const normalized_pnp_id = trim_ascii(pnp_id);
  if (normalized_pnp_id.empty() || virtual_pnp_id(normalized_pnp_id)) {
    return false;
  }
  constexpr std::array<std::string_view, 11> physical_prefixes{
      "acpi\\", "bth\\", "bthenum\\", "bthledevice\\", "bluetooth\\",
      "hid\\", "i2c\\", "spi\\", "usb\\", "sd\\", "swd\\",
  };
  return std::ranges::any_of(physical_prefixes, [&](auto const prefix) {
    return starts_with_ascii(normalized_pnp_id, prefix);
  });
}

[[nodiscard]] application::HardwareInputDeviceConnection input_connection_for(
    std::string_view name, std::string_view pnp_id,
    std::span<std::string const> descriptions) {
  auto const normalized_pnp_id = trim_ascii(pnp_id);
  auto internal = contains_ascii(name, "internal") ||
                  contains_ascii(name, "built-in") ||
                  contains_ascii(name, "built in") ||
                  name.find("内建") != std::string_view::npos ||
                  name.find("内置") != std::string_view::npos ||
                  std::ranges::any_of(
                      descriptions, [](std::string const& description) {
                        return internal_keyboard_description(description);
                      });
  auto external = contains_ascii(name, "external") ||
                  contains_ascii(name, "usb") ||
                  contains_ascii(name, "bluetooth") ||
                  contains_ascii(name, "wireless") ||
                  std::ranges::any_of(
                      descriptions, [](std::string const& description) {
                        return contains_ascii(description, "external") ||
                               contains_ascii(description, "usb") ||
                               contains_ascii(description, "bluetooth") ||
                               contains_ascii(description, "wireless");
                      });
  auto const acpi_or_i2c = starts_with_ascii(normalized_pnp_id, "acpi\\") ||
                           starts_with_ascii(normalized_pnp_id, "i2c\\") ||
                           starts_with_ascii(normalized_pnp_id, "spi\\");
  auto const usb_or_bluetooth = starts_with_ascii(normalized_pnp_id, "usb\\") ||
                                starts_with_ascii(normalized_pnp_id, "bth\\") ||
                                starts_with_ascii(normalized_pnp_id, "bthenum\\") ||
                                starts_with_ascii(normalized_pnp_id, "bthledevice\\") ||
                                starts_with_ascii(normalized_pnp_id, "bluetooth\\");
  internal = internal || acpi_or_i2c;
  external = external || usb_or_bluetooth;
  if (internal == external) {
    return application::HardwareInputDeviceConnection::unknown;
  }
  return internal ? application::HardwareInputDeviceConnection::internal
                  : application::HardwareInputDeviceConnection::external;
}

[[nodiscard]] std::optional<application::HardwareInputDeviceType>
input_device_type_for(std::string_view name, std::string_view pnp_class) {
  auto const is_touchpad =
      contains_ascii(name, "touchpad") || contains_ascii(name, "touch pad") ||
      contains_ascii(name, "precision touchpad") ||
      contains_ascii(name, "clickpad") || contains_ascii(name, "trackpad") ||
      contains_ascii(name, "track pad");
  if (is_touchpad && !contains_ascii(name, "touchscreen")) {
    return application::HardwareInputDeviceType::touchpad;
  }
  if (contains_ascii(pnp_class, "mouse") || contains_ascii(name, "mouse") ||
      contains_ascii(name, "pointing device") ||
      contains_ascii(name, "trackball")) {
    return application::HardwareInputDeviceType::mouse;
  }
  return std::nullopt;
}

[[nodiscard]] std::string input_device_category_name(
    application::HardwareInputDeviceType type,
    application::HardwareInputDeviceConnection connection) {
  std::string result;
  switch (connection) {
    case application::HardwareInputDeviceConnection::internal:
      result = "内建";
      break;
    case application::HardwareInputDeviceConnection::external:
      result = "外接";
      break;
    case application::HardwareInputDeviceConnection::unknown:
      break;
  }
  switch (type) {
    case application::HardwareInputDeviceType::keyboard:
      result += "键盘";
      break;
    case application::HardwareInputDeviceType::mouse:
      result += "鼠标";
      break;
    case application::HardwareInputDeviceType::touchpad:
      result += "触控板";
      break;
    case application::HardwareInputDeviceType::unknown:
      result += "输入设备";
      break;
  }
  return result;
}

void append_driver_reported_keyboard_key_counts(
    std::vector<std::uint32_t>& counts, std::string_view description) {
  auto const lowered = lower_ascii(description);
  constexpr std::string_view kChineseKey = "键";
  for (std::size_t index = 0; index < lowered.size();) {
    if (std::isdigit(static_cast<unsigned char>(lowered[index])) == 0) {
      ++index;
      continue;
    }
    auto const number_begin = index;
    while (index < lowered.size() &&
           std::isdigit(static_cast<unsigned char>(lowered[index])) != 0) {
      ++index;
    }
    std::uint32_t count = 0;
    if (!parse_integer(
            std::string_view{lowered}.substr(number_begin, index - number_begin),
            count) ||
        count == 0) {
      continue;
    }
    auto marker_begin = index;
    while (marker_begin < lowered.size() &&
           std::isspace(static_cast<unsigned char>(lowered[marker_begin])) != 0) {
      ++marker_begin;
    }
    if (marker_begin < lowered.size() && lowered[marker_begin] == '-') {
      ++marker_begin;
      while (marker_begin < lowered.size() &&
             std::isspace(static_cast<unsigned char>(lowered[marker_begin])) !=
                 0) {
        ++marker_begin;
      }
    }

    auto recognized = false;
    if (lowered.compare(marker_begin, 3, "key") == 0) {
      auto marker_end = marker_begin + 3;
      recognized = marker_end == lowered.size() ||
                   std::isalnum(static_cast<unsigned char>(lowered[marker_end])) ==
                       0;
    } else if (lowered.compare(marker_begin, kChineseKey.size(),
                               kChineseKey) == 0) {
      recognized = true;
    }
    if (recognized &&
        std::ranges::find(counts, count) == counts.end()) {
      counts.push_back(count);
    }
  }
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord>
classify_keyboard(std::vector<std::string> const& row,
                  std::string_view wmi_description,
                  std::span<std::string const> driver_descriptions,
                  bool virtual_host) {
  auto const pnp_id = row_value(row, 1);
  if (virtual_host || !input_pnp_id_is_physical(pnp_id) ||
      input_device_is_virtual(wmi_description, pnp_id,
                              driver_descriptions)) {
    return std::nullopt;
  }
  auto const connection = input_connection_for(
      wmi_description, pnp_id, driver_descriptions);
  if (connection == application::HardwareInputDeviceConnection::unknown) {
    return std::nullopt;
  }

  // Win32_Keyboard::Description may establish only that a keyboard is
  // internal. It is a generic WMI name, so type and key count must come from
  // an exact same-device driver description instead.
  auto type = KeyboardPresentationType::unknown;
  auto conflicting_type = false;
  std::vector<std::uint32_t> key_counts;
  for (auto const& description : driver_descriptions) {
    auto const candidate_type = keyboard_presentation_type(description);
    if (candidate_type == KeyboardPresentationType::unknown) {
      continue;
    }
    if (type == KeyboardPresentationType::unknown) {
      type = candidate_type;
    } else if (type != candidate_type) {
      conflicting_type = true;
    }
    append_driver_reported_keyboard_key_counts(key_counts, description);
  }

  auto name = input_device_category_name(
      application::HardwareInputDeviceType::keyboard, connection);
  if (connection == application::HardwareInputDeviceConnection::internal &&
      !conflicting_type && type != KeyboardPresentationType::unknown) {
    name += " · ";
    name += keyboard_presentation_name(type);
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::input_device,
      .name = std::move(name),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = !driver_descriptions.empty()
                    ? application::HardwareObservationSource::setup_api
                    : application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 2), row_value(row, 3)),
      .physically_present = true,
      .filter_reason =
          connection == application::HardwareInputDeviceConnection::internal
              ? "explicit same-device internal keyboard fact"
              : "physical keyboard PNP id with explicit external bus fact",
      .input_device_type = application::HardwareInputDeviceType::keyboard,
      .input_device_connection = connection,
      // An unknown type is rendered only as "内建键盘". A key count is shown
      // only when the same driver evidence confirms one unambiguous layout.
      .input_device_key_count =
          !conflicting_type && type != KeyboardPresentationType::unknown &&
              key_counts.size() == 1
              ? key_counts.front()
              : 0,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord>
classify_input_pnp(std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  auto const pnp_class = row_value(row, 5);
  if (name.empty() || virtual_host || !input_pnp_id_is_physical(pnp_id) ||
      input_device_is_virtual(name, pnp_id, std::span<std::string const>{})) {
    return std::nullopt;
  }
  auto const type = input_device_type_for(name, pnp_class);
  if (!type.has_value()) {
    return std::nullopt;
  }
  auto const connection =
      input_connection_for(name, pnp_id, std::span<std::string const>{});
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::input_device,
      .name = input_device_category_name(*type, connection),
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "physical input-device PNP id with explicit class/name",
      .input_device_type = *type,
      .input_device_connection = connection,
  };
}

struct CollectedDevice final {
  application::HardwareDeviceRecord record;
  // This key is deliberately confined to the Windows adapter because a full
  // PNP instance identifier is not part of the application observation.
  std::string stable_instance_key;
};

void append_grouped_device(std::vector<CollectedDevice>& devices,
                           CollectedDevice device) {
  auto& record = device.record;
  // Identical DIMMs are one physical model with a quantity, not separate
  // rows. Their compact summary deliberately uses ASCII parentheses, so only
  // this memory-specific grouping is allowed to inspect that presentation.
  if (record.kind == application::HardwareDeviceKind::memory) {
    for (auto& existing : devices) {
      auto& existing_record = existing.record;
      auto comparable_name = [](std::string const& value) {
        auto const marker = value.find(" (");
        return marker == std::string::npos ? value : value.substr(0, marker);
      };
      auto const same_memory_model =
          existing_record.kind == application::HardwareDeviceKind::memory &&
          comparable_name(existing_record.name) == comparable_name(record.name) &&
          ((existing_record.model_detail.empty() && record.model_detail.empty()) ||
           (!existing_record.model_detail.empty() &&
            existing_record.model_detail == record.model_detail));
      if (existing_record.kind == record.kind && same_memory_model &&
          existing_record.network_link == record.network_link &&
          existing_record.storage_media == record.storage_media &&
          existing_record.display_connection == record.display_connection) {
        existing_record.quantity += record.quantity;
        return;
      }
    }
  }
  // A repeated storage row is a duplicate projection of one physical
  // instance, not a second drive. Only an exact non-empty PNP identity may
  // collapse it; equal model text is insufficient.
  if (record.kind == application::HardwareDeviceKind::storage &&
      !device.stable_instance_key.empty()) {
    for (auto const& existing : devices) {
      if (existing.record.kind == record.kind &&
          !existing.stable_instance_key.empty() &&
          existing.stable_instance_key == device.stable_instance_key) {
        return;
      }
    }
  }
  // A PnPEntity row can be surfaced more than once by the provider. Input
  // devices are instance facts, so only the exact full PNP identity may merge
  // them; equal category text is not sufficient.
  if (record.kind == application::HardwareDeviceKind::input_device &&
      !device.stable_instance_key.empty()) {
    for (auto const& existing : devices) {
      if (existing.record.kind == record.kind &&
          !existing.stable_instance_key.empty() &&
          existing.stable_instance_key == device.stable_instance_key) {
        return;
      }
    }
  }
  // DesktopMonitor and PnPEntity project one panel through different WMI
  // classes. Match only their full stable PNP identity; same-model panels
  // must remain separate records.
  if (record.kind == application::HardwareDeviceKind::display) {
    for (auto& existing : devices) {
      auto& existing_record = existing.record;
      if (existing_record.kind == record.kind &&
          !existing.stable_instance_key.empty() &&
          !device.stable_instance_key.empty() &&
          existing.stable_instance_key == device.stable_instance_key) {
        if ((existing_record.display_width == 0 ||
             existing_record.display_height == 0) &&
            record.display_width != 0 && record.display_height != 0) {
          existing_record.display_width = record.display_width;
          existing_record.display_height = record.display_height;
        }
        if (existing_record.display_size_tenths_inch == 0 &&
            record.display_size_tenths_inch != 0) {
          existing_record.display_size_tenths_inch =
              record.display_size_tenths_inch;
        }
        if (existing_record.display_refresh_rate_hz == 0 &&
            record.display_refresh_rate_hz != 0) {
          existing_record.display_refresh_rate_hz = record.display_refresh_rate_hz;
        }
        if (existing_record.physical_refresh_rate_limit_hz == 0 &&
            record.physical_refresh_rate_limit_hz != 0) {
          existing_record.physical_refresh_rate_limit_hz =
              record.physical_refresh_rate_limit_hz;
        }
        if (existing_record.display_connection ==
                application::HardwareDisplayConnection::unknown &&
            record.display_connection !=
                application::HardwareDisplayConnection::unknown) {
          existing_record.display_connection = record.display_connection;
        }
        return;
      }
    }
  }
  devices.push_back(std::move(device));
}

[[nodiscard]] std::string grouped_name(
    application::HardwareDeviceRecord const& record) {
  if (record.kind == application::HardwareDeviceKind::input_device &&
      record.input_device_type == application::HardwareInputDeviceType::keyboard &&
      record.input_device_connection ==
          application::HardwareInputDeviceConnection::internal) {
    auto result = record.name.empty() ? std::string{"内建键盘"} : record.name;
    if (record.input_device_key_count != 0) {
      result += "（" + std::to_string(record.input_device_key_count) + " 键）";
    }
    return result;
  }
  if (record.kind == application::HardwareDeviceKind::display) {
    auto result = record.name;
    std::string facts;
    auto append_fact = [&facts](std::string value) {
      if (!facts.empty()) {
        facts += "；";
      }
      facts += std::move(value);
    };
    if (record.display_width != 0 && record.display_height != 0) {
      append_fact(std::to_string(record.display_width) + " × " +
                  std::to_string(record.display_height));
    } else {
      append_fact("分辨率未识别");
    }
    if (record.display_refresh_rate_hz != 0) {
      append_fact(std::to_string(record.display_refresh_rate_hz) + " Hz");
    } else {
      append_fact("刷新率未识别");
    }
    switch (record.display_connection) {
      case application::HardwareDisplayConnection::internal:
        append_fact("内建");
        break;
      case application::HardwareDisplayConnection::external:
        append_fact("外接");
        break;
      case application::HardwareDisplayConnection::unknown:
        append_fact("内建/外接未识别");
        break;
    }
    if (record.display_size_tenths_inch != 0) {
      auto const tenths = record.display_size_tenths_inch;
      auto size = std::to_string(tenths / 10);
      if (tenths % 10 != 0) {
        size += ".";
        size += std::to_string(tenths % 10);
      }
      append_fact(std::move(size) + " 英寸");
    } else {
      append_fact("英寸未识别");
    }
    result += "（";
    result += facts;
    result += "）";
    return result;
  }
  auto result = record.name;
  if (record.kind == application::HardwareDeviceKind::memory &&
      record.quantity > 1 && record.capacity_bytes != 0) {
    constexpr std::uint64_t kGigabyte = 1024ull * 1024ull * 1024ull;
    auto const module_gb = (record.capacity_bytes + kGigabyte / 2) / kGigabyte;
    auto const total_gb = module_gb * record.quantity;
    auto const marker = std::to_string(module_gb) + "GB";
    auto const position = result.find(marker);
    if (position != std::string::npos) {
      result.replace(position, marker.size(), std::to_string(total_gb) + "GB");
      result += " (";
      for (std::uint32_t index = 0; index < record.quantity; ++index) {
        if (index != 0) {
          result += " + ";
        }
        result += std::to_string(module_gb);
        result += "GB";
      }
      result += ")";
    }
  }
  // Part numbers remain structured in the record but are omitted from the
  // compact user-facing summary when firmware reports slot-specific values.
  if (record.quantity > 1 && record.kind != application::HardwareDeviceKind::memory) {
    result += " x" + std::to_string(record.quantity);
  }
  return result;
}

void rebuild_summary(
    std::string& target,
    std::vector<application::HardwareDeviceRecord> const& devices,
    application::HardwareDeviceKind kind,
    std::optional<application::HardwareNetworkLink> link = std::nullopt,
    std::optional<application::HardwareStorageMedia> media = std::nullopt,
    std::optional<application::HardwareInputDeviceType> input_type =
        std::nullopt) {
  target.clear();
  for (auto const& device : devices) {
    if (device.kind != kind ||
        (link.has_value() && device.network_link != *link) ||
        (media.has_value() && device.storage_media != *media) ||
        (input_type.has_value() &&
         device.input_device_type != *input_type)) {
      continue;
    }
    if (!target.empty()) {
      target.push_back('\n');
    }
    target.append(grouped_name(device));
  }
}

[[nodiscard]] application::HardwareObservationCode map_code(
    WindowsHardwareQueryCode code) noexcept {
  switch (code) {
    case WindowsHardwareQueryCode::succeeded:
      return application::HardwareObservationCode::succeeded;
    case WindowsHardwareQueryCode::permission_denied:
      return application::HardwareObservationCode::permission_denied;
    case WindowsHardwareQueryCode::cancelled:
      return application::HardwareObservationCode::cancelled;
    case WindowsHardwareQueryCode::timed_out:
      return application::HardwareObservationCode::timed_out;
    case WindowsHardwareQueryCode::failed:
      return application::HardwareObservationCode::failed;
  }
  return application::HardwareObservationCode::failed;
}

struct CollectedObservation final {
  application::HardwareObservation observation;
  application::HardwareObservationCode code{
      application::HardwareObservationCode::succeeded};
  std::string error;
  std::size_t successful_queries{0};
};

[[nodiscard]] CollectedObservation collect(
    WindowsHardwareQueryExecutor& executor, std::stop_token cancellation) {
  CollectedObservation collected;
  struct QuerySpec final {
    std::string_view class_name;
    std::array<std::string_view, 12> properties;
    std::size_t property_count;
  };
  std::array<QuerySpec, 12> const specs{
      QuerySpec{"Win32_Processor",
                {"Name", "Manufacturer", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode", "NumberOfCores",
                 "NumberOfLogicalProcessors"},
                7},
      QuerySpec{"Win32_VideoController",
                {"Name", "AdapterCompatibility", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode", "VideoProcessor", "AdapterRAM"},
                7},
      QuerySpec{"Win32_BaseBoard",
                {"Manufacturer", "Product", "HostingBoard", "Status"}, 4},
      QuerySpec{"Win32_NetworkAdapter",
                {"Name", "Manufacturer", "AdapterType", "PhysicalAdapter",
                 "PNPDeviceID", "Status", "ConfigManagerErrorCode",
                 "ServiceName", "NetConnectionStatus"},
                9},
      QuerySpec{"Win32_ComputerSystem", {"Manufacturer", "Model"}, 2},
      QuerySpec{"Win32_PhysicalMemory",
                {"Manufacturer", "Capacity", "Speed", "PartNumber",
                 "DeviceLocator", "Tag", "SMBIOSMemoryType",
                 "ConfiguredClockSpeed"},
                8},
      QuerySpec{"Win32_DesktopMonitor",
                {"Name", "PNPDeviceID", "Status", "ConfigManagerErrorCode",
                 "ScreenWidth", "ScreenHeight"},
                6},
      QuerySpec{"Win32_DiskDrive",
                {"Model", "Manufacturer", "Size", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode", "InterfaceType", "MediaType"},
                8},
      QuerySpec{"Win32_SoundDevice",
                {"Name", "Manufacturer", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode"},
                5},
      QuerySpec{"Win32_PnPEntity",
                {"Name", "Manufacturer", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode", "PNPClass"},
                6},
      QuerySpec{"Win32_OperatingSystem",
                {"Caption", "Version", "BuildNumber", "OSArchitecture"}, 4},
      QuerySpec{"Win32_Keyboard",
                {"Description", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode"},
                4},
  };

  // Keep one row matrix per approved query so classification consumes the
  // exact property order returned by the executor.
  std::vector<std::vector<std::vector<std::string>>> rows_by_spec;
  rows_by_spec.resize(specs.size());
  bool virtual_host = false;

  for (std::size_t spec_index = 0; spec_index < specs.size(); ++spec_index) {
    auto const& spec = specs[spec_index];
    if (cancellation.stop_requested()) {
      collected.code = application::HardwareObservationCode::cancelled;
      collected.error = "hardware observation cancelled";
      return collected;
    }
    auto const result = executor.query(
        spec.class_name,
        std::span<std::string_view const>{spec.properties}.first(
            spec.property_count),
        cancellation);
    if (result.code != WindowsHardwareQueryCode::succeeded) {
      auto const mapped = map_code(result.code);
      if (mapped == application::HardwareObservationCode::permission_denied ||
          mapped == application::HardwareObservationCode::cancelled ||
          mapped == application::HardwareObservationCode::timed_out) {
        collected.code = mapped;
        collected.error = join_error(std::move(collected.error), spec.class_name,
                                     result.error);
        return collected;
      }
      collected.error = join_error(std::move(collected.error), spec.class_name,
                                   result.error);
      continue;
    }
    ++collected.successful_queries;
    if (spec_index == 4) {
      if (!result.rows.empty()) {
        auto const& row = result.rows.front();
        auto const manufacturer = row_value(row, 0);
        auto const model = row_value(row, 1);
        collected.observation.oem_model = localized_brand_model(
            model.empty() ? manufacturer : model, manufacturer);
        collected.observation.oem_vendor =
            vendor_from_text(manufacturer);
        virtual_host = virtualization_marker(pair_value(result));
      }
    } else if (spec_index == 10) {
      if (!result.rows.empty()) {
        auto const& row = result.rows.front();
        auto const caption = row_value(row, 0);
        auto const version = row_value(row, 1);
        auto const build = row_value(row, 2);
        auto const architecture = row_value(row, 3);
        collected.observation.operating_system = presented_operating_system(
            caption, version, build, architecture);
      }
    } else {
      rows_by_spec[spec_index] = result.rows;
    }
  }

  if (cancellation.stop_requested()) {
    collected.code = application::HardwareObservationCode::cancelled;
    collected.error = "hardware observation cancelled";
    return collected;
  }
  std::optional<WindowsCpuTopology> cpu_topology;
  std::vector<WindowsGpuMemory> gpu_adapters;
  std::vector<WindowsDisplayEdid> display_edids;
  std::vector<WindowsDisplayConnection> display_connections;
  std::vector<WindowsDisplayPhysicalSize> display_physical_sizes;
  std::vector<WindowsInputDeviceMetadata> input_device_metadata;
  // Optional capabilities are isolated: one unavailable platform source must
  // not erase facts already obtained from another source.
  try {
    cpu_topology = executor.cpu_topology(cancellation);
  } catch (...) {
    cpu_topology.reset();
  }
  try {
    gpu_adapters = executor.gpu_memory(cancellation);
  } catch (...) {
    gpu_adapters.clear();
  }
  std::vector<std::string> display_pnp_ids;
  auto append_display_pnp_id = [&display_pnp_ids](std::string pnp_id) {
    if (!starts_with_ascii(pnp_id, "display\\") ||
        std::ranges::find(display_pnp_ids, pnp_id) !=
            display_pnp_ids.end()) {
      return;
    }
    display_pnp_ids.push_back(std::move(pnp_id));
  };
  for (auto const& row : rows_by_spec[6]) {
    append_display_pnp_id(row_value(row, 1));
  }
  for (auto const& row : rows_by_spec[9]) {
    if (contains_ascii(row_value(row, 5), "monitor")) {
      append_display_pnp_id(row_value(row, 2));
    }
  }
  try {
    display_edids = executor.display_edids(
        std::span<std::string const>{display_pnp_ids}, cancellation);
  } catch (...) {
    display_edids.clear();
  }
  try {
    display_connections = executor.display_connections(
        std::span<std::string const>{display_pnp_ids}, cancellation);
  } catch (...) {
    display_connections.clear();
  }
  try {
    display_physical_sizes = executor.display_physical_sizes(
        std::span<std::string const>{display_pnp_ids}, cancellation);
  } catch (...) {
    display_physical_sizes.clear();
  }
  std::vector<std::string> keyboard_pnp_ids;
  for (auto const& row : rows_by_spec[11]) {
    auto const pnp_device_id = row_value(row, 1);
    auto const normalized_key = normalized_instance_key(pnp_device_id);
    if (normalized_key.empty() ||
        std::ranges::any_of(
            keyboard_pnp_ids, [&](std::string const& existing_pnp_device_id) {
              return normalized_instance_key(existing_pnp_device_id) ==
                     normalized_key;
            })) {
      continue;
    }
    keyboard_pnp_ids.push_back(pnp_device_id);
  }
  try {
    input_device_metadata = executor.input_device_metadata(
        std::span<std::string const>{keyboard_pnp_ids}, cancellation);
  } catch (...) {
    input_device_metadata.clear();
  }
  if (cancellation.stop_requested()) {
    collected.code = application::HardwareObservationCode::cancelled;
    collected.error = "hardware observation cancelled";
    return collected;
  }

  std::vector<CollectedDevice> devices;
  std::vector<std::string> confirmed_cpu_models;
  for (auto const& row : rows_by_spec[0]) {
    if (auto record = classify_cpu(row, virtual_host, cpu_topology)) {
      auto model = normalized_cpu_model_name(row_value(row, 0));
      if (!model.empty() &&
          std::ranges::find(confirmed_cpu_models, model) ==
              confirmed_cpu_models.end()) {
        confirmed_cpu_models.push_back(std::move(model));
      }
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  for (auto const& row : rows_by_spec[1]) {
    if (auto record = classify_gpu(
            row, virtual_host,
            std::span<WindowsGpuMemory const>{gpu_adapters},
            std::span<std::string const>{confirmed_cpu_models.data(),
                                         confirmed_cpu_models.size()})) {
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  for (auto const& row : rows_by_spec[2]) {
    if (auto record = classify_board(row, virtual_host)) {
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  for (auto const& row : rows_by_spec[3]) {
    if (auto record = classify_network(row, virtual_host)) {
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  for (auto const& row : rows_by_spec[5]) {
    if (auto record = classify_memory(row, virtual_host)) {
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  // Prefer the EDID-derived Win32_PnPEntity projection. DesktopMonitor often
  // exposes the same physical panel as a generic name, so enrich only the
  // matching full PNP instance below.
  for (auto const& row : rows_by_spec[9]) {
    if (auto record = classify_display_pnp(row, virtual_host)) {
      append_grouped_device(
          devices,
          {.record = std::move(*record),
           .stable_instance_key = normalized_instance_key(row_value(row, 2))});
    }
  }
  for (auto const& row : rows_by_spec[6]) {
    if (auto record = classify_display(row, virtual_host)) {
      auto stable_instance_key = normalized_instance_key(row_value(row, 1));
      bool enriched_pnp_projection = false;
      // Keep the concrete PnP model but borrow the resolution that is only
      // exposed by DesktopMonitor. Distinct instance suffixes never match.
      if (!stable_instance_key.empty()) {
        for (auto& existing : devices) {
          if (existing.record.kind != application::HardwareDeviceKind::display ||
              existing.stable_instance_key != stable_instance_key) {
            continue;
          }
          if ((existing.record.display_width == 0 ||
               existing.record.display_height == 0) &&
              record->display_width != 0 && record->display_height != 0) {
            existing.record.display_width = record->display_width;
            existing.record.display_height = record->display_height;
          }
          if (existing.record.display_size_tenths_inch == 0 &&
              record->display_size_tenths_inch != 0) {
            existing.record.display_size_tenths_inch =
                record->display_size_tenths_inch;
          }
          enriched_pnp_projection = true;
          break;
        }
      }
      if (!enriched_pnp_projection) {
        append_grouped_device(
            devices,
            {.record = std::move(*record),
             .stable_instance_key = std::move(stable_instance_key)});
      }
    }
  }
  for (auto& device : devices) {
    auto& record = device.record;
    if (record.kind != application::HardwareDeviceKind::display) {
      continue;
    }
    auto const instance_ordinal = display_instance_ordinal_for(
        std::span<std::string const>{display_pnp_ids},
        device.stable_instance_key);
    auto const model_instance_count = display_model_instance_count(
        std::span<std::string const>{display_pnp_ids}, record.model_detail);
    if (auto const connection = display_connection_for(
            std::span<WindowsDisplayConnection const>{display_connections},
            record.model_detail, instance_ordinal, model_instance_count);
        connection.has_value()) {
      // DisplayConfig is the active-mode source. It supersedes a stale or
      // absent DesktopMonitor projection, while an unavailable fact stays
      // explicitly unknown rather than being inferred from model text.
      record.display_connection = connection->connection;
      if (connection->width != 0 && connection->height != 0) {
        record.display_width = connection->width;
        record.display_height = connection->height;
      }
      record.display_refresh_rate_hz = connection->refresh_rate_hz;
    }
    auto const limit = physical_refresh_rate_limit_for(
        display_edids, record.model_detail, instance_ordinal,
        model_instance_count);
    record.physical_refresh_rate_limit_hz = limit.value_or(0);
    auto size = display_size_tenths_for(
        display_edids, record.model_detail, instance_ordinal,
        model_instance_count);
    if (!size.has_value()) {
      size = display_size_tenths_for(
          std::span<WindowsDisplayPhysicalSize const>{display_physical_sizes},
          record.model_detail, instance_ordinal);
    }
    record.display_size_tenths_inch = size.value_or(0);
  }
  for (auto const& row : rows_by_spec[7]) {
    if (auto record = classify_storage(row, virtual_host)) {
      append_grouped_device(
          devices,
          {.record = std::move(*record),
           .stable_instance_key = normalized_instance_key(row_value(row, 3))});
    }
  }
  for (auto const& row : rows_by_spec[8]) {
    if (auto record = classify_audio(row, virtual_host)) {
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  for (auto const& row : rows_by_spec[9]) {
    if (auto record = classify_input_pnp(row, virtual_host)) {
      append_grouped_device(
          devices,
          {.record = std::move(*record),
           .stable_instance_key = normalized_instance_key(row_value(row, 2))});
    }
  }
  for (auto const& row : rows_by_spec[9]) {
    if (auto record = classify_npu(row, virtual_host)) {
      append_grouped_device(devices, {.record = std::move(*record)});
    }
  }
  for (auto const& row : rows_by_spec[11]) {
    auto const wmi_description = row_value(row, 0);
    std::vector<std::string> driver_descriptions;
    auto const stable_instance_key = normalized_instance_key(row_value(row, 1));
    if (!stable_instance_key.empty()) {
      for (auto const& metadata : input_device_metadata) {
        if (normalized_instance_key(metadata.pnp_device_id) !=
            stable_instance_key) {
          continue;
        }
        if (!metadata.bus_reported_device_description.empty()) {
          driver_descriptions.push_back(metadata.bus_reported_device_description);
        }
      }
    }
    if (auto record = classify_keyboard(
            row, wmi_description,
            std::span<std::string const>{driver_descriptions}, virtual_host)) {
      append_grouped_device(
          devices,
          {.record = std::move(*record),
           .stable_instance_key = stable_instance_key});
    }
  }

  collected.observation.devices.reserve(devices.size());
  for (auto& device : devices) {
    collected.observation.devices.push_back(std::move(device.record));
  }

  // Keep legacy summaries populated while exposing stable category splits.
  rebuild_summary(collected.observation.cpu, collected.observation.devices,
                  application::HardwareDeviceKind::cpu);
  rebuild_summary(collected.observation.gpu, collected.observation.devices,
                  application::HardwareDeviceKind::gpu);
  rebuild_summary(collected.observation.motherboard,
                  collected.observation.devices,
                  application::HardwareDeviceKind::motherboard);
  rebuild_summary(collected.observation.network_adapter,
                  collected.observation.devices,
                  application::HardwareDeviceKind::network_adapter);
  rebuild_summary(collected.observation.wired_network_adapter,
                  collected.observation.devices,
                  application::HardwareDeviceKind::network_adapter,
                  application::HardwareNetworkLink::wired);
  rebuild_summary(collected.observation.wireless_network_adapter,
                  collected.observation.devices,
                  application::HardwareDeviceKind::network_adapter,
                  application::HardwareNetworkLink::wireless);
  rebuild_summary(collected.observation.memory, collected.observation.devices,
                  application::HardwareDeviceKind::memory);
  rebuild_summary(collected.observation.display, collected.observation.devices,
                  application::HardwareDeviceKind::display);
  rebuild_summary(collected.observation.storage, collected.observation.devices,
                  application::HardwareDeviceKind::storage);
  rebuild_summary(collected.observation.solid_state_storage,
                  collected.observation.devices,
                  application::HardwareDeviceKind::storage, std::nullopt,
                  application::HardwareStorageMedia::solid_state);
  rebuild_summary(collected.observation.hard_disk_storage,
                  collected.observation.devices,
                  application::HardwareDeviceKind::storage, std::nullopt,
                  application::HardwareStorageMedia::hard_disk);
  rebuild_summary(collected.observation.audio, collected.observation.devices,
                  application::HardwareDeviceKind::audio);
  rebuild_summary(collected.observation.npu, collected.observation.devices,
                  application::HardwareDeviceKind::npu);
  rebuild_summary(collected.observation.keyboard,
                   collected.observation.devices,
                   application::HardwareDeviceKind::input_device, std::nullopt,
                   std::nullopt,
                   application::HardwareInputDeviceType::keyboard);
  rebuild_summary(collected.observation.mouse,
                  collected.observation.devices,
                  application::HardwareDeviceKind::input_device, std::nullopt,
                  std::nullopt,
                  application::HardwareInputDeviceType::mouse);
  rebuild_summary(collected.observation.touchpad,
                  collected.observation.devices,
                  application::HardwareDeviceKind::input_device, std::nullopt,
                  std::nullopt,
                  application::HardwareInputDeviceType::touchpad);

  if (!collected.observation.usable()) {
    if (collected.code == application::HardwareObservationCode::succeeded) {
      collected.code = application::HardwareObservationCode::failed;
    }
    if (collected.error.empty()) {
      collected.error = "hardware observation returned no confirmed physical hardware";
    }
  } else if (collected.successful_queries != specs.size() &&
             collected.code == application::HardwareObservationCode::succeeded) {
    collected.code = application::HardwareObservationCode::partial;
  }
  return collected;
}

class WmiHardwareQueryExecutor final : public WindowsHardwareQueryExecutor {
 public:
  [[nodiscard]] std::optional<WindowsCpuTopology> cpu_topology(
      std::stop_token cancellation) override {
    if (cancellation.stop_requested()) {
      return std::nullopt;
    }
    DWORD bytes = 0;
    if (::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr,
                                           &bytes) != FALSE ||
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
      return std::nullopt;
    }
    auto const slots = (static_cast<std::size_t>(bytes) +
                        sizeof(std::max_align_t) - 1) /
                       sizeof(std::max_align_t);
    std::vector<std::max_align_t> buffer(slots);
    if (::GetLogicalProcessorInformationEx(
            RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                buffer.data()),
            &bytes) == FALSE) {
      return std::nullopt;
    }

    std::array<std::uint32_t, 256> cores_by_efficiency{};
    std::uint32_t logical_processors = 0;
    std::size_t offset = 0;
    while (offset < bytes) {
      constexpr auto kProcessorOffset =
          offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);
      constexpr auto kGroupMaskOffset =
          offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
      if (bytes - offset < kProcessorOffset + kGroupMaskOffset) {
        return std::nullopt;
      }
      auto const* information =
          reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX const*>(
              reinterpret_cast<std::byte const*>(buffer.data()) + offset);
      if (information->Size == 0 || information->Size > bytes - offset) {
        return std::nullopt;
      }
      if (information->Relationship == RelationProcessorCore) {
        auto const group_bytes =
            static_cast<std::size_t>(information->Processor.GroupCount) *
            sizeof(GROUP_AFFINITY);
        if (information->Size < kProcessorOffset + kGroupMaskOffset +
                                    group_bytes) {
          return std::nullopt;
        }
        ++cores_by_efficiency[information->Processor.EfficiencyClass];
        for (WORD group = 0; group < information->Processor.GroupCount;
             ++group) {
          auto mask = information->Processor.GroupMask[group].Mask;
          while (mask != 0) {
            logical_processors += static_cast<std::uint32_t>(mask & 1);
            mask >>= 1;
          }
        }
      }
      offset += information->Size;
    }

    WindowsCpuTopology topology{.logical_processors = logical_processors};
    std::uint32_t distinct_efficiency_classes = 0;
    std::uint32_t efficiency_efficiency_class = 0;
    std::uint32_t performance_efficiency_class = 0;
    for (std::uint32_t index = 0; index < cores_by_efficiency.size(); ++index) {
      if (cores_by_efficiency[index] == 0) {
        continue;
      }
      if (distinct_efficiency_classes == 0) {
        efficiency_efficiency_class = index;
      }
      performance_efficiency_class = index;
      ++distinct_efficiency_classes;
    }
    // PROCESSOR_RELATIONSHIP defines higher EfficiencyClass values as more
    // performant and less efficient:
    // https://learn.microsoft.com/windows/win32/api/winnt/ns-winnt-processor_relationship
    // A single class describes a homogeneous topology, not a P-core label.
    // Two classes are P/E. Three classes are P/E/LP-E, ordered from highest
    // to lowest EfficiencyClass. Any other topology remains explicitly
    // unlabelled rather than being guessed.
    if (distinct_efficiency_classes == 2) {
      topology.performance_cores =
          cores_by_efficiency[performance_efficiency_class];
      topology.efficiency_cores = cores_by_efficiency[efficiency_efficiency_class];
      topology.split_known = topology.performance_cores != 0 &&
                             topology.efficiency_cores != 0 &&
                             topology.logical_processors != 0;
    } else if (distinct_efficiency_classes == 3) {
      std::uint32_t middle_efficiency_class = 0;
      for (std::uint32_t index = 0; index < cores_by_efficiency.size();
           ++index) {
        if (cores_by_efficiency[index] != 0 &&
            index != efficiency_efficiency_class &&
            index != performance_efficiency_class) {
          middle_efficiency_class = index;
          break;
        }
      }
      topology.performance_cores =
          cores_by_efficiency[performance_efficiency_class];
      topology.efficiency_cores = cores_by_efficiency[middle_efficiency_class];
      topology.low_power_efficiency_cores =
          cores_by_efficiency[efficiency_efficiency_class];
      topology.split_known = topology.performance_cores != 0 &&
                             topology.efficiency_cores != 0 &&
                             topology.low_power_efficiency_cores != 0 &&
                             topology.logical_processors != 0;
    }
    return topology;
  }

  [[nodiscard]] std::vector<WindowsGpuMemory> gpu_memory(
      std::stop_token cancellation) override {
    if (cancellation.stop_requested()) {
      return {};
    }
    auto const dxgi_module = ::LoadLibraryW(L"dxgi.dll");
    if (dxgi_module == nullptr) {
      return {};
    }
    using CreateDxgiFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto const create_factory = reinterpret_cast<CreateDxgiFactory1>(
        ::GetProcAddress(dxgi_module, "CreateDXGIFactory1"));
    if (create_factory == nullptr) {
      ::FreeLibrary(dxgi_module);
      return {};
    }

    std::vector<WindowsGpuMemory> result;
    {
      ComPtr<IDXGIFactory1> factory;
      if (FAILED(create_factory(
              __uuidof(IDXGIFactory1),
              reinterpret_cast<void**>(factory.GetAddressOf())))) {
        ::FreeLibrary(dxgi_module);
        return {};
      }

      for (UINT index = 0; !cancellation.stop_requested(); ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        auto const enumeration =
            factory->EnumAdapters1(index, adapter.GetAddressOf());
        if (enumeration == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        if (FAILED(enumeration)) {
          continue;
        }
        DXGI_ADAPTER_DESC1 description{};
        if (FAILED(adapter->GetDesc1(&description)) ||
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
          continue;
        }
        auto name = utf8_from_wide(description.Description);
        if (name.empty()) {
          continue;
        }
        auto gpu_type = application::HardwareGpuType::unknown;
        // Memory size alone cannot distinguish an entry-level discrete card
        // from an integrated adapter with a shared aperture. Keep DXGI
        // conservative; exact PCI/model identities are resolved later.
        if (description.VendorId == 0x10de) {
          gpu_type = application::HardwareGpuType::discrete;
        }
        result.push_back(WindowsGpuMemory{
            .model_name = std::move(name),
            .dedicated_video_memory =
                static_cast<std::uint64_t>(description.DedicatedVideoMemory),
            .shared_system_memory =
                static_cast<std::uint64_t>(description.SharedSystemMemory),
            .gpu_type = gpu_type,
            .vendor_id = description.VendorId,
            .device_id = description.DeviceId,
        });
      }
    }
    ::FreeLibrary(dxgi_module);
    return result;
  }

  [[nodiscard]] std::vector<WindowsDisplayEdid> display_edids(
      std::span<std::string const> pnp_device_ids,
      std::stop_token cancellation) override {
    std::vector<WindowsDisplayEdid> result;
    for (std::size_t index = 0; index < pnp_device_ids.size(); ++index) {
      auto const& pnp_device_id = pnp_device_ids[index];
      if (cancellation.stop_requested()) {
        return {};
      }
      if (!starts_with_ascii(pnp_device_id, "display\\") ||
          pnp_device_id.find("..") != std::string::npos ||
          pnp_device_id.find('/') != std::string::npos) {
        continue;
      }
      auto const wide_pnp_id = wide_from_utf8(pnp_device_id);
      auto const model_key = display_model_key(pnp_device_id);
      if (wide_pnp_id.empty() || model_key.empty()) {
        continue;
      }
      std::wstring registry_path =
          L"SYSTEM\\CurrentControlSet\\Enum\\" + wide_pnp_id +
          L"\\Device Parameters";
      HKEY raw_key = nullptr;
      if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, registry_path.c_str(), 0,
                          KEY_QUERY_VALUE, &raw_key) != ERROR_SUCCESS) {
        continue;
      }
      RegistryKey key{raw_key};
      DWORD type = 0;
      DWORD byte_count = 0;
      if (::RegQueryValueExW(key.get(), L"EDID", nullptr, &type, nullptr,
                             &byte_count) != ERROR_SUCCESS ||
          type != REG_BINARY || byte_count < 128 || byte_count % 128 != 0 ||
          byte_count > 128 * 32) {
        continue;
      }
      std::vector<std::uint8_t> bytes(byte_count);
      if (::RegQueryValueExW(key.get(), L"EDID", nullptr, &type, bytes.data(),
                             &byte_count) != ERROR_SUCCESS ||
          type != REG_BINARY || byte_count < 128 || byte_count % 128 != 0) {
        continue;
      }
      bytes.resize(byte_count);
      result.push_back(
          {.model_key = model_key,
           .instance_ordinal = static_cast<std::uint32_t>(index + 1),
           .bytes = std::move(bytes)});
    }
    return result;
  }

  [[nodiscard]] std::vector<WindowsDisplayPhysicalSize> display_physical_sizes(
      std::span<std::string const> pnp_device_ids,
      std::stop_token cancellation) override {
    constexpr std::array<std::string_view, 4> kProperties{
        "InstanceName", "Active", "MaxHorizontalImageSize",
        "MaxVerticalImageSize"};
    auto const query_result = query_in_namespace(
        L"ROOT\\WMI", "WmiMonitorBasicDisplayParams",
        std::span<std::string_view const>{kProperties}, cancellation);
    if (query_result.code != WindowsHardwareQueryCode::succeeded) {
      return {};
    }

    std::vector<WindowsDisplayPhysicalSize> result;
    for (auto const& row : query_result.rows) {
      if (cancellation.stop_requested()) {
        return {};
      }
      bool active = false;
      std::uint32_t horizontal_centimeters = 0;
      std::uint32_t vertical_centimeters = 0;
      if (!parse_bool(row_value(row, 1), active) || !active ||
          !parse_integer(row_value(row, 2), horizontal_centimeters) ||
          !parse_integer(row_value(row, 3), vertical_centimeters) ||
          horizontal_centimeters == 0 || vertical_centimeters == 0) {
        continue;
      }

      auto const instance_name = row_value(row, 0);
      if (instance_name.size() <= 2 || !instance_name.ends_with("_0")) {
        continue;
      }
      // WMI appends exactly "_0" to the full PNP instance identity. Do not
      // derive an ordinal by model, prefix, or a fuzzy device-path match.
      auto const instance_ordinal = display_instance_ordinal_for(
          pnp_device_ids,
          std::string_view{instance_name}.substr(0, instance_name.size() - 2));
      if (instance_ordinal == 0) {
        continue;
      }
      auto const model_key =
          display_model_key(pnp_device_ids[instance_ordinal - 1]);
      if (model_key.empty()) {
        continue;
      }
      result.push_back(WindowsDisplayPhysicalSize{
          .model_key = model_key,
          .instance_ordinal = instance_ordinal,
          .horizontal_centimeters = horizontal_centimeters,
          .vertical_centimeters = vertical_centimeters,
      });
    }
    return result;
  }

  [[nodiscard]] std::vector<WindowsInputDeviceMetadata>
  input_device_metadata(std::span<std::string const> pnp_device_ids,
                        std::stop_token cancellation) override {
    std::vector<std::string> requested_instance_keys;
    requested_instance_keys.reserve(pnp_device_ids.size());
    for (auto const& pnp_device_id : pnp_device_ids) {
      auto const normalized_key = normalized_instance_key(pnp_device_id);
      if (normalized_key.empty() ||
          std::ranges::find(requested_instance_keys, normalized_key) !=
              requested_instance_keys.end()) {
        continue;
      }
      requested_instance_keys.push_back(normalized_key);
    }
    if (requested_instance_keys.empty() || cancellation.stop_requested()) {
      return {};
    }

    DeviceInfoSet devices{::SetupDiGetClassDevsW(
        &GUID_DEVCLASS_KEYBOARD, nullptr, nullptr, DIGCF_PRESENT)};
    if (!devices.usable()) {
      return {};
    }

    std::vector<WindowsInputDeviceMetadata> result;
    for (DWORD index = 0; !cancellation.stop_requested(); ++index) {
      SP_DEVINFO_DATA device_info{};
      device_info.cbSize = sizeof(device_info);
      if (::SetupDiEnumDeviceInfo(devices.get(), index, &device_info) == FALSE) {
        if (::GetLastError() == ERROR_NO_MORE_ITEMS) {
          break;
        }
        continue;
      }
      auto pnp_device_id = setup_device_instance_id(devices.get(), device_info);
      if (std::ranges::find(requested_instance_keys,
                            normalized_instance_key(pnp_device_id)) ==
          requested_instance_keys.end()) {
        continue;
      }
      result.push_back(WindowsInputDeviceMetadata{
          .pnp_device_id = std::move(pnp_device_id),
          .container_id = setup_device_container_id(devices.get(), device_info),
          .bus_reported_device_description = setup_device_property_string(
              devices.get(), device_info,
              DEVPKEY_Device_BusReportedDeviceDesc),
      });
    }
    return result;
  }

  [[nodiscard]] std::vector<WindowsDisplayConnection> display_connections(
      std::span<std::string const> pnp_device_ids,
      std::stop_token cancellation) override {
    std::vector<WindowsDisplayConnection> result;
    if (cancellation.stop_requested()) {
      return result;
    }

    std::vector<std::string> requested_model_keys;
    requested_model_keys.reserve(pnp_device_ids.size());
    for (auto const& pnp_device_id : pnp_device_ids) {
      auto const model_key = display_model_key(pnp_device_id);
      if (model_key.empty() ||
          std::ranges::find(requested_model_keys, model_key) !=
              requested_model_keys.end()) {
        continue;
      }
      requested_model_keys.push_back(model_key);
    }
    if (requested_model_keys.empty()) {
      return result;
    }

    UINT path_count = 0;
    UINT mode_count = 0;
    auto status = ::GetDisplayConfigBufferSizes(
        QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
    if (status != ERROR_SUCCESS || path_count == 0) {
      return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    bool query_succeeded = false;
    for (std::uint32_t attempt = 0; attempt < 3; ++attempt) {
      paths.resize(path_count);
      modes.resize(mode_count);
      auto queried_path_count = path_count;
      auto queried_mode_count = mode_count;
      status = ::QueryDisplayConfig(
          QDC_ONLY_ACTIVE_PATHS, &queried_path_count, paths.data(),
          &queried_mode_count, modes.data(), nullptr);
      if (status != ERROR_INSUFFICIENT_BUFFER) {
        path_count = queried_path_count;
        mode_count = queried_mode_count;
        query_succeeded = status == ERROR_SUCCESS;
        break;
      }
      status = ::GetDisplayConfigBufferSizes(
          QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
      if (status != ERROR_SUCCESS || path_count == 0) {
        return {};
      }
    }
    if (!query_succeeded || status != ERROR_SUCCESS || path_count == 0 ||
        path_count > paths.size() || mode_count > modes.size()) {
      return result;
    }
    paths.resize(path_count);
    modes.resize(mode_count);

    auto unique_requested_instance_ordinal_for_model =
        [&](std::string_view model_key) -> std::uint32_t {
      std::uint32_t candidate = 0;
      for (std::size_t index = 0; index < pnp_device_ids.size(); ++index) {
        auto const& pnp_device_id = pnp_device_ids[index];
        if (display_model_key(pnp_device_id) != model_key) {
          continue;
        }
        auto const ordinal = static_cast<std::uint32_t>(index + 1);
        if (candidate == 0) {
          candidate = ordinal;
        } else if (candidate != ordinal) {
          return 0;
        }
      }
      return candidate;
    };

    for (auto const& path : paths) {
      if (cancellation.stop_requested()) {
        return {};
      }
      if (!path.targetInfo.targetAvailable) {
        continue;
      }

      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
      target.header.size = sizeof(target);
      target.header.adapterId = path.targetInfo.adapterId;
      target.header.id = path.targetInfo.id;
      if (::DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS) {
        continue;
      }
      auto const model_key = display_model_key_from_monitor_device_path(
          target.monitorDevicePath);
      if (model_key.empty() ||
          std::ranges::find(requested_model_keys, model_key) ==
              requested_model_keys.end()) {
        continue;
      }

      auto const stable_instance_key =
          display_instance_key_from_monitor_device_path(
              target.monitorDevicePath);
      std::uint32_t instance_ordinal = 0;
      if (!stable_instance_key.empty()) {
        instance_ordinal = display_instance_ordinal_for(
            pnp_device_ids, stable_instance_key);
        // A path with an explicit but non-requested PNP identity must not
        // become a model-scoped fact for another same-model display.
        if (instance_ordinal == 0) {
          continue;
        }
      } else {
        // DisplayConfig paths normally expose only a model token. Associate
        // that model with an instance only when it is unique in the request;
        // otherwise retain an unscoped fact that the consumer rejects for
        // same-model multi-monitor requests.
        instance_ordinal =
            unique_requested_instance_ordinal_for_model(model_key);
      }

      auto connection = display_connection_from_technology(
          path.targetInfo.outputTechnology);
      if (target.outputTechnology != path.targetInfo.outputTechnology) {
        connection = application::HardwareDisplayConnection::unknown;
      }
      auto const dimensions = dimensions_from_target_mode(
          path, std::span<DISPLAYCONFIG_MODE_INFO const>{modes});
      auto const refresh_rate =
          refresh_rate_from_rational(path.targetInfo.refreshRate);
      result.push_back(WindowsDisplayConnection{
           .model_key = model_key,
           .instance_ordinal = instance_ordinal,
           .connection = connection,
          .width = dimensions.has_value() ? dimensions->first : 0,
          .height = dimensions.has_value() ? dimensions->second : 0,
          .refresh_rate_hz = refresh_rate.value_or(0),
      });
    }
    return result;
  }

  [[nodiscard]] WindowsHardwareQueryResult query(
      std::string_view class_name,
      std::span<std::string_view const> properties,
      std::stop_token cancellation) override {
    return query_in_namespace(L"ROOT\\CIMV2", class_name, properties,
                              cancellation);
  }

 private:
  [[nodiscard]] WindowsHardwareQueryResult query_in_namespace(
      std::wstring_view namespace_name,
      std::string_view class_name,
      std::span<std::string_view const> properties,
      std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
      return {.code = WindowsHardwareQueryCode::cancelled,
              .error = "hardware observation cancelled"};
    }
    ComApartment apartment;
    if (!apartment.usable()) {
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "COM apartment unavailable"};
    }

    auto const security = ::CoInitializeSecurity(
        nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(security) && security != RPC_E_TOO_LATE) {
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "WMI security initialization failed"};
    }

    ComPtr<IWbemLocator> locator;
    auto const locator_result = ::CoCreateInstance(
        CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(locator.GetAddressOf()));
    if (FAILED(locator_result)) {
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "WMI locator unavailable"};
    }

    ComPtr<IWbemServices> services;
    auto const namespace_storage = std::wstring{namespace_name};
    auto const namespace_bstr = ::SysAllocString(namespace_storage.c_str());
    if (namespace_bstr == nullptr) {
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "WMI namespace allocation failed"};
    }
    auto const connect_result = locator->ConnectServer(
        namespace_bstr, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
        services.GetAddressOf());
    ::SysFreeString(namespace_bstr);
    if (FAILED(connect_result)) {
      return {.code = connect_result == WBEM_E_ACCESS_DENIED
                          ? WindowsHardwareQueryCode::permission_denied
                          : WindowsHardwareQueryCode::failed,
              .error = "WMI namespace unavailable"};
    }

    auto const blanket_result = ::CoSetProxyBlanket(
        services.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
        EOAC_NONE);
    if (FAILED(blanket_result)) {
      return {.code = blanket_result == WBEM_E_ACCESS_DENIED
                          ? WindowsHardwareQueryCode::permission_denied
                          : WindowsHardwareQueryCode::failed,
              .error = "WMI proxy authorization failed"};
    }

    std::wstring query = L"SELECT ";
    for (std::size_t index = 0; index < properties.size(); ++index) {
      if (index != 0) {
        query.append(L", ");
      }
      query.append(wide_from_utf8(properties[index]));
    }
    query.append(L" FROM ");
    query.append(wide_from_utf8(class_name));
    auto const query_language = ::SysAllocString(L"WQL");
    auto const query_text = ::SysAllocString(query.c_str());
    if (query_language == nullptr || query_text == nullptr) {
      ::SysFreeString(query_language);
      ::SysFreeString(query_text);
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "WMI query allocation failed"};
    }
    ComPtr<IEnumWbemClassObject> enumerator;
    auto const query_result = services->ExecQuery(
        query_language, query_text,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
        enumerator.GetAddressOf());
    ::SysFreeString(query_language);
    ::SysFreeString(query_text);
    if (FAILED(query_result)) {
      return {.code = query_result == WBEM_E_ACCESS_DENIED
                          ? WindowsHardwareQueryCode::permission_denied
                          : WindowsHardwareQueryCode::failed,
              .error = "WMI query failed"};
    }

    WindowsHardwareQueryResult output{.code = WindowsHardwareQueryCode::succeeded};
    while (true) {
      if (cancellation.stop_requested()) {
        output.code = WindowsHardwareQueryCode::cancelled;
        output.error = "hardware observation cancelled";
        return output;
      }
      ComPtr<IWbemClassObject> object;
      ULONG returned = 0;
      auto const next_result = enumerator->Next(5000, 1, object.GetAddressOf(),
                                                &returned);
      if (next_result == WBEM_S_FALSE || returned == 0) {
        break;
      }
      if (next_result == WBEM_S_TIMEDOUT) {
        output.code = WindowsHardwareQueryCode::timed_out;
        output.error = "WMI query timed out";
        return output;
      }
      if (FAILED(next_result)) {
        output.code = next_result == WBEM_E_ACCESS_DENIED
                          ? WindowsHardwareQueryCode::permission_denied
                          : WindowsHardwareQueryCode::failed;
        output.error = "WMI result enumeration failed";
        return output;
      }

      std::vector<std::string> row;
      row.reserve(properties.size());
      for (auto const property : properties) {
        auto const property_name = ::SysAllocString(wide_from_utf8(property).c_str());
        if (property_name == nullptr) {
          row.emplace_back();
          continue;
        }
        VARIANT value;
        ::VariantInit(&value);
        auto const get_result = object->Get(property_name, 0, &value, nullptr,
                                            nullptr);
        ::SysFreeString(property_name);
        if (FAILED(get_result)) {
          row.emplace_back();
          ::VariantClear(&value);
          continue;
        }
        if (value.vt == VT_BSTR && value.bstrVal != nullptr) {
          row.push_back(utf8_from_wide(value.bstrVal));
        } else {
          switch (value.vt) {
            case VT_I1:
              row.push_back(std::to_string(value.cVal));
              break;
            case VT_UI1:
              row.push_back(std::to_string(value.bVal));
              break;
            case VT_BOOL:
              row.push_back(value.boolVal == VARIANT_TRUE ? "true" : "false");
              break;
            case VT_I2:
              row.push_back(std::to_string(value.iVal));
              break;
            case VT_UI2:
              row.push_back(std::to_string(value.uiVal));
              break;
            case VT_I4:
              row.push_back(std::to_string(value.lVal));
              break;
            case VT_UI4:
              row.push_back(std::to_string(value.ulVal));
              break;
            case VT_I8:
              row.push_back(std::to_string(value.llVal));
              break;
            case VT_UI8:
              row.push_back(std::to_string(value.ullVal));
              break;
            case VT_INT:
              row.push_back(std::to_string(value.intVal));
              break;
            case VT_UINT:
              row.push_back(std::to_string(value.uintVal));
              break;
            default:
              row.emplace_back();
              break;
          }
        }
        ::VariantClear(&value);
      }
      output.rows.push_back(std::move(row));
    }
    return output;
  }
};

}  // namespace

WindowsHardwareObserver::WindowsHardwareObserver()
    : executor_(std::make_unique<WmiHardwareQueryExecutor>()) {}

WindowsHardwareObserver::WindowsHardwareObserver(
    std::unique_ptr<WindowsHardwareQueryExecutor> executor)
    : executor_(std::move(executor)) {}

application::HardwareObservationResult WindowsHardwareObserver::observe(
    std::stop_token cancellation) {
  if (!executor_) {
    return {.code = application::HardwareObservationCode::failed,
            .error = "hardware query executor unavailable"};
  }
  auto collected = collect(*executor_, cancellation);
  return {.code = collected.code,
          .observation = std::move(collected.observation),
          .error = std::move(collected.error)};
}

std::optional<application::HardwareObservation>
WindowsHardwareObserver::current_model_observation(
    std::stop_token cancellation) {
  if (!executor_ || cancellation.stop_requested()) {
    return std::nullopt;
  }
  auto collected = collect(*executor_, cancellation);
  if (!collected.observation.usable() ||
      collected.code != application::HardwareObservationCode::succeeded) {
    return std::nullopt;
  }
  return collected.observation;
}

}  // namespace azzs::adapters::windows
