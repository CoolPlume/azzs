#include "azzs/adapters/windows/windows_driver_acquisition.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj_core.h>

#include <winrt/Windows.Networking.Connectivity.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace azzs::adapters::windows {
namespace {

using application::driver_acquisition::DriverAssistantAction;
using application::driver_acquisition::DriverEntrypoint;

constexpr wchar_t k_amd_install_url[] =
    L"https://www.amd.com/en/support/download/drivers.html";
constexpr wchar_t k_intel_assistant_url[] =
    L"https://www.intel.com/content/www/us/en/support/detect.html";
constexpr wchar_t k_nvidia_drivers_url[] = L"https://www.nvidia.com/drivers";
constexpr wchar_t k_dell_support_url[] = L"https://www.dell.com/support/home";
constexpr wchar_t k_hp_support_url[] = L"https://support.hp.com";
constexpr wchar_t k_lenovo_support_url[] = L"https://support.lenovo.com";
constexpr wchar_t k_asus_support_url[] = L"https://www.asus.com/support";

[[nodiscard]] std::optional<std::wstring> program_files_directory() {
  PWSTR raw_path{};
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT,
                                    nullptr, &raw_path)) ||
      raw_path == nullptr) {
    return std::nullopt;
  }
  std::wstring directory{raw_path};
  ::CoTaskMemFree(raw_path);
  if (directory.empty()) {
    return std::nullopt;
  }
  return directory;
}

[[nodiscard]] std::optional<std::wstring> amd_software_path() {
  auto directory = program_files_directory();
  if (!directory.has_value()) {
    return std::nullopt;
  }
  return *directory + L"\\AMD\\CNext\\CNext\\RadeonSoftware.exe";
}

[[nodiscard]] bool path_is_regular_file(std::wstring const& path) noexcept {
  auto const attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] bool shell_open(wchar_t const* target, std::string& error) {
  auto const result = reinterpret_cast<std::intptr_t>(
      ::ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL));
  if (result <= 32) {
    error = "system shell could not open the fixed driver entrypoint";
    return false;
  }
  return true;
}

[[nodiscard]] wchar_t const* fixed_url(DriverEntrypoint entrypoint) noexcept {
  switch (entrypoint) {
    case DriverEntrypoint::amd_software: return k_amd_install_url;
    case DriverEntrypoint::intel_driver_assistant: return k_intel_assistant_url;
    case DriverEntrypoint::nvidia_drivers: return k_nvidia_drivers_url;
    case DriverEntrypoint::dell_support: return k_dell_support_url;
    case DriverEntrypoint::hp_support: return k_hp_support_url;
    case DriverEntrypoint::lenovo_support: return k_lenovo_support_url;
    case DriverEntrypoint::asus_support: return k_asus_support_url;
  }
  return nullptr;
}

}  // namespace

bool WindowsDriverHandoffPlatform::assistant_installed() const noexcept {
  auto const path = amd_software_path();
  return path.has_value() && path_is_regular_file(*path);
}

bool WindowsDriverHandoffPlatform::open(DriverEntrypoint entrypoint,
                                        DriverAssistantAction action,
                                        std::string& error) {
  if (entrypoint == DriverEntrypoint::amd_software &&
      action == DriverAssistantAction::launch) {
    auto const path = amd_software_path();
    if (!path.has_value() || !path_is_regular_file(*path)) {
      error = "AMD Software is not installed at the controlled platform path";
      return false;
    }
    return shell_open(path->c_str(), error);
  }
  if (entrypoint == DriverEntrypoint::amd_software &&
      action == DriverAssistantAction::install) {
    return shell_open(k_amd_install_url, error);
  }
  if (action != DriverAssistantAction::open_page) {
    error = "the selected fixed driver entrypoint does not support this action";
    return false;
  }
  auto const target = fixed_url(entrypoint);
  if (target == nullptr) {
    error = "the selected fixed driver entrypoint is unavailable";
    return false;
  }
  return shell_open(target, error);
}

bool WindowsDriverNetworkObserver::available() const noexcept {
  try {
    auto const profile =
        winrt::Windows::Networking::Connectivity::NetworkInformation::
            GetInternetConnectionProfile();
    return profile && profile.GetNetworkConnectivityLevel() ==
                          winrt::Windows::Networking::Connectivity::
                              NetworkConnectivityLevel::InternetAccess;
  } catch (...) {
    return false;
  }
}

}  // namespace azzs::adapters::windows
