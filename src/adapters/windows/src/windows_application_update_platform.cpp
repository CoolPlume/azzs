#include "azzs/adapters/windows/windows_application_update_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::adapters::windows {
namespace {

constexpr std::string_view kManualGithubReleases{
    "https://github.com/CoolPlume/azzs/releases"};
constexpr std::string_view kGithubAccept{"application/vnd.github+json"};
constexpr std::string_view kGithubUserAgent{"azzs-update-check"};

#ifndef AZZS_APPLICATION_UPDATE_ENDPOINT
#define AZZS_APPLICATION_UPDATE_ENDPOINT ""
#endif

#ifndef AZZS_APPLICATION_VERSION
#error "AZZS_APPLICATION_VERSION must be supplied by the authoritative product version source"
#endif
#ifndef AZZS_APPLICATION_RELEASE_CHANNEL
#error "AZZS_APPLICATION_RELEASE_CHANNEL must be supplied by the authoritative product version source"
#endif

constexpr auto kApplicationReleaseChannel =
    application::parse_application_release_channel(
        AZZS_APPLICATION_RELEASE_CHANNEL);
static_assert(kApplicationReleaseChannel.has_value(),
              "AZZS_APPLICATION_RELEASE_CHANNEL must be stable or prerelease");

[[nodiscard]] application::UpdatePlatformResult unavailable(
    std::string detail) {
  return {.code = application::UpdatePlatformResultCode::unavailable,
          .detail = std::move(detail)};
}

[[nodiscard]] bool safe_release_tag(std::string_view value) noexcept {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](unsigned char byte) {
           return (byte >= 'a' && byte <= 'z') ||
                  (byte >= 'A' && byte <= 'Z') ||
                  (byte >= '0' && byte <= '9') || byte == '.' || byte == '-' ||
                  byte == '_';
         });
}

[[nodiscard]] std::wstring wide_ascii(std::string_view value) {
  return std::wstring{value.begin(), value.end()};
}

struct WinHttpHandleCloser final {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) {
      ::WinHttpCloseHandle(static_cast<HINTERNET>(handle));
    }
  }
};

using UniqueWinHttpHandle = std::unique_ptr<void, WinHttpHandleCloser>;

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(std::string_view value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  auto const count = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0);
  if (count <= 0) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            count) != count) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<std::string> wide_to_utf8(std::wstring_view value) {
  if (value.empty() ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::nullopt;
  }
  auto const count = ::WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0, nullptr, nullptr);
  if (count <= 0) return std::nullopt;
  std::string result(static_cast<std::size_t>(count), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count,
                            nullptr, nullptr) != count) {
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
  auto wide = utf8_to_wide(endpoint);
  if (!wide.has_value()) return std::nullopt;
  URL_COMPONENTS components{};
  components.dwStructSize = sizeof(components);
  components.dwSchemeLength = static_cast<DWORD>(-1);
  components.dwHostNameLength = static_cast<DWORD>(-1);
  components.dwUrlPathLength = static_cast<DWORD>(-1);
  components.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (!::WinHttpCrackUrl(wide->c_str(), static_cast<DWORD>(wide->size()),
                         ICU_REJECT_USERPWD, &components) ||
      components.nScheme != INTERNET_SCHEME_HTTPS ||
      components.lpszHostName == nullptr || components.dwHostNameLength == 0) {
    return std::nullopt;
  }
  ParsedHttpsEndpoint result{
      .host = std::wstring{components.lpszHostName, components.dwHostNameLength},
      .port = components.nPort == 0
                  ? static_cast<INTERNET_PORT>(INTERNET_DEFAULT_HTTPS_PORT)
                  : components.nPort,
  };
  if (components.lpszUrlPath != nullptr && components.dwUrlPathLength != 0) {
    result.object_name.assign(components.lpszUrlPath,
                              components.dwUrlPathLength);
  }
  if (components.lpszExtraInfo != nullptr && components.dwExtraInfoLength != 0) {
    result.object_name.append(components.lpszExtraInfo,
                              components.dwExtraInfoLength);
  }
  if (result.object_name.empty()) result.object_name = L"/";
  return result;
}

[[nodiscard]] application::GithubReleaseQueryResult query_unavailable(
    std::string detail) {
  return {.code = application::GithubReleaseQueryResultCode::unavailable,
          .detail = std::move(detail)};
}

[[nodiscard]] application::GithubReleaseQueryResult query_failed(
    std::string detail) {
  return {.code = application::GithubReleaseQueryResultCode::failed,
          .detail = std::move(detail)};
}

[[nodiscard]] ApplicationUpdateReleaseDocument document_unavailable(
    std::string detail) {
  return {.code = application::GithubReleaseQueryResultCode::unavailable,
          .detail = std::move(detail)};
}

[[nodiscard]] ApplicationUpdateReleaseDocument document_failed(
    std::string detail) {
  return {.code = application::GithubReleaseQueryResultCode::failed,
          .detail = std::move(detail)};
}

[[nodiscard]] std::string winhttp_error(std::string_view operation, DWORD code) {
  return "winhttp:" + std::string{operation} + " failed (" +
         std::to_string(code) + ")";
}

class WinHttpApplicationUpdateReleaseRequestExecutor final
    : public ApplicationUpdateReleaseRequestExecutor {
 public:
  [[nodiscard]] ApplicationUpdateReleaseDocument execute(
      ApplicationUpdateReleaseRequest const& request) override {
    if (request.method != ApplicationUpdateReleaseRequestMethod::get ||
        request.endpoint != AZZS_APPLICATION_UPDATE_ENDPOINT ||
        request.accept != kGithubAccept || request.user_agent != kGithubUserAgent ||
        request.timeout.count() <= 0 ||
        request.timeout.count() > std::numeric_limits<int>::max() ||
        request.maximum_response_bytes == 0) {
      return document_failed("invalid GitHub release request");
    }

    auto endpoint = parse_https_endpoint(request.endpoint);
    if (!endpoint.has_value()) {
      return document_failed(
          "GitHub release endpoint is not a valid fixed HTTPS endpoint");
    }
    UniqueWinHttpHandle session{::WinHttpOpen(
        L"azzs-update-check/0.2", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) {
      return document_unavailable(
          winhttp_error("WinHttpOpen", ::GetLastError()));
    }
    auto const timeout = static_cast<int>(request.timeout.count());
    if (!::WinHttpSetTimeouts(session.get(), timeout, timeout, timeout,
                              timeout)) {
      return document_unavailable(
          winhttp_error("WinHttpSetTimeouts", ::GetLastError()));
    }
    UniqueWinHttpHandle connection{::WinHttpConnect(
        session.get(), endpoint->host.c_str(), endpoint->port, 0)};
    if (!connection) {
      return document_unavailable(
          winhttp_error("WinHttpConnect", ::GetLastError()));
    }
    UniqueWinHttpHandle http_request{::WinHttpOpenRequest(
        connection.get(), L"GET", endpoint->object_name.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!http_request) {
      return document_unavailable(
          winhttp_error("WinHttpOpenRequest", ::GetLastError()));
    }
    DWORD disabled_features = WINHTTP_DISABLE_REDIRECTS;
    if (!::WinHttpSetOption(http_request.get(), WINHTTP_OPTION_DISABLE_FEATURE,
                            &disabled_features, sizeof(disabled_features))) {
      return document_unavailable(
          winhttp_error("WinHttpSetOption", ::GetLastError()));
    }

    auto const headers = wide_ascii(
        "Accept: " + std::string{request.accept} + "\r\n" +
        "User-Agent: " + std::string{request.user_agent} + "\r\n");
    if (!::WinHttpSendRequest(http_request.get(), headers.c_str(),
                              static_cast<DWORD>(headers.size()),
                              WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !::WinHttpReceiveResponse(http_request.get(), nullptr)) {
      return document_unavailable(
          winhttp_error("WinHttpReceiveResponse", ::GetLastError()));
    }
    DWORD status_code{};
    DWORD status_size = sizeof(status_code);
    if (!::WinHttpQueryHeaders(
            http_request.get(),
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
            WINHTTP_NO_HEADER_INDEX)) {
      return document_unavailable(
          winhttp_error("WinHttpQueryHeaders", ::GetLastError()));
    }
    if (status_code == 429U || status_code >= 500U || status_code == 408U) {
      return document_unavailable(
          "GitHub release query temporarily unavailable (HTTP " +
          std::to_string(status_code) + ")");
    }
    if (status_code != 200U) {
      return document_failed("GitHub release query returned HTTP status " +
                             std::to_string(status_code));
    }

    constexpr std::size_t kReadChunkBytes{16U * 1024U};
    std::array<char, kReadChunkBytes> buffer{};
    std::string document;
    while (true) {
      DWORD available{};
      if (!::WinHttpQueryDataAvailable(http_request.get(), &available)) {
        return document_unavailable(
            winhttp_error("WinHttpQueryDataAvailable", ::GetLastError()));
      }
      if (available == 0) {
        break;
      }
      if (document.size() >= request.maximum_response_bytes ||
          static_cast<std::size_t>(available) >
              request.maximum_response_bytes - document.size()) {
        return document_failed("GitHub release response exceeds 4 MiB");
      }
      auto const bytes_to_read = static_cast<DWORD>(std::min(
          {static_cast<std::size_t>(available), buffer.size(),
           request.maximum_response_bytes - document.size()}));
      DWORD read{};
      if (!::WinHttpReadData(http_request.get(), buffer.data(), bytes_to_read,
                             &read)) {
        return document_unavailable(
            winhttp_error("WinHttpReadData", ::GetLastError()));
      }
      if (read == 0) {
        return document_failed("GitHub release response ended unexpectedly");
      }
      document.append(buffer.data(), read);
    }
    return {.code = application::GithubReleaseQueryResultCode::succeeded,
            .document = std::move(document)};
  }
};

[[nodiscard]] std::optional<std::string> json_string(
    winrt::Windows::Data::Json::JsonObject const& object,
    wchar_t const* name, std::size_t maximum) {
  try {
    if (!object.HasKey(name)) return std::string{};
    auto value = object.GetNamedString(name, L"");
    auto converted = wide_to_utf8(value.c_str());
    if (!converted.has_value() || converted->size() > maximum) return std::nullopt;
    return converted;
  } catch (winrt::hresult_error const&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::string> json_number_string(
    winrt::Windows::Data::Json::JsonObject const& object, wchar_t const* name) {
  try {
    if (!object.HasKey(name)) return std::nullopt;
    auto const number = object.GetNamedNumber(name);
    if (number < 1.0 || number > 9.0e18) return std::nullopt;
    return std::to_string(static_cast<std::uint64_t>(number));
  } catch (winrt::hresult_error const&) {
    return std::nullopt;
  }
}

[[nodiscard]] std::optional<application::ApplicationBuildIdentity> target_for_asset(
    std::string_view name, std::string_view release_tag,
    bool release_prerelease) {
  // Only the product's published artifact matrix is accepted. Unknown
  // checksums, source archives, and third-party files are ignored.
  auto lower = std::string{name};
  std::ranges::transform(lower, lower.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  if (!lower.starts_with("azzs-") || lower.find('/') != std::string::npos ||
      lower.find('\\') != std::string::npos) {
    return std::nullopt;
  }
  auto const stem_end = lower.find_last_of('.');
  if (stem_end == std::string::npos) return std::nullopt;
  auto const stem = std::string_view{lower}.substr(5, stem_end - 5);
  auto const extension = std::string_view{lower}.substr(stem_end);
  std::string_view edition_token;
  std::string_view remainder;
  if (stem.starts_with("large-offline-")) {
    edition_token = "large-offline";
    remainder = stem.substr(std::string_view{"large-offline-"}.size());
  } else if (stem.starts_with("standard-")) {
    edition_token = "standard";
    remainder = stem.substr(std::string_view{"standard-"}.size());
  } else if (stem.starts_with("rescue-")) {
    edition_token = "rescue";
    remainder = stem.substr(std::string_view{"rescue-"}.size());
  } else {
    return std::nullopt;
  }
  auto const dash = remainder.find('-');
  if (dash == std::string_view::npos) return std::nullopt;
  auto const arch_token = remainder.substr(0, dash);
  auto const form_token = remainder.substr(dash + 1);
  auto edition = application::ApplicationReleaseEdition::standard;
  if (edition_token == "rescue") {
    edition = application::ApplicationReleaseEdition::rescue;
  } else if (edition_token == "large-offline") {
    edition = application::ApplicationReleaseEdition::large_offline;
  } else if (edition_token != "standard") {
    return std::nullopt;
  }
  auto architecture = domain::SystemArchitecture::unknown;
  if (arch_token == "x64") {
    architecture = domain::SystemArchitecture::x64;
  } else if (arch_token == "arm64") {
    architecture = domain::SystemArchitecture::arm64;
  } else {
    return std::nullopt;
  }
  auto form = application::ApplicationReleaseForm::portable;
  if (form_token == "portable" && extension == ".zip") {
    form = application::ApplicationReleaseForm::portable;
  } else if (form_token == "machine" && extension == ".msi") {
    form = application::ApplicationReleaseForm::installed;
  } else {
    return std::nullopt;
  }
  auto const channel = release_prerelease
                           ? application::ApplicationReleaseChannel::prerelease
                           : application::ApplicationReleaseChannel::stable;
  auto target = application::ApplicationBuildIdentity{
      .version = std::string{release_tag},
      .channel = channel,
      .architecture = architecture,
      .edition = edition,
      .form = form};
  return target.valid()
             ? std::optional<application::ApplicationBuildIdentity>{
                   std::move(target)}
                         : std::nullopt;
}

[[nodiscard]] application::GithubReleaseQueryResult parse_releases(
    std::string document) {
  try {
    auto wide_document = utf8_to_wide(document);
    if (!wide_document.has_value()) {
      return query_failed("GitHub release response is not valid UTF-8");
    }
    auto const root = winrt::Windows::Data::Json::JsonArray::Parse(
        winrt::hstring{*wide_document});
    std::vector<application::GithubApplicationRelease> releases;
    releases.reserve(root.Size());
    for (std::uint32_t index = 0; index < root.Size(); ++index) {
      auto const object = root.GetObjectAt(index);
      auto release_id = json_number_string(object, L"id");
      auto tag = json_string(object, L"tag_name", 128);
      if (!release_id.has_value() || !tag.has_value() ||
          !safe_release_tag(*tag)) {
        continue;
      }
      auto title = json_string(object, L"name", 512);
      auto body = json_string(object, L"body", 8192);
      auto published = json_string(object, L"published_at", 128);
      if (!title.has_value() || !body.has_value() || !published.has_value()) {
        return query_failed("GitHub release metadata contains an invalid text field");
      }
      if (title->empty()) *title = *tag;
      auto const draft = object.GetNamedBoolean(L"draft", false);
      auto const prerelease = object.GetNamedBoolean(L"prerelease", false);
      winrt::Windows::Data::Json::JsonArray assets;
      try {
        assets = object.GetNamedArray(L"assets");
      } catch (winrt::hresult_error const&) {
        continue;
      }
      std::vector<application::GithubApplicationAsset> parsed_assets;
      for (std::uint32_t asset_index = 0; asset_index < assets.Size();
           ++asset_index) {
        auto const asset_object = assets.GetObjectAt(asset_index);
        auto asset_id = json_number_string(asset_object, L"id");
        auto asset_name = json_string(asset_object, L"name", 256);
        auto download_url = json_string(asset_object, L"browser_download_url",
                                         2048);
        if (!asset_id.has_value() || !asset_name.has_value() ||
            !download_url.has_value()) {
          continue;
        }
        auto target = target_for_asset(*asset_name, *tag, prerelease);
        if (!target.has_value()) continue;
        if (!download_url->starts_with(
                "https://github.com/CoolPlume/azzs/releases/download/")) {
          continue;
        }
        parsed_assets.push_back({.asset_id = std::move(*asset_id),
                                 .target = std::move(*target),
                                 .name = std::move(*asset_name),
                                 .download_url = std::move(*download_url)});
      }
      if (parsed_assets.empty()) continue;
      auto const release_tag = *tag;
      releases.push_back({
          .release_id = std::move(*release_id),
          .tag_name = std::move(*tag),
          .title = std::move(*title),
          .draft = draft,
          .prerelease = prerelease,
          .assets = std::move(parsed_assets),
          .html_url = std::string{kManualGithubReleases} + "/tag/" + release_tag,
          .published_at = std::move(*published),
          .body = std::move(*body),
      });
    }
    return {.code = application::GithubReleaseQueryResultCode::succeeded,
            .releases = std::move(releases)};
  } catch (winrt::hresult_error const& error) {
    return query_failed("GitHub release JSON could not be parsed (" +
                        std::to_string(static_cast<std::uint32_t>(error.code())) +
                        ")");
  } catch (...) {
    return query_failed("GitHub release JSON could not be parsed");
  }
}

}  // namespace

WindowsApplicationUpdatePlatform::WindowsApplicationUpdatePlatform(
    application::PlatformInfo const& platform,
    application::ApplicationUpdateHealthStorage& health_storage)
    : platform_(platform), health_storage_(health_storage),
      owned_executor_(
          std::make_unique<WinHttpApplicationUpdateReleaseRequestExecutor>()),
      request_executor_(owned_executor_.get()) {}

WindowsApplicationUpdatePlatform::~WindowsApplicationUpdatePlatform() = default;

WindowsApplicationUpdatePlatform::WindowsApplicationUpdatePlatform(
    application::PlatformInfo const& platform,
    application::ApplicationUpdateHealthStorage& health_storage,
    ApplicationUpdateReleaseRequestExecutor& request_executor) noexcept
    : platform_(platform), health_storage_(health_storage),
      request_executor_(&request_executor) {}

WindowsApplicationUpdatePlatform WindowsApplicationUpdatePlatform::for_testing(
    application::PlatformInfo const& platform,
    application::ApplicationUpdateHealthStorage& health_storage,
    ApplicationUpdateReleaseRequestExecutor& request_executor) {
  return WindowsApplicationUpdatePlatform{platform, health_storage,
                                          request_executor};
}

application::ApplicationBuildIdentity
WindowsApplicationUpdatePlatform::current_build() const noexcept {
  return application::ApplicationBuildIdentity{
      .version = AZZS_APPLICATION_VERSION,
      .channel = *kApplicationReleaseChannel,
      .architecture = platform_.windows_architecture(),
      .edition = application::ApplicationReleaseEdition::standard,
      .form = application::ApplicationReleaseForm::portable,
  };
}

application::GithubReleaseQueryResult
WindowsApplicationUpdatePlatform::query_releases() {
  if (std::string_view{AZZS_APPLICATION_UPDATE_ENDPOINT} !=
      "https://api.github.com/repos/CoolPlume/azzs/releases?per_page=100") {
    return query_failed("GitHub release endpoint is not the controlled project endpoint");
  }
  if (request_executor_ == nullptr) {
    return query_failed("GitHub release request executor is unavailable");
  }
  auto document = request_executor_->execute({
      .method = ApplicationUpdateReleaseRequestMethod::get,
      .endpoint = AZZS_APPLICATION_UPDATE_ENDPOINT,
      .accept = kGithubAccept,
      .user_agent = kGithubUserAgent,
      .timeout = kRequestTimeout,
      .maximum_response_bytes = kMaximumResponseBytes,
  });
  if (document.code != application::GithubReleaseQueryResultCode::succeeded) {
    return {.code = document.code, .detail = std::move(document.detail)};
  }
  if (document.document.size() > kMaximumResponseBytes) {
    return query_failed("GitHub release response exceeds 4 MiB");
  }
  return parse_releases(std::move(document.document));
}

application::ApplicationUpdateEffect
WindowsApplicationUpdatePlatform::download_and_replace(
    application::ApplicationUpdateCandidate const&) {
  return {.code = application::UpdatePlatformResultCode::unavailable,
          .detail =
              "real Windows download, replacement and UAC flow is not implemented"};
}

application::ApplicationUpdateEffect
WindowsApplicationUpdatePlatform::retry_start(
    application::ApplicationUpdateHealthRecord const&) {
  return {.code = application::UpdatePlatformResultCode::unavailable,
          .detail = "real Windows application retry launch is not implemented"};
}

application::ApplicationUpdateEffect WindowsApplicationUpdatePlatform::rollback(
    application::ApplicationUpdateHealthRecord const&) {
  return {.code = application::UpdatePlatformResultCode::unavailable,
          .detail = "real Windows application rollback is not implemented"};
}

application::ApplicationUpdateHealthRead
WindowsApplicationUpdatePlatform::read_health_record() {
  return health_storage_.read();
}

application::UpdatePlatformResult
WindowsApplicationUpdatePlatform::write_health_record(
    application::ApplicationUpdateHealthRecord const& record) {
  return health_storage_.write(record);
}

application::UpdatePlatformResult
WindowsApplicationUpdatePlatform::clear_health_record() {
  return health_storage_.clear();
}

application::ApplicationUpdateStartHealth
WindowsApplicationUpdatePlatform::confirm_started_healthy(
    application::ApplicationUpdateHealthRecord const& record) {
  auto persisted = health_storage_.read();
  if (persisted.code != application::UpdatePlatformResultCode::succeeded) {
    return {.code = persisted.code,
            .records_remain_visible = false,
            .detail = persisted.detail.empty()
                          ? "application update health record is unavailable"
                          : std::move(persisted.detail)};
  }
  if (!persisted.record.has_value() || *persisted.record != record) {
    return {.code = application::UpdatePlatformResultCode::failed,
            .records_remain_visible = false,
            .detail = "application update health record is no longer visible"};
  }
  auto const& expected =
      record.phase == application::ApplicationUpdateHealthPhase::
                          previous_pending_start_health
          ? record.previous
          : record.target;
  if (current_build() != expected) {
    return {.code = application::UpdatePlatformResultCode::failed,
            .records_remain_visible = true,
            .detail =
                "running application build does not match the pending health record"};
  }
  return {.code = application::UpdatePlatformResultCode::succeeded,
          .records_remain_visible = true};
}

application::UpdatePlatformResult
WindowsApplicationUpdatePlatform::open_manual_download(
    application::ManualApplicationDownloadRequest const& request) {
  std::string target{kManualGithubReleases};
  if (request.route ==
          application::ManualApplicationDownloadRoute::matching_stable_release &&
      request.candidate.has_value() &&
      safe_release_tag(request.candidate->release_tag)) {
    target += "/tag/" + request.candidate->release_tag;
  }
  auto const opened = reinterpret_cast<std::intptr_t>(::ShellExecuteW(
      nullptr, L"open", wide_ascii(target).c_str(), nullptr, nullptr,
      SW_SHOWNORMAL));
  if (opened <= 32) {
    return {.code = application::UpdatePlatformResultCode::failed,
            .detail = "could not open controlled GitHub Releases entry"};
  }
  return {.code = application::UpdatePlatformResultCode::succeeded,
          .detail = "opened controlled GitHub Releases entry"};
}

}  // namespace azzs::adapters::windows
