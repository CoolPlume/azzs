#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

#include "azzs/application/emergency_withdrawal_service.hpp"

namespace azzs::adapters::windows {

enum class EmergencyWithdrawalNoticeRequestMethod {
  get,
};

struct EmergencyWithdrawalNoticeRequest final {
  EmergencyWithdrawalNoticeRequestMethod method{
      EmergencyWithdrawalNoticeRequestMethod::get};
  std::string_view endpoint;
  std::chrono::milliseconds timeout;
  std::size_t maximum_response_bytes{};
};

class EmergencyWithdrawalNoticeRequestExecutor {
 public:
  virtual ~EmergencyWithdrawalNoticeRequestExecutor() = default;

  [[nodiscard]] virtual application::NoticeFetchResult execute(
      EmergencyWithdrawalNoticeRequest const& request) = 0;
};

// Reads only the release-configured emergency notice endpoint. The endpoint is
// compiled into the Windows adapter; no user, catalog, or runtime URL is read.
class WindowsEmergencyWithdrawalNoticeSource final
    : public application::EmergencyWithdrawalNoticeSource {
 public:
  static constexpr std::chrono::milliseconds kRequestTimeout{15'000};
  static constexpr std::size_t kMaximumDocumentBytes{4U * 1024U * 1024U};

  WindowsEmergencyWithdrawalNoticeSource();
  ~WindowsEmergencyWithdrawalNoticeSource() override;

  WindowsEmergencyWithdrawalNoticeSource(
      WindowsEmergencyWithdrawalNoticeSource const&) = delete;
  WindowsEmergencyWithdrawalNoticeSource& operator=(
      WindowsEmergencyWithdrawalNoticeSource const&) = delete;

  // This factory exists solely for no-network adapter contract tests. The
  // production constructor still obtains its endpoint only from build config.
  [[nodiscard]] static WindowsEmergencyWithdrawalNoticeSource for_testing(
      EmergencyWithdrawalNoticeRequestExecutor& executor);
  [[nodiscard]] static WindowsEmergencyWithdrawalNoticeSource for_testing(
      std::string release_endpoint,
      EmergencyWithdrawalNoticeRequestExecutor& executor);

  [[nodiscard]] application::NoticeFetchResult fetch() override;

 private:
  WindowsEmergencyWithdrawalNoticeSource(
      std::string release_endpoint,
      EmergencyWithdrawalNoticeRequestExecutor& executor);

  std::string release_endpoint_;
  std::unique_ptr<EmergencyWithdrawalNoticeRequestExecutor> owned_executor_;
  EmergencyWithdrawalNoticeRequestExecutor* executor_{};
};

}  // namespace azzs::adapters::windows
