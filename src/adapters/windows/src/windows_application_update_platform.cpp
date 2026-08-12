#include "azzs/adapters/windows/windows_application_update_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

namespace azzs::adapters::windows {
namespace {

constexpr std::string_view kManualGithubReleases{
    "https://github.com/CoolPlume/azzs/releases"};

#ifndef AZZS_APPLICATION_VERSION
#define AZZS_APPLICATION_VERSION "0.1.0"
#endif

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

}  // namespace

WindowsApplicationUpdatePlatform::WindowsApplicationUpdatePlatform(
    application::PlatformInfo const& platform,
    application::ApplicationUpdateHealthStorage& health_storage) noexcept
    : platform_(platform), health_storage_(health_storage) {}

application::ApplicationBuildIdentity
WindowsApplicationUpdatePlatform::current_build() const noexcept {
  return application::ApplicationBuildIdentity{
      .version = AZZS_APPLICATION_VERSION,
      .channel = application::ApplicationReleaseChannel::stable,
      .architecture = platform_.windows_architecture(),
      .edition = application::ApplicationReleaseEdition::standard,
      .form = application::ApplicationReleaseForm::portable,
  };
}

application::GithubReleaseQueryResult
WindowsApplicationUpdatePlatform::query_releases() {
  return {.code = application::GithubReleaseQueryResultCode::unavailable,
          .detail =
              "GitHub release query adapter is typed but not connected in this build"};
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
