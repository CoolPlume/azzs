#include "azzs/adapters/windows/windows_platform_info.hpp"

#include <cstdint>

#include <windows.h>
#include <winternl.h>

namespace azzs::adapters::windows {

namespace {

using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

}  // namespace

std::optional<domain::SystemVersion> WindowsPlatformInfo::windows_version()
    const {
  auto const ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return std::nullopt;
  }

  auto const rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(
      ::GetProcAddress(ntdll, "RtlGetVersion"));
  if (rtl_get_version == nullptr) {
    return std::nullopt;
  }

  RTL_OSVERSIONINFOW version{};
  version.dwOSVersionInfoSize = static_cast<ULONG>(sizeof(version));
  if (rtl_get_version(&version) < 0) {
    return std::nullopt;
  }

  return domain::SystemVersion{
      .major = static_cast<std::uint32_t>(version.dwMajorVersion),
      .minor = static_cast<std::uint32_t>(version.dwMinorVersion),
      .build = static_cast<std::uint32_t>(version.dwBuildNumber),
  };
}

}  // namespace azzs::adapters::windows
