#include "azzs/adapters/windows/windows_hardware_observer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

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

struct LocalizedBrand final {
  std::string_view marker;
  std::string_view display;
};

// Keep the original model text and replace only a confirmed vendor token.
// This gives people who recognize a Chinese vendor name a useful cue without
// turning a hardware observation into a guessed model translation.
constexpr std::array<LocalizedBrand, 15> kLocalizedBrands{
    LocalizedBrand{"titan army", "泰坦军团 TITAN ARMY"},
    LocalizedBrand{"p275mv", "泰坦军团 TITAN ARMY P275MV"},
    LocalizedBrand{"sk hynix", "SK 海力士"},
    LocalizedBrand{"hynix", "SK 海力士"},
    LocalizedBrand{"samsung", "三星 Samsung"},
    LocalizedBrand{"micron", "美光 Micron"},
    LocalizedBrand{"realtek", "瑞昱 Realtek"},
    LocalizedBrand{"boe", "京东方 BOE"},
    LocalizedBrand{"nvidia", "英伟达 NVIDIA"},
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
  auto result = trim_ascii(model);
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
      return replace_first_ascii_case_insensitive(
          std::move(result), candidate.marker, candidate.display);
    }
  }
  return std::string{brand->display} + " " + result;
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

[[nodiscard]] application::HardwareDisplayConnection display_connection_from(
    std::string_view name, std::string_view pnp_id) noexcept {
  auto const model_key = display_model_key(pnp_id);
  // BOE0CD1 is the EDID model token observed for the built-in panel on the
  // supported reference machine. Do not generalize this to other BOE panels.
  if (model_key == "boe0cd1" || contains_ascii(name, "integrated monitor") ||
      name.find("集成监视器") != std::string_view::npos ||
      name.find("内建显示器") != std::string_view::npos) {
    return application::HardwareDisplayConnection::internal;
  }
  // P275MV PLUS is a verified TITAN ARMY external-monitor model. Other
  // generic PnP names remain unknown instead of being guessed as external.
  if (contains_ascii(name, "p275mv")) {
    return application::HardwareDisplayConnection::external;
  }
  return application::HardwareDisplayConnection::unknown;
}

[[nodiscard]] std::string_view display_connection_label(
    application::HardwareDisplayConnection connection) noexcept {
  switch (connection) {
    case application::HardwareDisplayConnection::internal: return "内建";
    case application::HardwareDisplayConnection::external: return "外接";
    case application::HardwareDisplayConnection::unknown: return "类型未确认";
  }
  return "类型未确认";
}

[[nodiscard]] std::string presented_display_name(
    std::string_view name, std::string_view manufacturer,
    std::string_view pnp_id,
    application::HardwareDisplayConnection connection) {
  auto result = localized_brand_model(
      format_display_name(name, pnp_id, false), manufacturer);
  if (result.empty()) {
    return {};
  }
  result += "（";
  result += display_connection_label(connection);
  result += "）";
  return result;
}

void append_display_resolution(std::string& value, std::uint32_t width,
                               std::uint32_t height) {
  if (width == 0 || height == 0) {
    return;
  }
  auto const closing = value.rfind("）");
  auto const resolution = "；" + std::to_string(width) + " × " +
                          std::to_string(height);
  if (closing == std::string::npos) {
    value += "（";
    value += resolution.substr(std::string_view{"；"}.size());
    value += "）";
    return;
  }
  value.insert(closing, resolution);
}

[[nodiscard]] bool has_display_resolution(std::string_view value) noexcept {
  return value.find(" × ") != std::string_view::npos;
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
    std::string_view model_key) noexcept {
  std::optional<std::uint32_t> limit;
  auto const normalized_key = lower_ascii(model_key);
  for (auto const& edid : edids) {
    if (lower_ascii(edid.model_key) != normalized_key) {
      continue;
    }
    auto const candidate = physical_refresh_rate_limit_from_edid(edid.bytes);
    if (!candidate.has_value() ||
        (limit.has_value() && *limit != *candidate)) {
      return std::nullopt;
    }
    limit = candidate;
  }
  return limit;
}

void append_display_refresh_rate_limit(
    std::string& value, std::optional<std::uint32_t> limit) {
  std::string detail{"；物理刷新率上限（EDID）："};
  if (limit.has_value()) {
    detail += std::to_string(*limit);
    detail += " Hz";
  } else {
    detail += "未读取";
  }
  auto const closing = value.rfind("）");
  if (closing == std::string::npos) {
    value += "（";
    value += detail.substr(std::string_view{"；"}.size());
    value += "）";
    return;
  }
  value.insert(closing, detail);
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
  std::string result;
  result.reserve(value.size());
  for (auto const character : value) {
    auto const unsigned_character = static_cast<unsigned char>(character);
    if (std::isalnum(unsigned_character) != 0) {
      result.push_back(static_cast<char>(std::tolower(unsigned_character)));
    }
  }
  return result;
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

[[nodiscard]] std::string gpu_memory_description(
    WindowsGpuMemory const* memory) {
  if (memory == nullptr ||
      (memory->dedicated_video_memory == 0 &&
       memory->shared_system_memory == 0)) {
    return "显存：未读取";
  }
  std::string result;
  if (memory->dedicated_video_memory != 0) {
    result = "专用显存 ";
    result += gpu_memory_size(memory->dedicated_video_memory, true);
  } else {
    result = "专用显存 未读取";
  }
  if (memory->shared_system_memory != 0) {
    result += "；共享内存 ";
    result += gpu_memory_size(memory->shared_system_memory, false);
  }
  return result;
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
  auto const topology_cores = topology.has_value() && topology->split_known
                                  ? topology->performance_cores +
                                        topology->efficiency_cores
                                  : 0;
  auto const reported_cores = cores_valid ? cores : topology_cores;
  auto const reported_threads =
      threads_valid ? threads
                    : (topology.has_value() ? topology->logical_processors : 0);
  if (reported_cores != 0 || reported_threads != 0 ||
      (topology.has_value() && topology->split_known)) {
    display_name += "（";
    if (reported_cores != 0) {
      display_name += "核心 ";
      display_name += std::to_string(reported_cores);
    }
    if (reported_cores != 0 && reported_threads != 0) {
      display_name += "，";
    }
    if (reported_threads != 0) {
      display_name += "线程 ";
      display_name += std::to_string(reported_threads);
    }
    if (topology.has_value() && topology->split_known &&
        topology->performance_cores != 0 && topology->efficiency_cores != 0) {
      display_name += "；P 核 ";
      display_name += std::to_string(topology->performance_cores);
      display_name += "，E 核 ";
      display_name += std::to_string(topology->efficiency_cores);
    } else if (contains_ascii(name, "275hx")) {
      // This reference model is a verified hybrid processor. Do not infer a
      // split for any other model when the platform API is unavailable.
      display_name += "；P/E 核未读取";
    }
    display_name += "）";
  }
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
      .performance_core_count =
          topology.has_value() && topology->split_known
              ? topology->performance_cores
              : 0,
      .efficiency_core_count = topology.has_value() && topology->split_known
                                   ? topology->efficiency_cores
                                   : 0,
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_gpu(
    std::vector<std::string> const& row, bool virtual_host,
    std::span<WindowsGpuMemory const> dxgi_adapters) {
  auto const name = row_value(row, 0);
  auto const compatibility = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !physical_bus(pnp_id, false)) {
    return std::nullopt;
  }
  auto display_name = localized_brand_model(name, compatibility);
  auto const* memory = reliable_gpu_memory(name, dxgi_adapters);
  display_name += "（";
  display_name += gpu_memory_description(memory);
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

  std::string name = localized_brand_model(manufacturer, manufacturer);
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
  auto const connection = display_connection_from(name, pnp_id);
  auto display_name =
      presented_display_name(name, manufacturer, pnp_id, connection);
  if (display_name.empty() || generic_display_name(display_name)) {
    return std::nullopt;
  }
  std::uint32_t width{};
  std::uint32_t height{};
  if (!pnp_entity_row) {
    auto const width_valid = parse_integer(row_value(row, 4), width) && width > 0;
    auto const height_valid =
        parse_integer(row_value(row, 5), height) && height > 0;
    if (width_valid && height_valid) {
      append_display_resolution(display_name, width, height);
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
      .display_connection = connection,
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
  auto const connection = display_connection_from(name, pnp_id);
  auto const display_name =
      presented_display_name(name, manufacturer, pnp_id, connection);
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
      .display_connection = connection,
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

[[nodiscard]] std::string storage_interface_from_text(
    std::string_view model, std::string_view interface_type,
    std::string_view pnp_id) {
  std::string source{model};
  source += " ";
  source += interface_type;
  source += " ";
  source += pnp_id;
  if (contains_ascii(source, "nvme")) {
    return "NVMe";
  }
  if (contains_ascii(source, "sata")) {
    return "SATA";
  }
  if (contains_ascii(source, "usb")) {
    return "USB";
  }
  if (contains_ascii(source, "sas")) {
    return "SAS";
  }
  if (contains_ascii(source, "scsi")) {
    return "SCSI";
  }
  if (contains_ascii(source, "ide") || contains_ascii(source, "ata")) {
    return "IDE/ATA";
  }
  return {};
}

[[nodiscard]] bool exact_pcb01_product(std::string_view model) noexcept {
  return lower_ascii(trim_ascii(model)) ==
         "sk hynix pcb01 hfs001tfm9x187n";
}

[[nodiscard]] std::string pcie_generation_from_text(
    std::string_view model, std::string_view manufacturer,
    std::string_view interface_type, std::string_view pnp_id) {
  // SK hynix identifies PCB01 as its PCIe Gen5 AI-PC SSD with SLC caching:
  // https://news.skhynix.com/en/sk-hynix-develops-pcb01-for-artificial-intelligence-pcs/
  // Keep the match product-specific; this fact must never leak to PC801 or a
  // similarly named but unverified PCB01 device.
  if (exact_pcb01_product(model)) {
    return "5.0 x4";
  }
  std::string source{model};
  source += " ";
  source += manufacturer;
  source += " ";
  source += interface_type;
  source += " ";
  source += pnp_id;
  for (int generation = 5; generation >= 3; --generation) {
    auto const number = std::to_string(generation);
    if (contains_ascii(source, "pcie " + number) ||
        contains_ascii(source, "pcie" + number) ||
        contains_ascii(source, "pcie gen " + number) ||
        contains_ascii(source, "pcie gen" + number) ||
        contains_ascii(source, "pci express " + number)) {
      return number + ".0";
    }
  }

  // These two model-specific facts are documented by the respective OEMs.
  // They are deliberately exact: nearby model families, including PCB01, do
  // not inherit PC801's metadata.
  if (contains_ascii(model, "samsung ssd 990 pro")) {
    return "4.0";
  }
  if (contains_ascii(model, "pc801") &&
      (contains_ascii(model, "hynix") ||
       contains_ascii(manufacturer, "hynix"))) {
    return "4.0";
  }
  return {};
}

[[nodiscard]] std::string nand_type_from_text(
    std::string_view model, std::string_view manufacturer,
    std::string_view media_type, std::string_view pnp_id) {
  // The 238-layer 4D TLC detail is cross-checked against model-specific
  // HFS001TFM9X187N/PCB01 1 TB identification and PCB01/P51 family data:
  // https://www.storagereview.com/review/hp-elitebook-x-g2i-review
  // https://www.storagereview.com/review/sk-hynix-platinum-p51-review-balanced-performance-for-demanding-workloads
  // https://www.techpowerup.com/ssd-specs/sk-hynix-pcb01-2-tb.d2057
  // These sources cross-check the NAND family only; no 2 TB capacity-specific
  // value is applied to this 1 TB model. SLC describes cache, not cell type.
  if (exact_pcb01_product(model)) {
    return "238 层 4D TLC（资料识别；SLC 缓存：支持）";
  }
  std::string source{model};
  source += " ";
  source += manufacturer;
  source += " ";
  source += media_type;
  source += " ";
  source += pnp_id;
  if (contains_ascii(source, "qlc")) {
    return "QLC";
  }
  if (contains_ascii(source, "tlc")) {
    return "TLC";
  }
  if (contains_ascii(source, "mlc")) {
    return "MLC";
  }
  if (contains_ascii(source, "slc")) {
    return "SLC";
  }
  if (contains_ascii(model, "samsung ssd 990 pro")) {
    return "TLC";
  }
  if (contains_ascii(model, "pc801") &&
      (contains_ascii(model, "hynix") ||
       contains_ascii(manufacturer, "hynix"))) {
    return "V7 176 层 4D NAND（单元类型未读取）";
  }
  return {};
}

[[nodiscard]] std::string storage_metadata_text(
    std::string_view storage_interface, std::string_view pcie_generation,
    std::string_view nand_type) {
  std::string result{"（接口："};
  result += (storage_interface.empty() ? "未读取" : storage_interface);
  result += "；PCIe 代际：";
  if (pcie_generation.empty()) {
    result += "未读取";
  } else {
    result += "PCIe ";
    result += pcie_generation;
  }
  result += "；NAND 颗粒：";
  result += (nand_type.empty() ? "未读取" : nand_type);
  result += "）";
  return result;
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
  auto const storage_interface =
      storage_interface_from_text(model, interface_type, pnp_id);
  auto const pcie_generation = pcie_generation_from_text(
      model, manufacturer, interface_type, pnp_id);
  auto const nand_type =
      nand_type_from_text(model, manufacturer, media_type, pnp_id);
  std::string name = localized_brand_model(model, manufacturer);
  name += " ";
  name += decimal_gigabytes(size);
  name += storage_metadata_text(storage_interface, pcie_generation, nand_type);
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
      .storage_media = media,
      .storage_interface = storage_interface,
      .pcie_generation = pcie_generation,
      .nand_type = nand_type,
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

void append_grouped_device(
    std::vector<application::HardwareDeviceRecord>& devices,
    application::HardwareDeviceRecord record) {
  // Identical DIMMs are one physical model with a quantity, not separate
  // rows. Their compact summary deliberately uses ASCII parentheses, so only
  // this memory-specific grouping is allowed to inspect that presentation.
  if (record.kind == application::HardwareDeviceKind::memory) {
    for (auto& existing : devices) {
      auto comparable_name = [](std::string const& value) {
        auto const marker = value.find(" (");
        return marker == std::string::npos ? value : value.substr(0, marker);
      };
      auto const same_memory_model =
          existing.kind == application::HardwareDeviceKind::memory &&
          comparable_name(existing.name) == comparable_name(record.name) &&
          ((existing.model_detail.empty() && record.model_detail.empty()) ||
           (!existing.model_detail.empty() &&
            existing.model_detail == record.model_detail));
      if (existing.kind == record.kind && same_memory_model &&
           existing.network_link == record.network_link &&
           existing.storage_media == record.storage_media &&
           existing.display_connection == record.display_connection) {
        existing.quantity += record.quantity;
        return;
      }
    }
  }
  // DesktopMonitor and PnPEntity project one panel through different WMI
  // classes.  Use the stable EDID/PNP model key and resolution helpers, not
  // punctuation in the localized presentation string, to collapse them.
  if (record.kind == application::HardwareDeviceKind::display) {
    for (auto& existing : devices) {
      if (existing.kind == record.kind &&
          ((!existing.model_detail.empty() &&
            existing.model_detail == record.model_detail) ||
           (existing.model_detail.empty() && record.model_detail.empty() &&
            existing.name == record.name)) &&
          existing.storage_media == record.storage_media &&
          existing.display_connection == record.display_connection) {
        if (!has_display_resolution(existing.name) &&
            has_display_resolution(record.name)) {
          append_display_resolution(existing.name, record.display_width,
                                    record.display_height);
          existing.display_width = record.display_width;
          existing.display_height = record.display_height;
        }
        existing.quantity += record.quantity;
        return;
      }
    }
  }
  devices.push_back(std::move(record));
}

[[nodiscard]] std::string grouped_name(
    application::HardwareDeviceRecord const& record) {
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
    std::optional<application::HardwareStorageMedia> media = std::nullopt) {
  target.clear();
  for (auto const& device : devices) {
    if (device.kind != kind ||
        (link.has_value() && device.network_link != *link) ||
        (media.has_value() && device.storage_media != *media)) {
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
  std::array<QuerySpec, 11> const specs{
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
  if (cancellation.stop_requested()) {
    collected.code = application::HardwareObservationCode::cancelled;
    collected.error = "hardware observation cancelled";
    return collected;
  }

  for (auto const& row : rows_by_spec[0]) {
    if (auto record = classify_cpu(row, virtual_host, cpu_topology)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[1]) {
    if (auto record = classify_gpu(
            row, virtual_host,
            std::span<WindowsGpuMemory const>{gpu_adapters})) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[2]) {
    if (auto record = classify_board(row, virtual_host)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[3]) {
    if (auto record = classify_network(row, virtual_host)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[5]) {
    if (auto record = classify_memory(row, virtual_host)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  // Prefer the EDID-derived Win32_PnPEntity projection. DesktopMonitor often
  // exposes the same physical panel as a generic name, so remember its model
  // token and skip that duplicate projection below.
  std::vector<std::string> pnp_display_keys;
  for (auto const& row : rows_by_spec[9]) {
    if (auto record = classify_display_pnp(row, virtual_host)) {
      pnp_display_keys.push_back(display_model_key(row_value(row, 2)));
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[6]) {
    auto const desktop_key = display_model_key(row_value(row, 1));
    if (auto record = classify_display(row, virtual_host)) {
      bool enriched_pnp_projection = false;
      if (!desktop_key.empty() &&
          std::ranges::any_of(pnp_display_keys, [&](auto const& pnp_key) {
            return pnp_key == desktop_key;
          })) {
        // Keep the concrete PnP model but borrow the resolution that is only
        // exposed by DesktopMonitor.  Repeated DesktopMonitor rows therefore
        // remain a single display record.
        for (auto& existing : collected.observation.devices) {
          if (existing.kind != application::HardwareDeviceKind::display ||
              existing.model_detail != desktop_key) {
            continue;
          }
          if (!has_display_resolution(existing.name) &&
              has_display_resolution(record->name)) {
            append_display_resolution(existing.name, record->display_width,
                                      record->display_height);
            existing.display_width = record->display_width;
            existing.display_height = record->display_height;
          }
          enriched_pnp_projection = true;
          break;
        }
      }
      if (!enriched_pnp_projection) {
        append_grouped_device(collected.observation.devices, std::move(*record));
      }
    }
  }
  for (auto& device : collected.observation.devices) {
    if (device.kind != application::HardwareDeviceKind::display) {
      continue;
    }
    auto const limit = physical_refresh_rate_limit_for(
        display_edids, device.model_detail);
    device.physical_refresh_rate_limit_hz = limit.value_or(0);
    append_display_refresh_rate_limit(device.name, limit);
  }
  for (auto const& row : rows_by_spec[7]) {
    if (auto record = classify_storage(row, virtual_host)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[8]) {
    if (auto record = classify_audio(row, virtual_host)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[9]) {
    if (auto record = classify_npu(row, virtual_host)) {
      append_grouped_device(collected.observation.devices, std::move(*record));
    }
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
  rebuild_summary(collected.observation.unclassified_storage,
                  collected.observation.devices,
                  application::HardwareDeviceKind::storage, std::nullopt,
                  application::HardwareStorageMedia::unknown);
  rebuild_summary(collected.observation.audio, collected.observation.devices,
                  application::HardwareDeviceKind::audio);
  rebuild_summary(collected.observation.npu, collected.observation.devices,
                  application::HardwareDeviceKind::npu);

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
    // Only exactly two observed classes are presented as P/E cores; more
    // complex topologies remain unlabelled.
    if (distinct_efficiency_classes == 2) {
      topology.performance_cores =
          cores_by_efficiency[performance_efficiency_class];
      topology.efficiency_cores = cores_by_efficiency[efficiency_efficiency_class];
      topology.split_known = topology.performance_cores != 0 &&
                             topology.efficiency_cores != 0;
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
        result.push_back(WindowsGpuMemory{
            .model_name = std::move(name),
            .dedicated_video_memory =
                static_cast<std::uint64_t>(description.DedicatedVideoMemory),
            .shared_system_memory =
                static_cast<std::uint64_t>(description.SharedSystemMemory),
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
    for (auto const& pnp_device_id : pnp_device_ids) {
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
          {.model_key = model_key, .bytes = std::move(bytes)});
    }
    return result;
  }

  [[nodiscard]] WindowsHardwareQueryResult query(
      std::string_view class_name,
      std::span<std::string_view const> properties,
      std::stop_token cancellation) override {
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
    auto const namespace_name = ::SysAllocString(L"ROOT\\CIMV2");
    if (namespace_name == nullptr) {
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "WMI namespace allocation failed"};
    }
    auto const connect_result = locator->ConnectServer(
        namespace_name, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
        services.GetAddressOf());
    ::SysFreeString(namespace_name);
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
