#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "azzs/adapters/windows/windows_emergency_withdrawal_notice_source.hpp"

namespace {

using azzs::adapters::windows::EmergencyWithdrawalNoticeRequest;
using azzs::adapters::windows::EmergencyWithdrawalNoticeRequestExecutor;
using azzs::adapters::windows::EmergencyWithdrawalNoticeRequestMethod;
using azzs::adapters::windows::WindowsEmergencyWithdrawalNoticeSource;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows emergency withdrawal notice contract failed: "
              << message << '\n';
  }
  return condition;
}

class RecordingRequestExecutor final
    : public EmergencyWithdrawalNoticeRequestExecutor {
 public:
  [[nodiscard]] azzs::application::NoticeFetchResult execute(
      EmergencyWithdrawalNoticeRequest const& request) override {
    ++calls;
    last_request = request;
    return result;
  }

  unsigned calls{};
  EmergencyWithdrawalNoticeRequest last_request;
  azzs::application::NoticeFetchResult result;
};

[[nodiscard]] bool verify_fixed_release_endpoint() {
  auto const endpoint = std::string_view{AZZS_NOTICE_SOURCE_EXPECTED_ENDPOINT};
  return expect(
             endpoint ==
                 "https://raw.githubusercontent.com/CoolPlume/azzs/main/release/emergency-withdrawal-notice.txt",
             "the release source must compile the fixed project notice endpoint") &&
         expect(endpoint.starts_with("https://"),
                "the release source endpoint must remain HTTPS");
}

[[nodiscard]] bool verify_unconfigured_and_configured_contract() {
  RecordingRequestExecutor executor;
  auto unconfigured =
      WindowsEmergencyWithdrawalNoticeSource::for_testing("", executor);
  auto const unconfigured_result = unconfigured.fetch();
  bool passed = expect(
      !unconfigured_result.succeeded && executor.calls == 0 &&
          unconfigured_result.error ==
              "emergency withdrawal release endpoint is not configured",
      "an unconfigured endpoint must fail without invoking the transport");

  executor.result = {.succeeded = true,
                     .document = "source=project-security\nrevision=1\n"};
  auto source = WindowsEmergencyWithdrawalNoticeSource::for_testing(executor);
  auto const configured_result = source.fetch();
  passed &= expect(
      configured_result.succeeded && executor.calls == 1 &&
          executor.last_request.method == EmergencyWithdrawalNoticeRequestMethod::get &&
          executor.last_request.endpoint ==
              std::string_view{AZZS_NOTICE_SOURCE_EXPECTED_ENDPOINT} &&
          executor.last_request.timeout == std::chrono::seconds{15} &&
          executor.last_request.timeout ==
              WindowsEmergencyWithdrawalNoticeSource::kRequestTimeout &&
          executor.last_request.maximum_response_bytes == 4U * 1024U * 1024U &&
          executor.last_request.maximum_response_bytes ==
              WindowsEmergencyWithdrawalNoticeSource::kMaximumDocumentBytes,
      "a configured endpoint must issue the fixed HTTPS GET request contract");
  return passed;
}

[[nodiscard]] bool verify_rejected_and_failed_boundaries() {
  RecordingRequestExecutor executor;
  auto insecure = WindowsEmergencyWithdrawalNoticeSource::for_testing(
      "http://release.example.invalid/emergency-withdrawals-v1", executor);
  auto const insecure_result = insecure.fetch();
  bool passed = expect(
      !insecure_result.succeeded && executor.calls == 0 &&
          insecure_result.error ==
              "emergency withdrawal release endpoint must use HTTPS",
      "a non-HTTPS endpoint must not invoke the transport");

  auto source = WindowsEmergencyWithdrawalNoticeSource::for_testing(
      "https://release.example.invalid/emergency-withdrawals-v1", executor);
  executor.result = {.error = "simulated transport failure"};
  auto const failed_result = source.fetch();
  passed &= expect(
      !failed_result.succeeded && executor.calls == 1 &&
          failed_result.error == "simulated transport failure",
      "a non-network transport failure must cross the adapter boundary explicitly");

  azzs::application::NoticeFetchResult oversized_response;
  oversized_response.succeeded = true;
  oversized_response.document = std::string(
      WindowsEmergencyWithdrawalNoticeSource::kMaximumDocumentBytes + 1U,
      'x');
  executor.result = oversized_response;
  auto const oversized_result = source.fetch();
  passed &= expect(
      !oversized_result.succeeded && executor.calls == 2 &&
          oversized_result.error ==
              "emergency withdrawal notice response exceeds 4 MiB",
      "responses above the fixed 4 MiB limit must fail before the core");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_fixed_release_endpoint();
  passed &= verify_unconfigured_and_configured_contract();
  passed &= verify_rejected_and_failed_boundaries();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
