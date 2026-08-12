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

domain::SystemArchitecture WindowsPlatformInfo::windows_architecture() const {
  SYSTEM_INFO system_info{};
  ::GetNativeSystemInfo(&system_info);
  switch (system_info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      return domain::SystemArchitecture::x64;
    case PROCESSOR_ARCHITECTURE_ARM64:
      return domain::SystemArchitecture::arm64;
    default:
      return domain::SystemArchitecture::unknown;
  }
}

}  // namespace azzs::adapters::windows
