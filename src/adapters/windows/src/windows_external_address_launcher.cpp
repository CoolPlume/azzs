#include "azzs/adapters/windows/windows_external_address_launcher.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace azzs::adapters::windows {
namespace {

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(
    std::string_view value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  auto const count = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) {
    return std::nullopt;
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            count) != count) {
    return std::nullopt;
  }
  return result;
}

}  // namespace

bool WindowsExternalAddressLauncher::open_declared_address(
    std::string_view software_id,
    domain::software_catalog::CatalogSource const& declared_source,
    std::string& error) {
  if (software_id.empty() || !declared_source.purpose.has_value() ||
      !domain::software_catalog::valid_http_address(declared_source.address)) {
    error = "external handoff requires a declared HTTP(S) catalog source";
    return false;
  }
  auto address = utf8_to_wide(declared_source.address);
  if (!address.has_value()) {
    error = "declared source address is not valid UTF-8";
    return false;
  }
  auto const result = reinterpret_cast<std::intptr_t>(
      ::ShellExecuteW(nullptr, L"open", address->c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL));
  if (result <= 32) {
    error = "system browser could not open the declared source";
    return false;
  }
  return true;
}

}  // namespace azzs::adapters::windows
