#include "azzs/adapters/windows/windows_system_settings_adapter.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <TlHelp32.h>
#include <winternl.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::adapters::windows {
namespace {

using namespace application;
namespace settings_domain = application::settings_domain;

constexpr wchar_t kClassicMenuSubKey[] =
    L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}"
    L"\\InprocServer32";
constexpr wchar_t kExplorerLegacyParentOne[] =
    L"Software\\Classes\\CLSID\\{2aa9162e-c906-4dd9-ad0b-3d24a8eef5a0}";
constexpr wchar_t kExplorerLegacyParentTwo[] =
    L"Software\\Classes\\CLSID\\{6480100b-5a83-4d1e-9f69-8ae5a88e9a33}";
constexpr wchar_t kExplorerLegacyParentDefaultOne[] =
    L"CLSID_ItemsViewAdapter";
constexpr wchar_t kExplorerLegacyParentDefaultTwo[] =
    L"File Explorer Xaml Island View Adapter";
constexpr wchar_t kExplorerLegacyInprocDefault[] =
    L"C:\\Windows\\System32\\Windows.UI.FileExplorer.dll_";
constexpr wchar_t kExplorerLegacyThreadingModel[] = L"Apartment";

enum class FixedKeyState : std::uint8_t {
  absent,
  controlled_empty_default,
  unexpected,
};

[[nodiscard]] bool delete_key_tree(wchar_t const* subkey);

[[nodiscard]] SystemSettingsRead read_failure(
    SystemSettingsAdapterStatus status, std::string detail) {
  return {.status = status, .detail = std::move(detail)};
}

[[nodiscard]] SystemSettingsAdapterResult result(
    SystemSettingsAdapterStatus status, std::string detail = {}) {
  return {.status = status, .detail = std::move(detail)};
}

[[nodiscard]] FixedKeyState inspect_fixed_empty_key(wchar_t const* subkey) {
  HKEY key{};
  auto const status =
      ::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &key);
  if (status == ERROR_FILE_NOT_FOUND) {
    return FixedKeyState::absent;
  }
  if (status != ERROR_SUCCESS) {
    return FixedKeyState::unexpected;
  }
  DWORD subkey_count{};
  DWORD value_count{};
  auto const metadata_status = ::RegQueryInfoKeyW(
      key, nullptr, nullptr, nullptr, &subkey_count, nullptr, nullptr,
      &value_count, nullptr, nullptr, nullptr, nullptr);
  if (metadata_status != ERROR_SUCCESS || subkey_count != 0 ||
      value_count != 1) {
    ::RegCloseKey(key);
    return FixedKeyState::unexpected;
  }
  DWORD type{};
  DWORD size{};
  auto const query_status =
      ::RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &size);
  if (query_status != ERROR_SUCCESS || type != REG_SZ ||
      size != sizeof(wchar_t)) {
    ::RegCloseKey(key);
    return FixedKeyState::unexpected;
  }
  wchar_t value{};
  auto const read_status = ::RegQueryValueExW(
      key, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size);
  ::RegCloseKey(key);
  return read_status == ERROR_SUCCESS && value == L'\0'
             ? FixedKeyState::controlled_empty_default
             : FixedKeyState::unexpected;
}

[[nodiscard]] bool ensure_fixed_empty_key(wchar_t const* subkey) {
  auto const existing = inspect_fixed_empty_key(subkey);
  if (existing == FixedKeyState::controlled_empty_default) {
    return true;
  }
  if (existing == FixedKeyState::unexpected) {
    return false;
  }
  HKEY key{};
  auto const status = ::RegCreateKeyExW(
      HKEY_CURRENT_USER, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  constexpr wchar_t kEmptyValue[] = L"";
  auto const write_status = ::RegSetValueExW(
      key, nullptr, 0, REG_SZ,
      reinterpret_cast<BYTE const*>(kEmptyValue), sizeof(kEmptyValue));
  ::RegCloseKey(key);
  return write_status == ERROR_SUCCESS;
}

[[nodiscard]] bool read_reg_sz(HKEY key, wchar_t const* value_name,
                               std::wstring& value) {
  DWORD type{};
  DWORD size{};
  auto status =
      ::RegQueryValueExW(key, value_name, nullptr, &type, nullptr, &size);
  if (status != ERROR_SUCCESS || type != REG_SZ || size < sizeof(wchar_t) ||
      size % sizeof(wchar_t) != 0) {
    return false;
  }
  std::vector<wchar_t> buffer(size / sizeof(wchar_t));
  status = ::RegQueryValueExW(key, value_name, nullptr, &type,
                              reinterpret_cast<BYTE*>(buffer.data()), &size);
  if (status != ERROR_SUCCESS || type != REG_SZ || buffer.back() != L'\0') {
    return false;
  }
  value.assign(buffer.data(), buffer.size() - 1);
  return true;
}

[[nodiscard]] FixedKeyState inspect_legacy_explorer_key(
    wchar_t const* parent_subkey, wchar_t const* parent_default) {
  HKEY parent{};
  auto status =
      ::RegOpenKeyExW(HKEY_CURRENT_USER, parent_subkey, 0, KEY_READ, &parent);
  if (status == ERROR_FILE_NOT_FOUND) {
    return FixedKeyState::absent;
  }
  if (status != ERROR_SUCCESS) {
    return FixedKeyState::unexpected;
  }

  DWORD subkey_count{};
  DWORD value_count{};
  status = ::RegQueryInfoKeyW(parent, nullptr, nullptr, nullptr, &subkey_count,
                              nullptr, nullptr, &value_count, nullptr, nullptr,
                              nullptr, nullptr);
  std::wstring parent_value;
  auto const parent_valid =
      status == ERROR_SUCCESS && subkey_count == 1 && value_count == 1 &&
      read_reg_sz(parent, nullptr, parent_value) &&
      parent_value == parent_default;
  HKEY inproc{};
  if (parent_valid) {
    status = ::RegOpenKeyExW(parent, L"InprocServer32", 0, KEY_READ, &inproc);
  }
  if (!parent_valid || status != ERROR_SUCCESS) {
    if (parent_valid && status == ERROR_FILE_NOT_FOUND) {
      ::RegCloseKey(parent);
      return FixedKeyState::unexpected;
    }
    ::RegCloseKey(parent);
    return FixedKeyState::unexpected;
  }

  DWORD inproc_subkey_count{};
  DWORD inproc_value_count{};
  status = ::RegQueryInfoKeyW(
      inproc, nullptr, nullptr, nullptr, &inproc_subkey_count, nullptr,
      nullptr, &inproc_value_count, nullptr, nullptr, nullptr, nullptr);
  std::wstring inproc_value;
  std::wstring threading_model;
  auto const controlled =
      status == ERROR_SUCCESS && inproc_subkey_count == 0 &&
      inproc_value_count == 2 && read_reg_sz(inproc, nullptr, inproc_value) &&
      read_reg_sz(inproc, L"ThreadingModel", threading_model) &&
      inproc_value == kExplorerLegacyInprocDefault &&
      threading_model == kExplorerLegacyThreadingModel;
  ::RegCloseKey(inproc);
  ::RegCloseKey(parent);
  return controlled ? FixedKeyState::controlled_empty_default
                    : FixedKeyState::unexpected;
}

[[nodiscard]] bool ensure_legacy_explorer_key(wchar_t const* parent_subkey,
                                               wchar_t const* parent_default) {
  auto const existing =
      inspect_legacy_explorer_key(parent_subkey, parent_default);
  if (existing == FixedKeyState::controlled_empty_default) {
    return true;
  }
  if (existing == FixedKeyState::unexpected) {
    return false;
  }

  HKEY parent{};
  auto status = ::RegCreateKeyExW(
      HKEY_CURRENT_USER, parent_subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_WRITE, nullptr, &parent, nullptr);
  if (status != ERROR_SUCCESS) {
    return false;
  }
  auto const parent_bytes = static_cast<DWORD>(
      (std::wstring_view{parent_default}.size() + 1) * sizeof(wchar_t));
  status = ::RegSetValueExW(
      parent, nullptr, 0, REG_SZ,
      reinterpret_cast<BYTE const*>(parent_default), parent_bytes);
  HKEY inproc{};
  if (status == ERROR_SUCCESS) {
    status = ::RegCreateKeyExW(parent, L"InprocServer32", 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                               &inproc, nullptr);
  }
  if (status == ERROR_SUCCESS) {
    auto const inproc_bytes = static_cast<DWORD>(
        (std::wstring_view{kExplorerLegacyInprocDefault}.size() + 1) *
        sizeof(wchar_t));
    status = ::RegSetValueExW(
        inproc, nullptr, 0, REG_SZ,
        reinterpret_cast<BYTE const*>(kExplorerLegacyInprocDefault),
        inproc_bytes);
  }
  if (status == ERROR_SUCCESS) {
    auto const threading_bytes = static_cast<DWORD>(
        (std::wstring_view{kExplorerLegacyThreadingModel}.size() + 1) *
        sizeof(wchar_t));
    status = ::RegSetValueExW(
        inproc, L"ThreadingModel", 0, REG_SZ,
        reinterpret_cast<BYTE const*>(kExplorerLegacyThreadingModel),
        threading_bytes);
  }
  if (inproc != nullptr) {
    ::RegCloseKey(inproc);
  }
  ::RegCloseKey(parent);
  if (status != ERROR_SUCCESS) {
    static_cast<void>(delete_key_tree(parent_subkey));
    return false;
  }
  return true;
}

[[nodiscard]] bool delete_legacy_explorer_key(wchar_t const* parent_subkey,
                                               wchar_t const* parent_default) {
  auto const existing =
      inspect_legacy_explorer_key(parent_subkey, parent_default);
  if (existing == FixedKeyState::absent) {
    return true;
  }
  return existing == FixedKeyState::controlled_empty_default &&
         delete_key_tree(parent_subkey);
}

[[nodiscard]] bool delete_key_tree(wchar_t const* subkey) {
  auto const status = ::RegDeleteTreeW(HKEY_CURRENT_USER, subkey);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

[[nodiscard]] bool delete_fixed_empty_key(wchar_t const* subkey) {
  auto const existing = inspect_fixed_empty_key(subkey);
  if (existing == FixedKeyState::absent) {
    return true;
  }
  return existing == FixedKeyState::controlled_empty_default &&
         delete_key_tree(subkey);
}

[[nodiscard]] bool terminate_explorer_processes() {
  auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return false;
  }
  bool terminated = true;
  PROCESSENTRY32W entry{.dwSize = sizeof(entry)};
  if (!::Process32FirstW(snapshot, &entry)) {
    auto const error = ::GetLastError();
    ::CloseHandle(snapshot);
    return error == ERROR_NO_MORE_FILES;
  }
  do {
    if (std::wstring_view{entry.szExeFile} == L"explorer.exe") {
      auto process =
          ::OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                        entry.th32ProcessID);
      if (process == nullptr ||
          !::TerminateProcess(process, 0) ||
          ::WaitForSingleObject(process, 5'000) != WAIT_OBJECT_0) {
        terminated = false;
      }
      if (process != nullptr) {
        ::CloseHandle(process);
      }
    }
  } while (::Process32NextW(snapshot, &entry));
  ::CloseHandle(snapshot);
  return terminated;
}

[[nodiscard]] bool start_explorer() {
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  wchar_t command[] = L"explorer.exe";
  auto const started = ::CreateProcessW(nullptr, command, nullptr, nullptr,
                                        FALSE, 0, nullptr, nullptr, &startup,
                                        &process);
  if (!started) {
    return false;
  }
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return true;
}

[[nodiscard]] std::optional<settings_domain::WindowsVersion>
map_windows_version(DWORD major, DWORD build) {
  if (major != 10) {
    return std::nullopt;
  }
  if (build >= 26'200 && build < 27'000) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_11,
        .feature_update_year = 25,
        .feature_update_half = 2,
    };
  }
  if (build >= 26'100 && build < 26'200) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_11,
        .feature_update_year = 24,
        .feature_update_half = 2,
    };
  }
  if (build >= 22'631 && build < 26'100) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_11,
        .feature_update_year = 23,
        .feature_update_half = 2,
    };
  }
  if (build >= 22'621 && build < 22'631) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_11,
        .feature_update_year = 22,
        .feature_update_half = 2,
    };
  }
  if (build >= 22'000 && build < 22'621) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_11,
        .feature_update_year = 21,
        .feature_update_half = 2,
    };
  }
  if (build >= 19'045 && build < 22'000) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_10,
        .feature_update_year = 22,
        .feature_update_half = 2,
    };
  }
  if (build >= 19'044 && build < 19'045) {
    return settings_domain::WindowsVersion{
        .generation = settings_domain::WindowsGeneration::windows_10,
        .feature_update_year = 21,
        .feature_update_half = 2,
    };
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RTL_OSVERSIONINFOW> read_native_windows_version() {
  RTL_OSVERSIONINFOW version{.dwOSVersionInfoSize = sizeof(version)};
  auto const ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return std::nullopt;
  }
  using RtlGetVersion = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  auto const get_version = reinterpret_cast<RtlGetVersion>(
      ::GetProcAddress(ntdll, "RtlGetVersion"));
  if (get_version == nullptr || get_version(&version) < 0) {
    return std::nullopt;
  }
  return version;
}

[[nodiscard]] std::string display_version(
    settings_domain::WindowsVersion const& version) {
  return "Windows " + std::to_string(static_cast<unsigned>(version.generation)) +
         " " + std::to_string(version.feature_update_year) + "H" +
         std::to_string(version.feature_update_half);
}

}  // namespace

std::optional<settings_domain::WindowsVersion>
WindowsSystemSettingsAdapter::windows_version() const {
  auto const version = read_native_windows_version();
  if (!version.has_value()) {
    return std::nullopt;
  }
  return map_windows_version(version->dwMajorVersion, version->dwBuildNumber);
}

std::optional<SystemSettingsWindowsVersionFact>
WindowsSystemSettingsAdapter::windows_version_fact() const {
  auto const native_version = read_native_windows_version();
  if (!native_version.has_value()) {
    return std::nullopt;
  }
  auto const mapped = map_windows_version(native_version->dwMajorVersion,
                                          native_version->dwBuildNumber);
  if (!mapped.has_value()) {
    return std::nullopt;
  }
  return SystemSettingsWindowsVersionFact{
      .display_version = display_version(*mapped),
      .internal_build = native_version->dwBuildNumber,
  };
}

SystemSettingsRead WindowsSystemSettingsAdapter::read(
    ControlledSystemSetting setting) {
  switch (setting) {
    case ControlledSystemSetting::classic_context_menu:
      switch (inspect_fixed_empty_key(kClassicMenuSubKey)) {
        case FixedKeyState::controlled_empty_default:
          return {.status = SystemSettingsAdapterStatus::succeeded,
                  .value = ClassicContextMenuMode::classic};
        case FixedKeyState::absent:
          return {.status = SystemSettingsAdapterStatus::succeeded,
                  .value = ClassicContextMenuMode::windows11};
        case FixedKeyState::unexpected:
          return read_failure(
              SystemSettingsAdapterStatus::read_failed,
              "经典右键菜单受控键包含非工作台管理的值");
      }
      break;
    case ControlledSystemSetting::windows10_explorer:
      auto const first = inspect_legacy_explorer_key(
          kExplorerLegacyParentOne, kExplorerLegacyParentDefaultOne);
      auto const second = inspect_legacy_explorer_key(
          kExplorerLegacyParentTwo, kExplorerLegacyParentDefaultTwo);
      if (first == FixedKeyState::unexpected ||
          second == FixedKeyState::unexpected) {
        return read_failure(
            SystemSettingsAdapterStatus::read_failed,
            "资源管理器受控键包含非工作台管理的值");
      }
      if (first == FixedKeyState::controlled_empty_default &&
          second == FixedKeyState::controlled_empty_default) {
        return {.status = SystemSettingsAdapterStatus::succeeded,
                .value = ExplorerPresentationMode::windows10};
      }
      if (first == FixedKeyState::absent && second == FixedKeyState::absent) {
        return {.status = SystemSettingsAdapterStatus::succeeded,
                .value = ExplorerPresentationMode::windows11};
      }
      return read_failure(SystemSettingsAdapterStatus::read_failed,
                          "资源管理器固定映射不完整");
  }
  return read_failure(SystemSettingsAdapterStatus::unsupported,
                      "未知的受控系统设置");
}

SystemSettingsAdapterResult WindowsSystemSettingsAdapter::apply(
    ControlledSystemSetting setting) {
  switch (setting) {
    case ControlledSystemSetting::classic_context_menu:
      return ensure_fixed_empty_key(kClassicMenuSubKey)
                 ? result(SystemSettingsAdapterStatus::succeeded)
                 : result(SystemSettingsAdapterStatus::apply_failed,
                          "创建经典右键菜单受控键失败");
    case ControlledSystemSetting::windows10_explorer:
      return ensure_legacy_explorer_key(kExplorerLegacyParentOne,
                                        kExplorerLegacyParentDefaultOne) &&
                     ensure_legacy_explorer_key(kExplorerLegacyParentTwo,
                                                kExplorerLegacyParentDefaultTwo)
                 ? result(SystemSettingsAdapterStatus::succeeded)
                 : result(SystemSettingsAdapterStatus::apply_failed,
                          "应用 Windows 10 风格资源管理器固定映射失败");
  }
  return result(SystemSettingsAdapterStatus::unsupported, "未知的受控系统设置");
}

SystemSettingsAdapterResult WindowsSystemSettingsAdapter::restore(
    ControlledSystemSetting setting, WindowsSystemSettingValue value) {
  switch (setting) {
    case ControlledSystemSetting::classic_context_menu: {
      auto const* mode = std::get_if<ClassicContextMenuMode>(&value);
      if (mode == nullptr) {
        return result(SystemSettingsAdapterStatus::restore_failed,
                      "经典右键菜单恢复数据类型不匹配");
      }
      return *mode == ClassicContextMenuMode::classic
                 ? ensure_fixed_empty_key(kClassicMenuSubKey)
                       ? result(SystemSettingsAdapterStatus::succeeded)
                       : result(SystemSettingsAdapterStatus::restore_failed)
                 : delete_fixed_empty_key(kClassicMenuSubKey)
                       ? result(SystemSettingsAdapterStatus::succeeded)
                       : result(SystemSettingsAdapterStatus::restore_failed);
    }
    case ControlledSystemSetting::windows10_explorer: {
      auto const* mode = std::get_if<ExplorerPresentationMode>(&value);
      if (mode == nullptr) {
        return result(SystemSettingsAdapterStatus::restore_failed,
                      "资源管理器恢复数据类型不匹配");
      }
      return *mode == ExplorerPresentationMode::windows10
                 ? apply(setting)
                 : delete_legacy_explorer_key(kExplorerLegacyParentOne,
                                               kExplorerLegacyParentDefaultOne) &&
                           delete_legacy_explorer_key(
                               kExplorerLegacyParentTwo,
                               kExplorerLegacyParentDefaultTwo)
                       ? result(SystemSettingsAdapterStatus::succeeded)
                       : result(SystemSettingsAdapterStatus::restore_failed);
    }
  }
  return result(SystemSettingsAdapterStatus::unsupported, "未知的受控系统设置");
}

SystemSettingsAdapterResult
WindowsSystemSettingsAdapter::restart_explorer() {
  if (!terminate_explorer_processes() || !start_explorer()) {
    return result(SystemSettingsAdapterStatus::restart_failed,
                  "资源管理器重启失败");
  }
  return result(SystemSettingsAdapterStatus::succeeded);
}

}  // namespace azzs::adapters::windows
