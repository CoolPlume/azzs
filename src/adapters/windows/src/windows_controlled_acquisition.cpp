#include "azzs/adapters/windows/windows_controlled_acquisition.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace azzs::adapters::windows {
namespace {

namespace cache = application::offline_package_cache;
namespace selection = application::software_selection;
namespace catalog = domain::software_catalog;

constexpr std::uint64_t kMaximumResponseBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr DWORD kTimeoutMilliseconds = 30'000;
constexpr wchar_t kUninstallRegistrySubkey[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

[[nodiscard]] HKEY registry_root(WindowsPresenceRegistryHive hive) noexcept {
  return hive == WindowsPresenceRegistryHive::current_user ? HKEY_CURRENT_USER
                                                            : HKEY_LOCAL_MACHINE;
}

[[nodiscard]] REGSAM registry_view(WindowsPresenceRegistryView view) noexcept {
  return view == WindowsPresenceRegistryView::view_32 ? KEY_WOW64_32KEY
                                                       : KEY_WOW64_64KEY;
}

[[nodiscard]] std::string registry_error_detail(char const* operation,
                                                LONG status) {
  return std::string{operation} + " failed with Win32 error " +
         std::to_string(status);
}

class WindowsRegistryPresenceQuery final : public WindowsPresenceRegistryQuery {
 public:
  [[nodiscard]] WindowsPresenceRegistryRead read(
      WindowsPresenceRegistryHive hive,
      WindowsPresenceRegistryView view) const override {
    HKEY uninstall{};
    auto const open_status = ::RegOpenKeyExW(
        registry_root(hive), kUninstallRegistrySubkey, 0,
        KEY_READ | registry_view(view), &uninstall);
    if (open_status == ERROR_FILE_NOT_FOUND) {
      return {.status = WindowsPresenceRegistryReadStatus::root_absent,
              .detail = "uninstall registry root is absent"};
    }
    if (open_status != ERROR_SUCCESS || uninstall == nullptr) {
      return {.detail = registry_error_detail("opening uninstall registry root",
                                              open_status)};
    }

    DWORD subkey_count{};
    DWORD maximum_subkey_length{};
    auto status = ::RegQueryInfoKeyW(
        uninstall, nullptr, nullptr, nullptr, &subkey_count,
        &maximum_subkey_length, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr);
    if (status != ERROR_SUCCESS) {
      ::RegCloseKey(uninstall);
      return {.detail = registry_error_detail("enumerating uninstall registry root",
                                              status)};
    }

    std::vector<WindowsPresenceRegistryEntry> entries;
    entries.reserve(subkey_count);
    std::vector<wchar_t> subkey_name(
        static_cast<std::size_t>(maximum_subkey_length) + 1U, L'\0');
    for (DWORD index = 0; index < subkey_count; ++index) {
      DWORD name_length = maximum_subkey_length;
      FILETIME last_write{};
      status = ::RegEnumKeyExW(uninstall, index, subkey_name.data(),
                               &name_length, nullptr, nullptr, nullptr,
                               &last_write);
      if (status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (status != ERROR_SUCCESS || name_length == 0) {
        ::RegCloseKey(uninstall);
        return {.detail = registry_error_detail("enumerating uninstall entry",
                                                status)};
      }

      HKEY entry_key{};
      status = ::RegOpenKeyExW(uninstall, subkey_name.data(), 0, KEY_READ,
                               &entry_key);
      if (status == ERROR_FILE_NOT_FOUND) {
        // An uninstall entry can disappear while the root is being read. It
        // is not evidence that the target is absent, so keep the root usable.
        continue;
      }
      if (status != ERROR_SUCCESS || entry_key == nullptr) {
        ::RegCloseKey(uninstall);
        return {.detail = registry_error_detail("opening uninstall entry",
                                                status)};
      }

      DWORD value_type{};
      DWORD value_bytes{};
      status = ::RegQueryValueExW(entry_key, L"DisplayName", nullptr,
                                  &value_type, nullptr, &value_bytes);
      if (status == ERROR_FILE_NOT_FOUND) {
        ::RegCloseKey(entry_key);
        continue;
      }
      if (status != ERROR_SUCCESS ||
          (value_type != REG_SZ && value_type != REG_EXPAND_SZ) ||
          value_bytes < sizeof(wchar_t) ||
          value_bytes % sizeof(wchar_t) != 0) {
        ::RegCloseKey(entry_key);
        ::RegCloseKey(uninstall);
        return {.detail = registry_error_detail("reading uninstall DisplayName",
                                                status)};
      }

      std::vector<wchar_t> value(value_bytes / sizeof(wchar_t), L'\0');
      status = ::RegQueryValueExW(
          entry_key, L"DisplayName", nullptr, &value_type,
          reinterpret_cast<BYTE*>(value.data()), &value_bytes);
      if (status != ERROR_SUCCESS ||
          (value_type != REG_SZ && value_type != REG_EXPAND_SZ) ||
          value.empty() || value.back() != L'\0' || value.front() == L'\0') {
        ::RegCloseKey(entry_key);
        ::RegCloseKey(uninstall);
        return {.detail = registry_error_detail("reading uninstall DisplayName",
                                                status)};
      }
      std::wstring display_version;
      DWORD version_type{};
      DWORD version_bytes{};
      auto const version_probe = ::RegQueryValueExW(
          entry_key, L"DisplayVersion", nullptr, &version_type, nullptr,
          &version_bytes);
      if (version_probe == ERROR_SUCCESS) {
        if ((version_type != REG_SZ && version_type != REG_EXPAND_SZ) ||
            version_bytes < sizeof(wchar_t) ||
            version_bytes % sizeof(wchar_t) != 0) {
          ::RegCloseKey(entry_key);
          ::RegCloseKey(uninstall);
          return {.detail = registry_error_detail("reading uninstall DisplayVersion",
                                                  version_probe)};
        }
        std::vector<wchar_t> version(version_bytes / sizeof(wchar_t), L'\0');
        auto const version_read = ::RegQueryValueExW(
            entry_key, L"DisplayVersion", nullptr, &version_type,
            reinterpret_cast<BYTE*>(version.data()), &version_bytes);
        if (version_read != ERROR_SUCCESS || version.empty() ||
            version.back() != L'\0' || version.front() == L'\0') {
          ::RegCloseKey(entry_key);
          ::RegCloseKey(uninstall);
          return {.detail = registry_error_detail("reading uninstall DisplayVersion",
                                                  version_read)};
        }
        display_version.assign(version.data(), version.size() - 1U);
      } else if (version_probe != ERROR_FILE_NOT_FOUND) {
        ::RegCloseKey(entry_key);
        ::RegCloseKey(uninstall);
        return {.detail = registry_error_detail("probing uninstall DisplayVersion",
                                                version_probe)};
      }
      ::RegCloseKey(entry_key);
      entries.push_back({.display_name = std::wstring{value.data(), value.size() - 1U},
                         .display_version = std::move(display_version)});
    }
    ::RegCloseKey(uninstall);
    return {.status = WindowsPresenceRegistryReadStatus::succeeded,
            .entries = std::move(entries),
            .detail = "uninstall registry root read successfully"};
  }
};

[[nodiscard]] bool same_display_name(std::wstring_view left,
                                     std::wstring_view right) noexcept {
  if (left.empty() || right.empty() ||
      left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  return ::CompareStringOrdinal(
             left.data(), static_cast<int>(left.size()), right.data(),
             static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool same_display_version(std::wstring_view left,
                                        std::wstring_view right) noexcept {
  if (left.empty() || right.empty() ||
      left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return false;
  }
  // DisplayVersion is a fixed baseline, not a friendly display label. Keep
  // the comparison ordinal and case-sensitive so a producer changing the
  // spelling cannot be mistaken for the reviewed version.
  return ::CompareStringOrdinal(
             left.data(), static_cast<int>(left.size()), right.data(),
             static_cast<int>(right.size()), FALSE) == CSTR_EQUAL;
}

[[nodiscard]] bool registration_matches(
    WindowsPresenceRegistration const& registration,
    WindowsPresenceRegistryEntry const& entry) noexcept {
  return std::ranges::any_of(registration.display_names,
                             [&](std::wstring const& display_name) {
                               return same_display_name(display_name,
                                                        entry.display_name);
                             });
}

[[nodiscard]] std::vector<WindowsPresenceRegistration>
canonical_presence_registrations() {
  return {
      {.software_id = "qq", .display_names = {L"QQ", L"腾讯QQ"}},
      {.software_id = "sogou-input", .display_names = {L"搜狗输入法"}},
      {.software_id = "game-cheats-manager",
       .display_names = {L"Game Cheats Manager"}},
      {.software_id = "cheat-engine", .display_names = {L"Cheat Engine"}},
      {.software_id = "office-tool-plus",
       .display_names = {L"Office Tool Plus"}},
      {.software_id = "internet-download-manager",
       .display_names = {L"Internet Download Manager"}},
      {.software_id = "the-geometers-sketchpad",
       .display_names = {L"The Geometer's Sketchpad", L"几何画板"}},
      {.software_id = "java-runtime",
       .display_names = {L"Java(TM) SE Development Kit 25", L"Java"}},
      {.software_id = "dotnet-runtime",
       .display_names = {L"Microsoft .NET Runtime - 10.0.11", L"Microsoft .NET"}},
      {.software_id = "directx-runtime",
       .display_names = {L"Microsoft DirectX", L"DirectX"}},
      {.software_id = "powershell-7",
       .display_names = {L"PowerShell 7-x64", L"PowerShell 7"}},
  };
}

[[nodiscard]] std::vector<WindowsPresenceRegistration>
normalize_presence_registrations(
    std::vector<WindowsPresenceRegistration> registrations) {
  auto const canonical = canonical_presence_registrations();
  if (registrations.empty()) {
    return canonical;
  }

  // The caller may select a reviewed subset, but cannot supply a registry
  // path, key, display name, or a new stable ID. Always replace supplied
  // names with the canonical project-owned identity rules.
  std::vector<WindowsPresenceRegistration> normalized;
  for (auto const& requested : registrations) {
    auto const found = std::ranges::find(canonical, requested.software_id,
                                         &WindowsPresenceRegistration::software_id);
    if (found != canonical.end() &&
        std::ranges::find(normalized, requested.software_id,
                          &WindowsPresenceRegistration::software_id) ==
            normalized.end()) {
      normalized.push_back(*found);
    }
  }
  return normalized;
}

[[nodiscard]] bool same_source(WindowsSourceResolutionRegistration const& registration,
                                std::string_view software_id,
                                catalog::CatalogSource const& declared_source) noexcept {
  return registration.software_id == software_id &&
         registration.purpose == declared_source.purpose.value_or(
                                      catalog::SourcePurpose::primary) &&
         registration.declared_address == declared_source.address;
}

[[nodiscard]] bool valid_https_address(std::string_view address) noexcept {
  return address.starts_with("https://") &&
         address.find('#') == std::string_view::npos &&
         catalog::valid_http_address(address);
}

[[nodiscard]] bool valid_fixed_installer_address(
    std::string_view address) noexcept {
  if (!valid_https_address(address)) {
    return false;
  }
  auto const authority = address.find("//") + 2U;
  auto const path_start = address.find('/', authority);
  if (path_start == std::string_view::npos) {
    return false;
  }
  auto const path_end = address.find_first_of("?#", path_start);
  auto const path = address.substr(
      path_start, path_end == std::string_view::npos
                      ? std::string_view::npos
                      : path_end - path_start);
  // Query parameters select a channel or mutable server-side state and are
  // therefore not part of a reviewed frozen installer identity.
  if (path_end != std::string_view::npos &&
      address[path_end] == '?') {
    return false;
  }
  // A release page, dynamic channel, HTML document or redirect endpoint is
  // not an installer asset. Only an explicitly frozen PE/MSI path is valid.
  return path.ends_with(".exe") || path.ends_with(".msi") || path.ends_with(".zip");
}

[[nodiscard]] bool valid_sha256(std::string_view value) noexcept {
  return value.size() == 64U &&
         std::ranges::all_of(value, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] bool github_release_asset_path_is_canonical(
    std::string_view path) noexcept {
  if (path.empty() || path.front() != '/') {
    return false;
  }
  std::vector<std::string_view> segments;
  std::size_t start = 1U;
  while (start <= path.size()) {
    auto const end = path.find('/', start);
    auto const segment_end = end == std::string_view::npos ? path.size() : end;
    if (segment_end == start) {
      return false;
    }
    segments.push_back(path.substr(start, segment_end - start));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
  }
  return segments.size() == 6U && !segments[0].empty() &&
         !segments[1].empty() && segments[2] == "releases" &&
         segments[3] == "download" && !segments[4].empty() &&
         !segments[5].empty();
}

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(std::string_view value);

[[nodiscard]] std::optional<std::string> wide_to_utf8(
    std::wstring_view value) {
  if (value.size() > static_cast<std::size_t>(INT_MAX)) {
    return std::nullopt;
  }
  auto const utf8_length = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (utf8_length <= 0) {
    return std::nullopt;
  }
  std::string result(static_cast<std::size_t>(utf8_length), '\0');
  if (::WideCharToMultiByte(
          CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
          static_cast<int>(value.size()), result.data(), utf8_length, nullptr,
          nullptr) != utf8_length) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<WindowsControlledAddressIdentity>
parse_controlled_address_identity(std::string_view address) {
  auto wide_address = utf8_to_wide(address);
  if (!wide_address.has_value()) {
    return std::nullopt;
  }
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  wchar_t host[256]{};
  wchar_t path[32768]{};
  wchar_t extra[32768]{};
  components.lpszHostName = host;
  components.dwHostNameLength = static_cast<DWORD>(std::size(host));
  components.lpszUrlPath = path;
  components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  components.lpszExtraInfo = extra;
  components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
  if (!::WinHttpCrackUrl(wide_address->c_str(),
                         static_cast<DWORD>(wide_address->size()), 0,
                         &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS ||
      components.dwHostNameLength == 0 || components.dwUserNameLength != 0 ||
      components.dwPasswordLength != 0 || components.dwUrlPathLength == 0 ||
      (components.dwExtraInfoLength != 0 &&
       std::wstring_view{components.lpszExtraInfo, components.dwExtraInfoLength}
               .find(L'#') != std::wstring_view::npos)) {
    return std::nullopt;
  }
  std::wstring lower_host{components.lpszHostName,
                          components.dwHostNameLength};
  std::ranges::transform(lower_host, lower_host.begin(), [](wchar_t value) {
    return static_cast<wchar_t>(::towlower(value));
  });
  auto host_utf8 = wide_to_utf8(lower_host);
  if (!host_utf8.has_value()) {
    return std::nullopt;
  }
  auto path_utf8 = wide_to_utf8(
      std::wstring_view{components.lpszUrlPath, components.dwUrlPathLength});
  if (!path_utf8.has_value() || path_utf8->empty()) {
    return std::nullopt;
  }
  std::string query;
  if (components.dwExtraInfoLength != 0) {
    auto const extra_info = std::wstring_view{components.lpszExtraInfo,
                                              components.dwExtraInfoLength};
    if (extra_info.front() != L'?') {
      return std::nullopt;
    }
    auto query_utf8 = wide_to_utf8(extra_info);
    if (!query_utf8.has_value()) {
      return std::nullopt;
    }
    query = std::move(*query_utf8);
  }
  return WindowsControlledAddressIdentity{
      .host = std::move(*host_utf8),
      .port = static_cast<std::uint16_t>(components.nPort),
      .path = std::move(*path_utf8),
      .query = std::move(query),
  };
}

[[nodiscard]] bool same_controlled_address_identity(
    WindowsControlledAddressIdentity const& registered,
    WindowsControlledAddressIdentity const& target) noexcept {
  return registered.host == target.host && registered.port == target.port &&
         registered.path == target.path && registered.query == target.query;
}

[[nodiscard]] std::string_view trim_ascii_ows(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1U);
  }
  return value;
}

[[nodiscard]] bool parse_decimal_u64(std::string_view value,
                                     std::uint64_t& result) noexcept {
  if (value.empty()) {
    return false;
  }
  auto const parsed = std::from_chars(value.data(), value.data() + value.size(),
                                      result);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

[[nodiscard]] bool content_range_matches(std::string_view content_range,
                                          std::uint64_t resume_from) noexcept {
  auto const value = trim_ascii_ows(content_range);
  constexpr std::string_view kUnit = "bytes ";
  if (!value.starts_with(kUnit)) {
    return false;
  }
  auto const range = value.substr(kUnit.size());
  auto const dash = range.find('-');
  auto const slash = range.find('/');
  if (dash == std::string_view::npos || slash == std::string_view::npos ||
      dash == 0 || slash <= dash + 1U ||
      range.find('-', dash + 1U) != std::string_view::npos ||
      range.find('/', slash + 1U) != std::string_view::npos) {
    return false;
  }
  std::uint64_t start{};
  std::uint64_t end{};
  if (!parse_decimal_u64(range.substr(0, dash), start) ||
      !parse_decimal_u64(range.substr(dash + 1U, slash - dash - 1U), end) ||
      start != resume_from || end < start) {
    return false;
  }
  auto const total = range.substr(slash + 1U);
  if (total == "*") {
    return true;
  }
  std::uint64_t total_bytes{};
  return parse_decimal_u64(total, total_bytes) && total_bytes > end;
}

[[nodiscard]] std::optional<std::string> query_content_range_utf8(
    HINTERNET request) {
  DWORD header_size = 0;
  if (::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE,
                           WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &header_size,
                           WINHTTP_NO_HEADER_INDEX) ||
      ::GetLastError() != ERROR_INSUFFICIENT_BUFFER || header_size == 0) {
    return std::nullopt;
  }
  std::wstring header(static_cast<std::size_t>(header_size) / sizeof(wchar_t) +
                          1U,
                      L'\0');
  if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_RANGE,
                             WINHTTP_HEADER_NAME_BY_INDEX, header.data(),
                             &header_size, WINHTTP_NO_HEADER_INDEX)) {
    return std::nullopt;
  }
  header.resize(static_cast<std::size_t>(header_size) / sizeof(wchar_t));
  if (!header.empty() && header.back() == L'\0') {
    header.pop_back();
  }
  return wide_to_utf8(header);
}

[[nodiscard]] std::vector<std::string> allowed_redirect_hosts(
    std::string_view software_id) {
  // GitHub release assets use a bounded, documented CDN redirect chain.  The
  // initial github.com URL remains the registered source; only these exact
  // asset hosts may receive the next request.  Other official sources are
  // intentionally no-redirect until their concrete asset path is reviewed.
  if (software_id == "game-cheats-manager" || software_id == "powershell-7") {
    return {"github.com", "release-assets.githubusercontent.com",
            "objects.githubusercontent.com"};
  }
  return {};
}

[[nodiscard]] bool contains_redirect_host(
    std::vector<std::string> const& allowed, std::string_view host) noexcept {
  return std::ranges::find(allowed, host) != allowed.end();
}

[[nodiscard]] bool is_github_release_cdn_host(std::string_view host) noexcept {
  return host == "release-assets.githubusercontent.com" ||
         host == "objects.githubusercontent.com";
}

[[nodiscard]] bool github_release_cdn_path_is_well_formed(
    std::string_view path) noexcept {
  constexpr std::string_view kPrefix =
      "/github-production-release-asset/";
  if (!path.starts_with(kPrefix)) {
    return false;
  }
  auto const suffix = path.substr(kPrefix.size());
  auto const separator = suffix.find('/');
  if (separator == 0 || separator == std::string_view::npos ||
      separator + 1U >= suffix.size() ||
      suffix.find('/', separator + 1U) != std::string_view::npos) {
    return false;
  }
  auto const asset_id = suffix.substr(0, separator);
  auto const asset_token = suffix.substr(separator + 1U);
  return std::ranges::all_of(asset_id, [](char value) {
           return value >= '0' && value <= '9';
         }) &&
         std::ranges::all_of(asset_token, [](char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f') ||
                  (value >= 'A' && value <= 'F') || value == '-';
         });
}

[[nodiscard]] bool github_release_cdn_query_binds_filename(
    std::string_view registered_path, std::string_view target_query) noexcept {
  auto const separator = registered_path.rfind('/');
  if (separator == std::string_view::npos ||
      separator + 1U >= registered_path.size() || target_query.size() <= 1U) {
    return false;
  }
  auto const filename = registered_path.substr(separator + 1U);
  // GitHub puts the original asset name in both its signed disposition
  // parameter and its response-content-disposition parameter.  Requiring
  // that name keeps the CDN exception bound to the registered release asset;
  // the signature itself remains owned and checked by GitHub's CDN.
  return target_query.find(filename) != std::string_view::npos;
}

[[nodiscard]] bool github_release_cdn_redirect_matches(
    WindowsControlledAddressIdentity const& registered,
    WindowsControlledAddressIdentity const& target) noexcept {
  return registered.host == "github.com" &&
         github_release_asset_path_is_canonical(registered.path) &&
         is_github_release_cdn_host(target.host) &&
         target.port == registered.port &&
         github_release_cdn_path_is_well_formed(target.path) &&
         github_release_cdn_query_binds_filename(registered.path,
                                                 target.query);
}

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(std::string_view value) {
  if (value.empty() || value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  auto const length = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             value.data(), static_cast<int>(value.size()),
                                             nullptr, 0);
  if (length <= 0) {
    return std::nullopt;
  }
  std::wstring result(static_cast<std::size_t>(length), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), length) != length) {
    return std::nullopt;
  }
  return result;
}

struct WinHttpCloser final {
  void operator()(void* value) const noexcept {
    if (value != nullptr) {
      ::WinHttpCloseHandle(static_cast<HINTERNET>(value));
    }
  }
};
using UniqueWinHttpHandle = std::unique_ptr<void, WinHttpCloser>;

[[nodiscard]] std::optional<std::string> sha256_hex(
    std::span<std::byte const> bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return std::nullopt;
  }
  ULONG object_length{};
  ULONG hash_length{};
  ULONG property_size{};
  auto const object_status = ::BCryptGetProperty(
      algorithm, BCRYPT_OBJECT_LENGTH,
      reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
      &property_size, 0);
  auto const hash_status = ::BCryptGetProperty(
      algorithm, BCRYPT_HASH_LENGTH,
      reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length),
      &property_size, 0);
  if (!BCRYPT_SUCCESS(object_status) || !BCRYPT_SUCCESS(hash_status) ||
      hash_length != 32U || object_length == 0U) {
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return std::nullopt;
  }
  std::vector<std::byte> object(object_length);
  std::vector<std::byte> digest(hash_length);
  BCRYPT_HASH_HANDLE hash{};
  auto const create_status = ::BCryptCreateHash(
      algorithm, &hash, reinterpret_cast<PUCHAR>(object.data()), object_length,
      nullptr, 0, 0);
  if (!BCRYPT_SUCCESS(create_status)) {
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return std::nullopt;
  }
  auto const data_status = bytes.empty()
                               ? static_cast<NTSTATUS>(0)
                               : ::BCryptHashData(
                                     hash,
                                     reinterpret_cast<PUCHAR>(const_cast<std::byte*>(
                                         bytes.data())),
                                     static_cast<ULONG>(bytes.size()), 0);
  auto const finish_status =
      BCRYPT_SUCCESS(data_status)
          ? ::BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                               hash_length, 0)
          : data_status;
  ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!BCRYPT_SUCCESS(finish_status)) {
    return std::nullopt;
  }
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(hash_length * 2U);
  for (auto const byte : digest) {
    auto const value = std::to_integer<unsigned int>(byte);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

[[nodiscard]] cache::ControlledDownloadResult transfer_https(
    std::string_view software_id, std::string_view address,
    std::vector<std::string> const& redirect_hosts,
    std::uint64_t resume_from, std::optional<std::uint64_t> expected_bytes,
    std::optional<std::string> const& expected_sha256) {
  constexpr std::size_t kMaximumRedirects = 5;
  auto const registered_identity = parse_controlled_address_identity(address);
  if (!registered_identity.has_value()) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "registered HTTPS address has no stable asset identity"};
  }
  std::vector<std::string> visited_addresses;
  visited_addresses.reserve(kMaximumRedirects + 1U);
  std::string current_address{address};
  for (std::size_t redirect_count = 0;; ++redirect_count) {
    if (std::ranges::find(visited_addresses, current_address) !=
        visited_addresses.end()) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect loop was rejected"};
    }
    visited_addresses.push_back(current_address);
  auto wide_address = utf8_to_wide(current_address);
  if (!wide_address.has_value()) {
    return {.detail = "registered HTTPS address is not valid UTF-8"};
  }
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  wchar_t host[256]{};
  wchar_t path[32768]{};
  wchar_t extra[32768]{};
  components.lpszHostName = host;
  components.dwHostNameLength = static_cast<DWORD>(std::size(host));
  components.lpszUrlPath = path;
  components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  components.lpszExtraInfo = extra;
  components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
  if (!::WinHttpCrackUrl(wide_address->c_str(), static_cast<DWORD>(wide_address->size()),
                         0, &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS || components.dwHostNameLength == 0 ||
      components.dwUserNameLength != 0 || components.dwPasswordLength != 0 ||
      components.dwUrlPathLength == 0 ||
      (components.dwExtraInfoLength != 0 &&
       std::wstring_view{components.lpszExtraInfo, components.dwExtraInfoLength}.find(L'#') !=
           std::wstring_view::npos)) {
    return {.detail = "registered address must be an HTTPS URL without credentials or fragments"};
  }

  UniqueWinHttpHandle session{::WinHttpOpen(L"azzs-controlled-cache/1",
                                             WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                             WINHTTP_NO_PROXY_NAME,
                                             WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session) {
    return {.code = cache::ControlledDownloadCode::network_unavailable,
            .detail = "WinHTTP session could not be opened"};
  }
  ::WinHttpSetTimeouts(session.get(), kTimeoutMilliseconds, kTimeoutMilliseconds,
                       kTimeoutMilliseconds, kTimeoutMilliseconds);
  std::wstring host_name{components.lpszHostName, components.dwHostNameLength};
  UniqueWinHttpHandle connection{::WinHttpConnect(
      session.get(), host_name.c_str(), components.nPort, 0)};
  if (!connection) {
    return {.code = cache::ControlledDownloadCode::network_unavailable,
            .detail = "WinHTTP connection could not be opened"};
  }
  std::wstring request_path{components.lpszUrlPath, components.dwUrlPathLength};
  if (request_path.empty()) {
    request_path = L"/";
  }
  // Keep the query string returned separately by WinHttpCrackUrl. Dropping it
  // changes signed or architecture-specific download assets.
  if (components.dwExtraInfoLength != 0) {
    request_path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
  }
  UniqueWinHttpHandle request{::WinHttpOpenRequest(
      connection.get(), L"GET", request_path.c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
  if (!request) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "WinHTTP request could not be opened"};
  }
  // A controlled source must not silently move to a different host or asset.
  DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
  if (!::WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirect_policy, sizeof(redirect_policy))) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "WinHTTP redirect policy could not be fixed"};
  }
  std::wstring range_header;
  if (resume_from != 0) {
    range_header = L"Range: bytes=" + std::to_wstring(resume_from) + L"-\r\n";
  }
  if (!::WinHttpSendRequest(request.get(), range_header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                                 : range_header.c_str(),
                           range_header.empty() ? 0 : static_cast<DWORD>(-1),
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !::WinHttpReceiveResponse(request.get(), nullptr)) {
    return {.code = cache::ControlledDownloadCode::network_unavailable,
            .detail = "controlled HTTPS request failed"};
  }
  DWORD status{};
  DWORD status_size = sizeof(status);
  if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE |
                                            WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX)) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled HTTPS response status is unavailable"};
  }
  if (status == 301 || status == 302 || status == 303 || status == 307 ||
      status == 308) {
    if (resume_from != 0 || redirect_count >= kMaximumRedirects) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect limit or resume boundary was rejected"};
    }
    DWORD location_size = 0;
    if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION,
                               WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                               &location_size, WINHTTP_NO_HEADER_INDEX) &&
        ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect location is unavailable"};
    }
    std::wstring location(location_size / sizeof(wchar_t) + 1U, L'\0');
    if (!::WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION,
                               WINHTTP_HEADER_NAME_BY_INDEX, location.data(),
                               &location_size, WINHTTP_NO_HEADER_INDEX)) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect location could not be read"};
    }
    location.resize(location_size / sizeof(wchar_t));
    if (!location.empty() && location.back() == L'\0') {
      location.pop_back();
    }
    auto location_utf8 = std::string{};
    if (location.size() > static_cast<std::size_t>(INT_MAX)) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect location is too long"};
    }
    auto const utf8_length = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, location.data(),
        static_cast<int>(location.size()), nullptr, 0, nullptr, nullptr);
    if (utf8_length <= 0) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect location is not UTF-8"};
    }
    location_utf8.resize(static_cast<std::size_t>(utf8_length));
    if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, location.data(),
                              static_cast<int>(location.size()),
                              location_utf8.data(), utf8_length, nullptr,
                              nullptr) != utf8_length) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS redirect location conversion failed"};
    }
    auto target_identity = parse_controlled_address_identity(location_utf8);
    auto const exact_identity =
        target_identity.has_value() &&
        windows_controlled_redirect_matches(*registered_identity,
                                             *target_identity);
    auto const github_cdn_identity =
        target_identity.has_value() &&
        expected_bytes.has_value() && expected_sha256.has_value() &&
        valid_sha256(*expected_sha256) &&
        windows_controlled_github_release_redirect_matches(
            *registered_identity, *target_identity);
    if (!target_identity.has_value() || (!exact_identity && !github_cdn_identity) ||
        !contains_redirect_host(redirect_hosts, target_identity->host)) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail =
                  "controlled HTTPS redirect target is not the registered asset"};
    }
    current_address = std::move(location_utf8);
    continue;
  }
  if (resume_from != 0) {
    if (status != 206) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "server did not honor the controlled resume range"};
    }
    auto const content_range = query_content_range_utf8(request.get());
    if (!content_range.has_value() ||
        !content_range_matches(*content_range, resume_from)) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail =
                  "server Content-Range did not begin at the controlled resume offset"};
    }
  }
  if (resume_from == 0 && status != 200) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled HTTPS response was not successful"};
  }
  std::vector<std::byte> bytes;
  std::uint64_t total = resume_from;
  for (;;) {
    DWORD available{};
    if (!::WinHttpQueryDataAvailable(request.get(), &available)) {
      return {.code = cache::ControlledDownloadCode::network_unavailable,
              .detail = "controlled HTTPS response size is unavailable"};
    }
    if (available == 0) {
      break;
    }
    if (available > kMaximumResponseBytes ||
        total > kMaximumResponseBytes - available) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled HTTPS response exceeds the cache limit"};
    }
    auto const old_size = bytes.size();
    if (static_cast<std::uint64_t>(old_size) >
            static_cast<std::uint64_t>(bytes.max_size()) - available ||
        available > kMaximumResponseBytes - total) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "controlled HTTPS response exceeds the cache limit"};
    }
    bytes.resize(old_size + available);
    DWORD read{};
    if (!::WinHttpReadData(request.get(), bytes.data() + old_size, available, &read)) {
      return {.code = cache::ControlledDownloadCode::network_unavailable,
              .detail = "controlled HTTPS response read failed"};
    }
    if (read == 0) {
      bytes.resize(old_size);
      break;
    }
    bytes.resize(old_size + read);
    total += read;
  }
  if (expected_bytes.has_value() && total != *expected_bytes) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled HTTPS response byte count did not match the registered asset"};
  }
  if (expected_sha256.has_value()) {
    if (resume_from != 0) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "摘要绑定资产不允许续传"};
    }
    if (!valid_sha256(*expected_sha256)) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "registered SHA-256 is malformed"};
    }
    auto const digest = sha256_hex(bytes);
    if (!digest.has_value()) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "Windows SHA-256 verification could not be completed"};
    }
    if (*digest != *expected_sha256) {
      return {.code = cache::ControlledDownloadCode::failed,
              .detail = "downloaded bytes did not match the registered SHA-256"};
    }
  }
  return {.code = cache::ControlledDownloadCode::completed,
          .bytes = std::move(bytes),
          .detail = "controlled HTTPS package transfer completed for " +
                    std::string{software_id}};
  }
}

}  // namespace

std::optional<WindowsControlledAddressIdentity>
parse_windows_controlled_https_address(std::string_view address) {
  return parse_controlled_address_identity(address);
}

bool windows_controlled_redirect_matches(
    WindowsControlledAddressIdentity const& registered,
    WindowsControlledAddressIdentity const& target) noexcept {
  return same_controlled_address_identity(registered, target);
}

bool windows_controlled_github_release_redirect_matches(
    WindowsControlledAddressIdentity const& registered,
    WindowsControlledAddressIdentity const& target) noexcept {
  return github_release_cdn_redirect_matches(registered, target);
}

bool windows_controlled_content_range_matches(
    std::string_view content_range, std::uint64_t resume_from) noexcept {
  return content_range_matches(content_range, resume_from);
}

WindowsRegisteredSourceResolver::WindowsRegisteredSourceResolver(
    std::vector<WindowsSourceResolutionRegistration> registrations)
    : registrations_(std::move(registrations)) {}

selection::SourceResolutionResult WindowsRegisteredSourceResolver::resolve(
    std::string_view software_id, catalog::CatalogSource const& declared_source) {
  if (software_id.empty() || !declared_source.purpose.has_value() ||
      !valid_https_address(declared_source.address)) {
    return {.error = "declared source is incomplete or invalid"};
  }
  auto const found = std::ranges::find_if(
      registrations_, [&](WindowsSourceResolutionRegistration const& registration) {
        return same_source(registration, software_id, declared_source);
      });
  if (found == registrations_.end() ||
      !valid_fixed_installer_address(found->actual_address) ||
      found->version.empty() || found->hosting_mechanism.empty() || found->branch.empty() ||
      found->capability_version.empty() || found->packages.empty()) {
    return {.error = "no complete controlled source registration matches the declaration"};
  }
  selection::SourceResolutionResult result;
  result.snapshot = domain::software_selection::ResolvedSourceSnapshot{
      .software_id = found->software_id,
      .declared_purpose = found->purpose,
      .declared_address = found->declared_address,
      .version = found->version,
      .actual_address = found->actual_address,
      .hosting_mechanism = found->hosting_mechanism,
      .branch = found->branch,
      .packages = found->packages,
      .network_required = found->network_required,
      .resolved_at_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count(),
      .capability_version = found->capability_version,
  };
  if (!result.snapshot->valid()) {
    return {.error = "controlled source registration does not form a valid snapshot"};
  }
  result.resolved = true;
  return result;
}

WindowsControlledPackageDownloader::WindowsControlledPackageDownloader(
    ControlledCacheRoot root)
    : root_(std::move(root)) {}

bool WindowsControlledPackageDownloader::register_source(
    CacheAssetIdentity const& identity, std::string_view actual_address,
    std::optional<std::uint64_t> expected_bytes,
    std::optional<std::string_view> expected_sha256,
    std::vector<std::string> archive_members) {
  if (!root_.valid() || !identity.valid() ||
      !valid_fixed_installer_address(actual_address) ||
      (expected_bytes.has_value() && *expected_bytes == 0) ||
      (expected_sha256.has_value() && !valid_sha256(*expected_sha256))) {
    return false;
  }
  auto found = std::ranges::find(sources_, identity, &RegisteredSource::identity);
  if (found != sources_.end()) {
    return found->address == actual_address &&
           found->expected_bytes == expected_bytes &&
           found->expected_sha256.has_value() == expected_sha256.has_value() &&
           (!expected_sha256.has_value() ||
            *found->expected_sha256 == *expected_sha256) &&
           found->archive_members == archive_members;
  }
  sources_.push_back({.identity = identity,
                      .address = std::string{actual_address},
                      .allowed_redirect_hosts =
                          allowed_redirect_hosts(identity.software_id),
                      .expected_bytes = expected_bytes,
                      .expected_sha256 = expected_sha256.has_value()
                                             ? std::optional<std::string>{
                                                   std::string{*expected_sha256}}
                                             : std::nullopt,
                      .archive_members = std::move(archive_members)});
  return true;
}

void WindowsControlledPackageDownloader::clear_registered_sources() noexcept {
  sources_.clear();
}

cache::ControlledDownloadResult WindowsControlledPackageDownloader::transfer(
    cache::ControlledDownloadRequest const& request) {
  if (!root_.valid() || request.cache_root != root_ || !request.asset.valid()) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled package download request is not bound to this root"};
  }
  auto const found = std::ranges::find(sources_, request.asset.identity,
                                       &RegisteredSource::identity);
  if (found == sources_.end()) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled package identity has no registered HTTPS source"};
  }
  if (request.asset.expected_bytes != found->expected_bytes ||
      request.asset.expected_sha256 != found->expected_sha256 ||
      request.asset.archive_members != found->archive_members) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled package metadata is not bound to the registered asset"};
  }
  if (request.resume_from_bytes > kMaximumResponseBytes) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled package resume offset exceeds the cache limit"};
  }
  return transfer_https(found->identity.software_id, found->address,
                        found->allowed_redirect_hosts,
                        request.resume_from_bytes, found->expected_bytes,
                        found->expected_sha256);
}

WindowsRegistrySoftwarePresenceDetector::WindowsRegistrySoftwarePresenceDetector(
    std::vector<WindowsPresenceRegistration> registrations)
    : WindowsRegistrySoftwarePresenceDetector(
          std::make_unique<WindowsRegistryPresenceQuery>(),
          std::move(registrations)) {}

WindowsRegistrySoftwarePresenceDetector::WindowsRegistrySoftwarePresenceDetector(
    std::unique_ptr<WindowsPresenceRegistryQuery> query,
    std::vector<WindowsPresenceRegistration> registrations)
    : query_(std::move(query)),
      registrations_(normalize_presence_registrations(std::move(registrations))) {}

selection::PresenceDetection WindowsRegistrySoftwarePresenceDetector::detect_impl(
    std::string_view software_id, std::optional<std::wstring> expected_version) {
  auto const registration = std::ranges::find(
      registrations_, software_id, &WindowsPresenceRegistration::software_id);
  if (registration == registrations_.end()) {
    return {.detail = "software presence rule is unavailable for this stable ID"};
  }
  if (!query_) {
    return {.detail = "Windows uninstall registry query is unavailable"};
  }

  constexpr std::array<std::pair<WindowsPresenceRegistryHive,
                                 WindowsPresenceRegistryView>,
                       4>
      kRegistryRoots{{
          {WindowsPresenceRegistryHive::current_user,
           WindowsPresenceRegistryView::view_32},
          {WindowsPresenceRegistryHive::current_user,
           WindowsPresenceRegistryView::view_64},
          {WindowsPresenceRegistryHive::local_machine,
           WindowsPresenceRegistryView::view_32},
          {WindowsPresenceRegistryHive::local_machine,
           WindowsPresenceRegistryView::view_64},
      }};

  bool present = false;
  for (auto const [hive, view] : kRegistryRoots) {
    WindowsPresenceRegistryRead read;
    try {
      read = query_->read(hive, view);
    } catch (...) {
      return {.detail = "Windows uninstall registry query threw an exception"};
    }
    if (read.status == WindowsPresenceRegistryReadStatus::failed) {
      return {.detail = read.detail.empty()
                           ? "Windows uninstall registry query failed"
                           : read.detail};
    }
    if (read.status == WindowsPresenceRegistryReadStatus::root_absent) {
      continue;
    }
    if (read.status != WindowsPresenceRegistryReadStatus::succeeded) {
      return {.detail = "Windows uninstall registry query returned an unknown status"};
    }
    present = present || std::ranges::any_of(
                             read.entries, [&](WindowsPresenceRegistryEntry const& entry) {
                               if (!registration_matches(*registration, entry)) {
                                 return false;
                               }
                               if (!expected_version.has_value()) {
                                 return true;
                               }
                               return !entry.display_version.empty() &&
                                      same_display_version(entry.display_version,
                                                           *expected_version);
                             });
  }

  return {.completed = true,
          .present = present,
          .detail = present ? "fixed uninstall entry matched"
                            : "fixed uninstall roots contained no matching entry"};
}

selection::PresenceDetection WindowsRegistrySoftwarePresenceDetector::detect(
    std::string_view software_id) {
  return detect_impl(software_id, std::nullopt);
}

selection::PresenceDetection WindowsRegistrySoftwarePresenceDetector::detect_version(
    std::string_view software_id, std::string_view expected_version) {
  auto wide = utf8_to_wide(expected_version);
  if (!wide.has_value()) {
    return {.detail = "expected software version is not valid UTF-8"};
  }
  return detect_impl(software_id, std::move(*wide));
}

std::vector<WindowsSourceResolutionRegistration>
initial_windows_source_registrations() {
  using domain::architecture_selection::PackageArchitecture;
  using domain::software_selection::PackageType;
  auto make = [](std::string software_id, std::string declared,
                 std::string version, std::string actual,
                 std::string mechanism, std::string branch,
                 std::string identity,
                 PackageType package_type = PackageType::online_installer,
                 PackageArchitecture package_architecture =
                     PackageArchitecture::x64,
                 std::optional<std::uint64_t> expected_bytes = std::nullopt,
                 std::optional<std::string> expected_sha256 = std::nullopt,
                 std::vector<std::string> archive_members = {}) {
    auto candidate_software_id = software_id;
    auto candidate_version = version;
    return WindowsSourceResolutionRegistration{
        .software_id = std::move(software_id),
        .purpose = catalog::SourcePurpose::primary,
        .declared_address = std::move(declared),
        .version = std::move(version),
        .actual_address = std::move(actual),
        .hosting_mechanism = std::move(mechanism),
        .branch = std::move(branch),
        .packages = {{
            .candidate = {
                .software_id = std::move(candidate_software_id),
                .architecture = package_architecture,
                .version = std::move(candidate_version),
                .identity = std::move(identity),
            },
            .package_type = package_type,
            .complete_package = package_type == PackageType::full_package,
            .network_required = package_type != PackageType::full_package,
            .expected_bytes = expected_bytes,
            .expected_sha256 = expected_sha256,
            .archive_members = std::move(archive_members),
        }},
        .network_required = package_type != PackageType::full_package,
        .capability_version = "windows-controlled-source-v1",
    };
  };
  return {
      make("sogou-input", "https://shurufa.sogou.com/windows", "16.7",
           "https://ime-sec.gtimg.com/202608220205/868019120776d61ca6203f504b825ad2/pc/pinyin_guanwang_16.7b.exe",
           "official-fixed-asset", "Windows 个人版", "sogou-16.7-x64"),
      make("game-cheats-manager",
           "https://github.com/dyang886/Game-Cheats-Manager/releases", "2.4.6",
           "https://github.com/dyang886/Game-Cheats-Manager/releases/download/v2.4.6/Game.Cheats.Manager.Setup.2.4.6.exe",
            "github-release-asset", "GitHub stable release", "gcm-2.4.6-x64",
            PackageType::full_package, PackageArchitecture::x64, 44676819ULL,
            "d6229ec299b001c277327e78256cb7bcc471a5bb90a86fbd6aeebcb76bc6a129"),
      make("internet-download-manager",
           "https://www.internetdownloadmanager.com/download.html",
           "trial-2026-08",
           "https://download.internetdownloadmanager.com/idman643build10.exe",
           "official-trial-asset", "Windows stable trial", "idm-trial-2026-08-x64"),
      make("java-runtime", "https://www.oracle.com/java/technologies/downloads/",
           "25", "https://download.oracle.com/java/25/latest/jdk-25_windows-x64_bin.msi",
           "oracle-fixed-lts-asset", "Oracle JDK 25 LTS", "java-jdk-25-x64"),
       make("qq", "https://im.qq.com/pcqq", "9.9.33",
            "https://qqdl.gtimg.cn/qqfile/QQNT/9.9.33/release/497e2f1f/QQ_9.9.33_260813_x64_01.exe",
            "official-fixed-asset", "Windows x64 stable release", "qq-9.9.33-x64",
            PackageType::online_installer, PackageArchitecture::x64,
            314581680ULL),
       make("dotnet-runtime", "https://dotnet.microsoft.com/download/dotnet",
            "10.0.11",
            "https://builds.dotnet.microsoft.com/dotnet/Runtime/10.0.11/dotnet-runtime-10.0.11-win-x64.exe",
            "microsoft-fixed-asset", ".NET Runtime x64", "dotnet-runtime-10.0.11-x64",
            PackageType::online_installer, PackageArchitecture::x64,
            30604768ULL),
       make("directx-runtime",
            "https://www.microsoft.com/en-us/download/details.aspx?id=35",
            "9.29.1974.1",
            "https://download.microsoft.com/download/1/7/1/1718ccc4-6315-4d8e-9543-8e28a4e18c4c/dxwebsetup.exe",
            "microsoft-fixed-asset", "DirectX Web Installer", "directx-9.29.1974.1",
            PackageType::online_installer,
            PackageArchitecture::architecture_independent, 295320ULL),
       make("powershell-7", "https://github.com/PowerShell/PowerShell/releases",
           "7.6.4", "https://github.com/PowerShell/PowerShell/releases/download/v7.6.4/PowerShell-7.6.4-win-x64.msi",
           "github-release-asset", "Windows stable release", "powershell-7.6.4-x64",
            PackageType::full_package, PackageArchitecture::x64, 115515392ULL,
            "d11942df52fd12470169797abfa4781d9480efdc81000ba4fa55a5b921ed8dd0"),
       make("office-tool-plus", "https://otp.landian.vip/en-us/download.html",
            "11.6.6.0",
            "https://github.com/YerongAI/Office-Tool/releases/download/v11.6.6.0/Office_Tool_v11.6.6.0_x64.zip",
            "github-release-archive", "Windows x64 portable release", "office-tool-plus-11.6.6.0-x64",
            PackageType::archive_package, PackageArchitecture::x64, 10698489ULL,
            "43BA169E4D07C8E45ED4846D7171BFBC521E8F61EFFF366112B7C6EF9DAE627B",
            {"Office Tool/Office Tool Plus.exe"}),
  };
}

std::vector<WindowsPresenceRegistration> initial_windows_presence_registrations() {
  return canonical_presence_registrations();
}

}  // namespace azzs::adapters::windows
