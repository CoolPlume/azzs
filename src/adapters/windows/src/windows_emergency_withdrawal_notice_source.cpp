#include "azzs/adapters/windows/windows_emergency_withdrawal_notice_source.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifndef AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT
#define AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT ""
#endif

namespace azzs::adapters::windows {
namespace {

constexpr std::string_view kUnconfiguredEndpointError{
    "emergency withdrawal release endpoint is not configured"};
constexpr std::string_view kInvalidEndpointError{
    "emergency withdrawal release endpoint must use HTTPS"};
constexpr std::string_view kResponseTooLargeError{
    "emergency withdrawal notice response exceeds 4 MiB"};

struct WinHttpHandleCloser final {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) {
      ::WinHttpCloseHandle(static_cast<HINTERNET>(handle));
    }
  }
};

using UniqueWinHttpHandle = std::unique_ptr<void, WinHttpHandleCloser>;

[[nodiscard]] application::NoticeFetchResult winhttp_failure(
    std::string_view operation, DWORD error) {
  return {.error = "winhttp:" + std::string{operation} + " failed (" +
                   std::to_string(error) + ")"};
}

[[nodiscard]] application::NoticeFetchResult response_too_large() {
  return {.error = std::string{kResponseTooLargeError}};
}

[[nodiscard]] bool is_configured_https_endpoint(
    std::string_view endpoint) noexcept {
  constexpr std::string_view scheme{"https://"};
  if (!endpoint.starts_with(scheme) ||
      endpoint.find('#') != std::string_view::npos) {
    return false;
  }
  auto const authority_end = endpoint.find_first_of("/?", scheme.size());
  auto const authority = endpoint.substr(
      scheme.size(), authority_end == std::string_view::npos
                         ? std::string_view::npos
                         : authority_end - scheme.size());
  return !authority.empty() && authority.find('@') == std::string_view::npos;
}

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

struct ParsedHttpsEndpoint final {
  std::wstring host;
  INTERNET_PORT port{};
  std::wstring object_name;
};

[[nodiscard]] std::optional<ParsedHttpsEndpoint> parse_https_endpoint(
    std::string_view endpoint) {
  auto wide_endpoint = utf8_to_wide(endpoint);
  if (!wide_endpoint.has_value() ||
      wide_endpoint->size() >
          static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
    return std::nullopt;
  }

  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(wide_endpoint->c_str(),
                         static_cast<DWORD>(wide_endpoint->size()),
                         ICU_REJECT_USERPWD, &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS ||
      components.lpszHostName == nullptr || components.dwHostNameLength == 0) {
    return std::nullopt;
  }

  ParsedHttpsEndpoint parsed{
      .host = std::wstring{components.lpszHostName,
                           components.dwHostNameLength},
      .port = components.nPort == 0
                  ? static_cast<INTERNET_PORT>(INTERNET_DEFAULT_HTTPS_PORT)
                  : components.nPort,
  };
  if (components.lpszUrlPath != nullptr && components.dwUrlPathLength != 0) {
    parsed.object_name.assign(components.lpszUrlPath,
                              components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength != 0) {
    parsed.object_name.append(components.lpszExtraInfo,
                              components.dwExtraInfoLength);
  }
  if (parsed.object_name.empty()) {
    parsed.object_name = L"/";
  }
  return parsed;
}

class WinHttpEmergencyWithdrawalNoticeRequestExecutor final
    : public EmergencyWithdrawalNoticeRequestExecutor {
 public:
  [[nodiscard]] application::NoticeFetchResult execute(
      EmergencyWithdrawalNoticeRequest const& request) override {
    if (request.method != EmergencyWithdrawalNoticeRequestMethod::get ||
        request.timeout.count() <= 0 ||
        request.timeout.count() > std::numeric_limits<int>::max() ||
        request.maximum_response_bytes == 0 ||
        !is_configured_https_endpoint(request.endpoint)) {
      return {.error = "invalid emergency withdrawal request"};
    }

    auto endpoint = parse_https_endpoint(request.endpoint);
    if (!endpoint.has_value()) {
      return {.error = std::string{kInvalidEndpointError}};
    }

    UniqueWinHttpHandle session{::WinHttpOpen(
        L"Windows Initial Setup Workbench/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
      return winhttp_failure("WinHttpOpen", ::GetLastError());
    }

    auto const timeout = static_cast<int>(request.timeout.count());
    if (!::WinHttpSetTimeouts(session.get(), timeout, timeout, timeout,
                              timeout)) {
      return winhttp_failure("WinHttpSetTimeouts", ::GetLastError());
    }

    UniqueWinHttpHandle connection{::WinHttpConnect(
        session.get(), endpoint->host.c_str(), endpoint->port, 0)};
    if (!connection) {
      return winhttp_failure("WinHttpConnect", ::GetLastError());
    }

    UniqueWinHttpHandle http_request{::WinHttpOpenRequest(
        connection.get(), L"GET", endpoint->object_name.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!http_request) {
      return winhttp_failure("WinHttpOpenRequest", ::GetLastError());
    }

    DWORD disabled_features = WINHTTP_DISABLE_REDIRECTS;
    if (!::WinHttpSetOption(http_request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                            &disabled_features, sizeof(disabled_features))) {
      return winhttp_failure("WinHttpSetOption", ::GetLastError());
    }

    if (!::WinHttpSendRequest(http_request.get(), WINHTTP_NO_ADDITIONAL_HEADERS,
                              0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
      return winhttp_failure("WinHttpSendRequest", ::GetLastError());
    }
    if (!::WinHttpReceiveResponse(http_request.get(), nullptr)) {
      return winhttp_failure("WinHttpReceiveResponse", ::GetLastError());
    }

    DWORD status_code{};
    DWORD status_code_size = sizeof(status_code);
    if (!::WinHttpQueryHeaders(
            http_request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_code_size,
            WINHTTP_NO_HEADER_INDEX)) {
      return winhttp_failure("WinHttpQueryHeaders", ::GetLastError());
    }
    if (status_code != 200U) {
      return {.error = "emergency withdrawal request returned HTTP status " +
                       std::to_string(status_code)};
    }

    constexpr std::size_t kReadChunkBytes{16U * 1024U};
    std::array<char, kReadChunkBytes> buffer{};
    std::string document;
    while (true) {
      DWORD available{};
      if (!::WinHttpQueryDataAvailable(http_request.get(), &available)) {
        return winhttp_failure("WinHttpQueryDataAvailable", ::GetLastError());
      }
      if (available == 0) {
        break;
      }
      if (document.size() >= request.maximum_response_bytes ||
          static_cast<std::size_t>(available) >
              request.maximum_response_bytes - document.size()) {
        return response_too_large();
      }

      auto const bytes_to_read = static_cast<DWORD>(std::min(
          {static_cast<std::size_t>(available), buffer.size(),
           request.maximum_response_bytes - document.size()}));
      DWORD read{};
      if (!::WinHttpReadData(http_request.get(), buffer.data(), bytes_to_read,
                             &read)) {
        return winhttp_failure("WinHttpReadData", ::GetLastError());
      }
      if (read == 0) {
        return {.error = "emergency withdrawal response ended unexpectedly"};
      }
      document.append(buffer.data(), read);
    }

    return {.succeeded = true, .document = std::move(document)};
  }
};

}  // namespace

WindowsEmergencyWithdrawalNoticeSource::WindowsEmergencyWithdrawalNoticeSource()
    : release_endpoint_(AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT),
      owned_executor_(
          std::make_unique<WinHttpEmergencyWithdrawalNoticeRequestExecutor>()),
      executor_(owned_executor_.get()) {}

WindowsEmergencyWithdrawalNoticeSource::~WindowsEmergencyWithdrawalNoticeSource() =
    default;

WindowsEmergencyWithdrawalNoticeSource::WindowsEmergencyWithdrawalNoticeSource(
    std::string release_endpoint,
    EmergencyWithdrawalNoticeRequestExecutor& executor)
    : release_endpoint_(std::move(release_endpoint)), executor_(&executor) {}

WindowsEmergencyWithdrawalNoticeSource
WindowsEmergencyWithdrawalNoticeSource::for_testing(
    EmergencyWithdrawalNoticeRequestExecutor& executor) {
  return WindowsEmergencyWithdrawalNoticeSource{
      AZZS_EMERGENCY_WITHDRAWAL_NOTICE_ENDPOINT, executor};
}

WindowsEmergencyWithdrawalNoticeSource
WindowsEmergencyWithdrawalNoticeSource::for_testing(
    std::string release_endpoint,
    EmergencyWithdrawalNoticeRequestExecutor& executor) {
  return WindowsEmergencyWithdrawalNoticeSource{std::move(release_endpoint),
                                                 executor};
}

application::NoticeFetchResult WindowsEmergencyWithdrawalNoticeSource::fetch() {
  if (release_endpoint_.empty()) {
    return {.error = std::string{kUnconfiguredEndpointError}};
  }
  if (!is_configured_https_endpoint(release_endpoint_)) {
    return {.error = std::string{kInvalidEndpointError}};
  }
  if (executor_ == nullptr) {
    return {.error = "emergency withdrawal request executor is unavailable"};
  }

  auto result = executor_->execute({
      .method = EmergencyWithdrawalNoticeRequestMethod::get,
      .endpoint = release_endpoint_,
      .timeout = kRequestTimeout,
      .maximum_response_bytes = kMaximumDocumentBytes,
  });
  if (result.succeeded && result.document.size() > kMaximumDocumentBytes) {
    return response_too_large();
  }
  return result;
}

}  // namespace azzs::adapters::windows
