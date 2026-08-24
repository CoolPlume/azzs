#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "azzs/application/application_update.hpp"
#include "azzs/application/platform_info.hpp"

namespace azzs::adapters::windows {

enum class ApplicationUpdateReleaseRequestMethod {
  get,
};

struct ApplicationUpdateReleaseRequest final {
  ApplicationUpdateReleaseRequestMethod method{
      ApplicationUpdateReleaseRequestMethod::get};
  std::string_view endpoint;
  std::string_view accept;
  std::string_view user_agent;
  std::chrono::milliseconds timeout;
  std::size_t maximum_response_bytes{};
};

struct ApplicationUpdateReleaseDocument final {
  application::GithubReleaseQueryResultCode code{
      application::GithubReleaseQueryResultCode::failed};
  std::string document;
  std::string detail;
};

class ApplicationUpdateReleaseRequestExecutor {
 public:
  virtual ~ApplicationUpdateReleaseRequestExecutor() = default;

  [[nodiscard]] virtual ApplicationUpdateReleaseDocument execute(
      ApplicationUpdateReleaseRequest const& request) = 0;
};

class WindowsApplicationUpdatePlatform final
    : public application::ApplicationUpdatePlatform {
 public:
  static constexpr std::chrono::milliseconds kRequestTimeout{10'000};
  static constexpr std::size_t kMaximumResponseBytes{4U * 1024U * 1024U};

  explicit WindowsApplicationUpdatePlatform(
      application::PlatformInfo const& platform,
      application::ApplicationUpdateHealthStorage& health_storage);
  ~WindowsApplicationUpdatePlatform() override;

  // This factory exists solely for no-network adapter contract tests. The
  // production constructor still uses the fixed build-configured endpoint.
  [[nodiscard]] static WindowsApplicationUpdatePlatform for_testing(
      application::PlatformInfo const& platform,
      application::ApplicationUpdateHealthStorage& health_storage,
      ApplicationUpdateReleaseRequestExecutor& request_executor);

  [[nodiscard]] application::ApplicationBuildIdentity current_build()
      const noexcept override;
  [[nodiscard]] application::GithubReleaseQueryResult query_releases() override;
  [[nodiscard]] application::ApplicationUpdateEffect download_and_replace(
      application::ApplicationUpdateCandidate const& candidate) override;
  [[nodiscard]] application::ApplicationUpdateEffect retry_start(
      application::ApplicationUpdateHealthRecord const& record) override;
  [[nodiscard]] application::ApplicationUpdateEffect rollback(
      application::ApplicationUpdateHealthRecord const& record) override;
  [[nodiscard]] application::ApplicationUpdateHealthRead read_health_record()
      override;
  [[nodiscard]] application::UpdatePlatformResult write_health_record(
      application::ApplicationUpdateHealthRecord const& record) override;
  [[nodiscard]] application::UpdatePlatformResult clear_health_record()
      override;
  [[nodiscard]] application::ApplicationUpdateStartHealth confirm_started_healthy(
      application::ApplicationUpdateHealthRecord const& record) override;
  [[nodiscard]] application::UpdatePlatformResult open_manual_download(
      application::ManualApplicationDownloadRequest const& request) override;

 private:
  WindowsApplicationUpdatePlatform(
      application::PlatformInfo const& platform,
      application::ApplicationUpdateHealthStorage& health_storage,
      ApplicationUpdateReleaseRequestExecutor& request_executor) noexcept;

  application::PlatformInfo const& platform_;
  application::ApplicationUpdateHealthStorage& health_storage_;
  std::unique_ptr<ApplicationUpdateReleaseRequestExecutor> owned_executor_;
  ApplicationUpdateReleaseRequestExecutor* request_executor_{};
};

}  // namespace azzs::adapters::windows
