#pragma once

#include "azzs/application/application_update.hpp"
#include "azzs/application/platform_info.hpp"

namespace azzs::adapters::windows {

class WindowsApplicationUpdatePlatform final
    : public application::ApplicationUpdatePlatform {
 public:
  explicit WindowsApplicationUpdatePlatform(
      application::PlatformInfo const& platform,
      application::ApplicationUpdateHealthStorage& health_storage) noexcept;

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
  application::PlatformInfo const& platform_;
  application::ApplicationUpdateHealthStorage& health_storage_;
};

}  // namespace azzs::adapters::windows
