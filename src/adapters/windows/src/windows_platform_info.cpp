#include "azzs/adapters/windows/windows_platform_info.hpp"

#include <cstdint>
#include <string>

#include <winrt/Windows.System.Profile.h>
#include <winrt/base.h>

namespace azzs::adapters::windows {

std::optional<domain::SystemVersion> WindowsPlatformInfo::windows_version()
    const {
  try {
    auto const encoded_text = winrt::to_string(
        winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo()
            .DeviceFamilyVersion());
    auto const encoded = std::stoull(encoded_text);

    return domain::SystemVersion{
        .major = static_cast<std::uint32_t>((encoded >> 48) & 0xffff),
        .minor = static_cast<std::uint32_t>((encoded >> 32) & 0xffff),
        .build = static_cast<std::uint32_t>((encoded >> 16) & 0xffff),
    };
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace azzs::adapters::windows
