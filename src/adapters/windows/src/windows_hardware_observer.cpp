#include "azzs/adapters/windows/windows_hardware_observer.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
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
  return lower_ascii(value).find(needle) != std::string::npos;
}

[[nodiscard]] bool starts_with_ascii(std::string_view value,
                                     std::string_view prefix) {
  auto const lowered = lower_ascii(value);
  auto const lowered_prefix = lower_ascii(prefix);
  return lowered.size() >= lowered_prefix.size() &&
         lowered.compare(0, lowered_prefix.size(), lowered_prefix) == 0;
}

[[nodiscard]] std::string row_value(std::vector<std::string> const& row,
                                    std::size_t index) {
  return index < row.size() ? row[index] : std::string{};
}

[[nodiscard]] bool parse_bool(std::string_view value, bool& result) {
  auto const lowered = lower_ascii(value);
  if (lowered == "true" || lowered == "1" || lowered == "yes") {
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
  constexpr std::array<std::string_view, 12> markers{
      "hyper-v", "hyperv", "vmware", "virtualbox", "qemu", "kvm",
      "xen", "parallels", "bochs", "virtual machine", "hvm dom", "bhyve",
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
  return starts_with_ascii(lowered, "pci\\");
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
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id)) {
    return std::nullopt;
  }
  if (!starts_with_ascii(pnp_id, "acpi\\") &&
      !starts_with_ascii(pnp_id, "processor\\")) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::cpu,
      .name = name,
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "ACPI/processor PNP id on a non-virtual host",
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_gpu(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const compatibility = row_value(row, 1);
  auto const pnp_id = row_value(row, 2);
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !physical_bus(pnp_id, false)) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::gpu,
      .name = name,
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(compatibility.empty() ? name : compatibility),
      .physically_present = true,
      .filter_reason = "PCI PNP id on a non-virtual host",
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_board(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const manufacturer = row_value(row, 0);
  auto const product = row_value(row, 1);
  bool hosting_board = false;
  if (manufacturer.empty() || product.empty() || virtual_host ||
      !parse_bool(row_value(row, 2), hosting_board) || !hosting_board) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::motherboard,
      .name = manufacturer + " " + product,
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 3), row_value(row, 4)),
      .vendor = vendor_from_text(manufacturer),
      .physically_present = true,
      .filter_reason = "Win32_BaseBoard HostingBoard=true on a non-virtual host",
  };
}

[[nodiscard]] std::optional<application::HardwareDeviceRecord> classify_network(
    std::vector<std::string> const& row, bool virtual_host) {
  auto const name = row_value(row, 0);
  auto const manufacturer = row_value(row, 1);
  auto const adapter_type = row_value(row, 2);
  auto const physical_adapter = row_value(row, 3);
  auto const pnp_id = row_value(row, 4);
  auto const service = row_value(row, 7);
  bool is_physical = false;
  if (name.empty() || virtual_host || virtual_pnp_id(pnp_id) ||
      !parse_bool(physical_adapter, is_physical) || !is_physical ||
      !physical_bus(pnp_id, true) ||
      obvious_nonphysical_network(name, service, adapter_type)) {
    return std::nullopt;
  }
  return application::HardwareDeviceRecord{
      .kind = application::HardwareDeviceKind::network_adapter,
      .name = name,
      .physicality = application::HardwareDevicePhysicality::confirmed_physical,
      .source = application::HardwareObservationSource::wmi,
      .confidence = application::HardwareObservationConfidence::confirmed,
      .status = device_status(row_value(row, 5), row_value(row, 6)),
      .vendor = vendor_from_text(manufacturer.empty() ? name : manufacturer),
      .physically_present = true,
      .filter_reason = "PhysicalAdapter=true with PCI/USB/SD/ACPI PNP id",
  };
}

void append_unique_summary(std::string& target,
                           application::HardwareDeviceRecord const& record) {
  if (!target.empty()) {
    target.append("; ");
  }
  target.append(record.name);
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
  std::array<QuerySpec, 5> const specs{
      QuerySpec{"Win32_Processor",
                {"Name", "Manufacturer", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode"},
                5},
      QuerySpec{"Win32_VideoController",
                {"Name", "AdapterCompatibility", "PNPDeviceID", "Status",
                 "ConfigManagerErrorCode", "VideoProcessor"},
                6},
      QuerySpec{"Win32_BaseBoard",
                {"Manufacturer", "Product", "HostingBoard", "Status",
                 "ConfigManagerErrorCode"},
                5},
      QuerySpec{"Win32_NetworkAdapter",
                {"Name", "Manufacturer", "AdapterType", "PhysicalAdapter",
                 "PNPDeviceID", "Status", "ConfigManagerErrorCode",
                 "ServiceName", "NetConnectionStatus"},
                9},
      QuerySpec{"Win32_ComputerSystem", {"Manufacturer", "Model"}, 2},
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
      collected.observation.oem_model = pair_value(result);
      if (!result.rows.empty()) {
        collected.observation.oem_vendor =
            vendor_from_text(row_value(result.rows.front(), 0));
        virtual_host = virtualization_marker(pair_value(result));
      }
    } else {
      rows_by_spec[spec_index] = result.rows;
    }
  }

  for (auto const& row : rows_by_spec[0]) {
    if (auto record = classify_cpu(row, virtual_host)) {
      append_unique_summary(collected.observation.cpu, *record);
      collected.observation.devices.push_back(std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[1]) {
    if (auto record = classify_gpu(row, virtual_host)) {
      append_unique_summary(collected.observation.gpu, *record);
      collected.observation.devices.push_back(std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[2]) {
    if (auto record = classify_board(row, virtual_host)) {
      append_unique_summary(collected.observation.motherboard, *record);
      collected.observation.devices.push_back(std::move(*record));
    }
  }
  for (auto const& row : rows_by_spec[3]) {
    if (auto record = classify_network(row, virtual_host)) {
      append_unique_summary(collected.observation.network_adapter, *record);
      collected.observation.devices.push_back(std::move(*record));
    }
  }

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
