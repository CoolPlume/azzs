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

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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

[[nodiscard]] std::string first_value(
    WindowsHardwareQueryResult const& result) {
  for (auto const& row : result.rows) {
    if (!row.empty() && !row.front().empty()) {
      return row.front();
    }
  }
  return {};
}

[[nodiscard]] std::string join_values(
    WindowsHardwareQueryResult const& result) {
  std::string combined;
  for (auto const& row : result.rows) {
    if (row.empty() || row.front().empty()) {
      continue;
    }
    if (!combined.empty() && combined != row.front()) {
      combined.append("; ");
      combined.append(row.front());
    } else if (combined.empty()) {
      combined = row.front();
    }
  }
  return combined;
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
    std::array<std::string_view, 2> properties;
    std::size_t property_count;
  };
  std::array<QuerySpec, 5> const specs{
      QuerySpec{"Win32_Processor", {"Name", {}}, 1},
      QuerySpec{"Win32_VideoController", {"Name", {}}, 1},
      QuerySpec{"Win32_BaseBoard", {"Manufacturer", "Product"}, 2},
      QuerySpec{"Win32_NetworkAdapter", {"Name", {}}, 1},
      QuerySpec{"Win32_ComputerSystem", {"Manufacturer", "Model"}, 2},
  };

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
    switch (spec_index) {
      case 0:
        collected.observation.cpu = first_value(result);
        break;
      case 1:
        collected.observation.gpu = join_values(result);
        break;
      case 2:
        collected.observation.motherboard = pair_value(result);
        break;
      case 3:
        collected.observation.network_adapter = join_values(result);
        break;
      case 4:
        collected.observation.oem_model = pair_value(result);
        break;
      default:
        break;
    }
  }

  if (!collected.observation.usable()) {
    if (collected.code == application::HardwareObservationCode::succeeded) {
      collected.code = application::HardwareObservationCode::failed;
    }
    if (collected.error.empty()) {
      collected.error = "hardware observation returned no model facts";
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
          row.emplace_back();
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
