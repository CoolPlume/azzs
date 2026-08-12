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

namespace azzs::adapters::windows {
namespace {

using namespace application;
namespace settings_domain = application::settings_domain;

constexpr wchar_t kClassicMenuSubKey[] =
    L"Software\\Classes\\CLSID\\{86ca1aa0-34aa-4e8b-a509-50c905bae2a2}"
    L"\\InprocServer32";
constexpr wchar_t kExplorerLegacyOne[] =
    L"Software\\Classes\\CLSID\\{2aa9162e-c906-4dd9-ad0b-3d24a8eef5a0}"
    L"\\InprocServer32";
constexpr wchar_t kExplorerLegacyTwo[] =
    L"Software\\Classes\\CLSID\\{6480100b-5a83-4d1e-9f69-8ae5a88e9a33}"
    L"\\InprocServer32";

enum class FixedKeyState : std::uint8_t {
  absent,
  controlled_empty_default,
  unexpected,
};

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

}  // namespace

std::optional<settings_domain::WindowsVersion>
WindowsSystemSettingsAdapter::windows_version() const {
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
  return map_windows_version(version.dwMajorVersion, version.dwBuildNumber);
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
      auto const first = inspect_fixed_empty_key(kExplorerLegacyOne);
      auto const second = inspect_fixed_empty_key(kExplorerLegacyTwo);
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
      return ensure_fixed_empty_key(kExplorerLegacyOne) &&
                     ensure_fixed_empty_key(kExplorerLegacyTwo)
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
                 : delete_fixed_empty_key(kExplorerLegacyOne) &&
                           delete_fixed_empty_key(kExplorerLegacyTwo)
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
