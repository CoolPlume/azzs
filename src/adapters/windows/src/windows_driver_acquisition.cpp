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
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::adapters::windows {
namespace {

using application::driver_acquisition::DriverAssistantAction;
using application::driver_acquisition::DriverEntrypoint;
using application::driver_acquisition::RescueToolTarget;

struct HandleCloser final {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct GuardedRescueFolder final {
  std::wstring path;
  std::vector<UniqueHandle> directory_guards;
};

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

[[nodiscard]] wchar_t const* rescue_folder_name(
    RescueToolTarget target) noexcept {
  switch (target) {
    case RescueToolTarget::generic_network_driver:
      return L"generic-network-driver";
    case RescueToolTarget::offline_network_diagnostics:
      return L"offline-network-diagnostics";
  }
  return nullptr;
}

[[nodiscard]] std::optional<std::wstring> module_directory(
    std::string& error) {
  std::vector<wchar_t> buffer(512);
  for (;;) {
    auto const length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      error = "the workbench module location is unavailable";
      return std::nullopt;
    }
    if (length < buffer.size()) {
      std::wstring module{buffer.data(), length};
      auto const separator = module.find_last_of(L"\\/");
      if (separator == std::wstring::npos || separator == 0) {
        error = "the workbench module location has no safe parent directory";
        return std::nullopt;
      }
      return module.substr(0, separator);
    }
    if (buffer.size() >= 32768) {
      error = "the workbench module location exceeds the supported path length";
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2);
  }
}

[[nodiscard]] bool hold_plain_directory(
    std::filesystem::path const& path, std::vector<UniqueHandle>& guards,
    std::string& error) {
  UniqueHandle directory{::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (directory.get() == INVALID_HANDLE_VALUE) {
    error = "a fixed rescue folder ancestor could not be opened safely";
    return false;
  }

  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    error = "the fixed rescue folder contains an unsafe reparse directory";
    return false;
  }
  guards.push_back(std::move(directory));
  return true;
}

[[nodiscard]] bool hold_plain_directory_chain(
    std::filesystem::path const& directory, std::vector<UniqueHandle>& guards,
    std::string& error) {
  auto const native = directory.native();
  if (!directory.has_root_name() || !directory.has_root_directory() ||
      native.starts_with(L"\\\\")) {
    error = "the workbench module location is not a local absolute path";
    return false;
  }

  auto current = directory.root_path();
  if (!hold_plain_directory(current, guards, error)) {
    return false;
  }
  for (auto const& component : directory.relative_path()) {
    if (component == L"." || component == L"..") {
      error = "the workbench module location has an unsafe path component";
      return false;
    }
    current /= component;
    if (!hold_plain_directory(current, guards, error)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool ensure_plain_child_directory(
    std::filesystem::path const& directory, std::vector<UniqueHandle>& guards,
    std::string& error) {
  if (!::CreateDirectoryW(directory.c_str(), nullptr)) {
    auto const create_error = ::GetLastError();
    if (create_error != ERROR_ALREADY_EXISTS) {
      error = "the fixed rescue folder could not be created";
      return false;
    }
  }
  return hold_plain_directory(directory, guards, error);
}

[[nodiscard]] std::optional<GuardedRescueFolder> fixed_rescue_folder(
    RescueToolTarget target, std::string& error) {
  auto const name = rescue_folder_name(target);
  if (name == nullptr) {
    error = "the requested rescue folder is unavailable";
    return std::nullopt;
  }
  auto parent = module_directory(error);
  if (!parent.has_value()) {
    return std::nullopt;
  }

  GuardedRescueFolder result;
  auto const module = std::filesystem::path{*parent};
  if (!hold_plain_directory_chain(module, result.directory_guards, error)) {
    return std::nullopt;
  }
  auto const rescue_root = module / L"rescue-tools";
  if (!ensure_plain_child_directory(rescue_root, result.directory_guards,
                                    error)) {
    return std::nullopt;
  }
  auto const folder = rescue_root / name;
  if (!ensure_plain_child_directory(folder, result.directory_guards, error)) {
    return std::nullopt;
  }
  result.path = folder.wstring();
  return result;
}

class ShellRescueFolderExplorer final : public WindowsRescueFolderExplorer {
 public:
  [[nodiscard]] bool open_folder(std::wstring const& folder,
                                 std::string& error) override {
    auto const result = reinterpret_cast<std::intptr_t>(::ShellExecuteW(
        nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) {
      error = "Windows Explorer could not open the fixed rescue folder";
      return false;
    }
    return true;
  }
};

}  // namespace

WindowsDriverHandoffPlatform::WindowsDriverHandoffPlatform()
    : rescue_folder_explorer_(std::make_unique<ShellRescueFolderExplorer>()) {}

WindowsDriverHandoffPlatform::WindowsDriverHandoffPlatform(
    std::unique_ptr<WindowsRescueFolderExplorer> rescue_folder_explorer)
    : rescue_folder_explorer_(std::move(rescue_folder_explorer)) {}

WindowsDriverHandoffPlatform::~WindowsDriverHandoffPlatform() = default;

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

bool WindowsDriverHandoffPlatform::open_rescue_folder(RescueToolTarget target,
                                                       std::string& error) {
  if (!rescue_folder_explorer_) {
    error = "the fixed rescue folder explorer is unavailable";
    return false;
  }
  auto directory = fixed_rescue_folder(target, error);
  if (!directory.has_value()) {
    return false;
  }
  return rescue_folder_explorer_->open_folder(directory->path, error);
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
