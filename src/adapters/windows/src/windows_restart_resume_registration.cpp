#include "azzs/adapters/windows/windows_restart_resume_registration.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace azzs::adapters::windows {
namespace {

constexpr wchar_t k_run_once_key[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr wchar_t k_run_once_value[] = L"AzzsRestartResume";
constexpr wchar_t k_resume_token[] = L"--azzs-resume-after-restart";

[[nodiscard]] std::optional<std::wstring> workbench_module_path() {
  std::vector<wchar_t> buffer(512);
  for (;;) {
    auto const length = ::GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return std::nullopt;
    }
    // A complete path may use the final character slot; truncation is
    // reported by returning the buffer size.
    if (length < buffer.size()) {
      return std::wstring{buffer.data(), length};
    }
    if (buffer.size() >= 32768) {
      return std::nullopt;
    }
    buffer.resize(buffer.size() * 2);
  }
}

[[nodiscard]] application::restart_resume::LoginResumeRegistrationResult
failure(char const* detail) {
  return {.code = application::restart_resume::LoginResumeRegistrationCode::failed,
          .detail = detail};
}

}  // namespace

application::restart_resume::LoginResumeRegistrationResult
WindowsLoginResumeRegistration::register_once() {
  auto module_path = workbench_module_path();
  if (!module_path.has_value()) {
    return failure("the workbench executable path could not be determined");
  }
  std::wstring command{L"\""};
  command.append(*module_path);
  command.append(L"\" ");
  command.append(k_resume_token);

  HKEY key{};
  auto status = ::RegCreateKeyExW(
      HKEY_CURRENT_USER, k_run_once_key, 0, nullptr, REG_OPTION_NON_VOLATILE,
      KEY_SET_VALUE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) {
    return failure("the per-user login resume registration could not be opened");
  }
  auto const bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
  status = ::RegSetValueExW(
      key, k_run_once_value, 0, REG_SZ,
      reinterpret_cast<BYTE const*>(command.c_str()), bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    return failure("the per-user login resume registration could not be written");
  }
  return {.code = application::restart_resume::LoginResumeRegistrationCode::registered};
}

application::restart_resume::LoginResumeRegistrationResult
WindowsLoginResumeRegistration::clear_once() {
  HKEY key{};
  auto status = ::RegOpenKeyExW(HKEY_CURRENT_USER, k_run_once_key, 0,
                               KEY_SET_VALUE, &key);
  if (status == ERROR_FILE_NOT_FOUND) {
    return {.code = application::restart_resume::LoginResumeRegistrationCode::cleared};
  }
  if (status != ERROR_SUCCESS) {
    return failure("the login resume registration could not be opened");
  }
  status = ::RegDeleteValueW(key, k_run_once_value);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    return failure("the login resume registration could not be cleared");
  }
  return {.code = application::restart_resume::LoginResumeRegistrationCode::cleared};
}

bool is_restart_resume_login_launch() noexcept {
  auto const* command_line = ::GetCommandLineW();
  return command_line != nullptr &&
         std::wstring_view{command_line}.find(k_resume_token) !=
             std::wstring_view::npos;
}

}  // namespace azzs::adapters::windows
