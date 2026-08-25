#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "azzs/adapters/windows/windows_application_update_platform.hpp"

namespace {

using azzs::adapters::windows::ApplicationUpdateReleaseDocument;
using azzs::adapters::windows::ApplicationUpdateReleaseRequest;
using azzs::adapters::windows::ApplicationUpdateReleaseRequestExecutor;
using azzs::adapters::windows::ApplicationUpdateReleaseRequestMethod;
using azzs::adapters::windows::WindowsApplicationUpdatePlatform;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows application update platform contract failed: "
              << message << '\n';
  }
  return condition;
}

class FixedPlatformInfo final : public azzs::application::PlatformInfo {
 public:
  [[nodiscard]] std::optional<azzs::domain::SystemVersion> windows_version()
      const override {
    return std::nullopt;
  }

  [[nodiscard]] azzs::domain::SystemArchitecture windows_architecture()
      const override {
    return azzs::domain::SystemArchitecture::x64;
  }
};

class MemoryHealthStorage final
    : public azzs::application::ApplicationUpdateHealthStorage {
 public:
  [[nodiscard]] azzs::application::ApplicationUpdateHealthRead read() override {
    return {.code = azzs::application::UpdatePlatformResultCode::succeeded};
  }

  [[nodiscard]] azzs::application::UpdatePlatformResult write(
      azzs::application::ApplicationUpdateHealthRecord const&) override {
    return {.code = azzs::application::UpdatePlatformResultCode::succeeded};
  }

  [[nodiscard]] azzs::application::UpdatePlatformResult clear() override {
    return {.code = azzs::application::UpdatePlatformResultCode::succeeded};
  }
};

class RecordingRequestExecutor final
    : public ApplicationUpdateReleaseRequestExecutor {
 public:
  [[nodiscard]] ApplicationUpdateReleaseDocument execute(
      ApplicationUpdateReleaseRequest const& request) override {
    ++calls;
    last_request = request;
    return result;
  }

  unsigned calls{};
  ApplicationUpdateReleaseRequest last_request;
  ApplicationUpdateReleaseDocument result;
};

constexpr std::string_view kReleaseFixture{R"json(
[
  {
    "id": 375225788,
    "tag_name": "v0.1.1",
    "name": "v0.1.1",
    "draft": false,
    "prerelease": false,
    "published_at": "2026-08-23T00:00:00Z",
    "body": "Runtime projection fixture",
    "assets": [
      {
        "id": 526271309,
        "name": "Azzs-standard-x64-portable.zip",
        "browser_download_url": "https://github.com/CoolPlume/azzs/releases/download/v0.1.1/Azzs-standard-x64-portable.zip"
      }
    ]
  }
]
)json"};

[[nodiscard]] bool verify_success_projection() {
  FixedPlatformInfo platform_info;
  MemoryHealthStorage health_storage;
  RecordingRequestExecutor executor;
  executor.result = {
      .code = azzs::application::GithubReleaseQueryResultCode::succeeded,
      .document = std::string{kReleaseFixture},
  };
  auto platform = WindowsApplicationUpdatePlatform::for_testing(
      platform_info, health_storage, executor);

  auto const result = platform.query_releases();
  bool passed = expect(
      result.code == azzs::application::GithubReleaseQueryResultCode::succeeded &&
          executor.calls == 1,
      "the fixture must traverse query_releases without network access");
  passed &= expect(
      executor.last_request.method == ApplicationUpdateReleaseRequestMethod::get &&
          executor.last_request.endpoint ==
              std::string_view{AZZS_APPLICATION_UPDATE_EXPECTED_ENDPOINT} &&
          executor.last_request.accept == "application/vnd.github+json" &&
          executor.last_request.user_agent == "azzs-update-check" &&
          executor.last_request.timeout == std::chrono::seconds{10} &&
          executor.last_request.timeout ==
              WindowsApplicationUpdatePlatform::kRequestTimeout &&
          executor.last_request.maximum_response_bytes == 4U * 1024U * 1024U &&
          executor.last_request.maximum_response_bytes ==
              WindowsApplicationUpdatePlatform::kMaximumResponseBytes,
      "query_releases must keep its fixed GitHub request contract");
  passed &= expect(
      result.releases.size() == 1 && result.releases.front().release_id == "375225788" &&
          result.releases.front().tag_name == "v0.1.1" &&
          !result.releases.front().draft && !result.releases.front().prerelease &&
          result.releases.front().assets.size() == 1 &&
          result.releases.front().assets.front().asset_id == "526271309" &&
          result.releases.front().assets.front().name ==
              "Azzs-standard-x64-portable.zip",
      "the real release fixture must project its matching asset");
  return passed;
}

[[nodiscard]] bool verify_failure_mapping() {
  FixedPlatformInfo platform_info;
  MemoryHealthStorage health_storage;
  RecordingRequestExecutor executor;
  auto platform = WindowsApplicationUpdatePlatform::for_testing(
      platform_info, health_storage, executor);

  executor.result = {
      .code = azzs::application::GithubReleaseQueryResultCode::unavailable,
      .detail = "simulated temporary failure",
  };
  auto const unavailable = platform.query_releases();
  bool passed = expect(
      unavailable.code == azzs::application::GithubReleaseQueryResultCode::unavailable &&
          unavailable.detail == "simulated temporary failure" && executor.calls == 1,
      "temporary transport failures must remain unavailable");

  executor.result = {
      .code = azzs::application::GithubReleaseQueryResultCode::failed,
      .detail = "simulated permanent failure",
  };
  auto const failed = platform.query_releases();
  passed &= expect(
      failed.code == azzs::application::GithubReleaseQueryResultCode::failed &&
          failed.detail == "simulated permanent failure" && executor.calls == 2,
      "non-temporary transport failures must remain failed");
  return passed;
}

[[nodiscard]] bool verify_live_github_release_query() {
  FixedPlatformInfo platform_info;
  MemoryHealthStorage health_storage;
  WindowsApplicationUpdatePlatform platform{platform_info, health_storage};

  auto const result = platform.query_releases();
  bool passed = expect(
      result.code == azzs::application::GithubReleaseQueryResultCode::succeeded,
      "the production WinHTTP executor must query the GitHub Releases API");
  passed &= expect(
      !result.releases.empty(),
      "the production GitHub response must contain a parseable release");
  if (passed) {
    std::cout << "live GitHub Releases query succeeded: releases="
              << result.releases.size() << '\n';
  } else if (!result.detail.empty()) {
    std::cerr << "live GitHub Releases query detail: " << result.detail << '\n';
  }
  return passed;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view{argv[1]} == "--live") {
    return verify_live_github_release_query() ? EXIT_SUCCESS : EXIT_FAILURE;
  }
  if (argc != 1) {
    std::cerr << "usage: azzs_windows_application_update_platform_contract"
              << " [--live]\n";
    return EXIT_FAILURE;
  }

  bool passed = true;
  passed &= verify_success_projection();
  passed &= verify_failure_mapping();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
