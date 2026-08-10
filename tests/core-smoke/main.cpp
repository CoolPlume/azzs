#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>

#include "azzs/application/bounded_cpu_scheduler.hpp"
#include "azzs/application/page_id.hpp"
#include "azzs/application/workbench.hpp"
#include "azzs/domain/minimum_version_policy.hpp"
#include "azzs/domain/system_version.hpp"
#include "azzs/testing/fixed_platform_info.hpp"

namespace {

using azzs::application::BoundedCpuScheduler;
using azzs::application::CpuComputationState;
using azzs::application::PageId;
using azzs::application::Workbench;
using azzs::domain::MinimumVersionRisk;
using azzs::domain::SystemVersion;
using azzs::testing::FixedPlatformInfo;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "core smoke failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool verify_scheduler_limit_and_order() {
  bool passed = true;

  BoundedCpuScheduler const detected_scheduler;
  BoundedCpuScheduler const oversized_scheduler{
      std::numeric_limits<std::size_t>::max()};
  passed &= expect(detected_scheduler.maximum_concurrency() >= 1,
                   "the CPU scheduler must retain at least one worker");
  auto const logical_processors =
      std::max<std::size_t>(1, std::thread::hardware_concurrency());
  if (logical_processors > 1) {
    passed &= expect(
        detected_scheduler.maximum_concurrency() < logical_processors,
        "CPU work must leave a logical processor available for responsiveness");
  }
  passed &= expect(
      oversized_scheduler.maximum_concurrency() ==
          detected_scheduler.maximum_concurrency(),
      "the CPU scheduler must not exceed available logical processors");

  BoundedCpuScheduler const scheduler{2};
  std::atomic_size_t active{0};
  std::atomic_size_t peak{0};
  constexpr std::array inputs{4, 1, 3, 2};

  auto const batch = scheduler.transform(
      std::span<int const>{inputs},
      [&](int input, std::stop_token) {
        auto const current = active.fetch_add(1) + 1;
        auto observed_peak = peak.load();
        while (current > observed_peak &&
               !peak.compare_exchange_weak(observed_peak, current)) {
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(input));
        active.fetch_sub(1);
        return input * input;
      });

  passed &= expect(peak.load() <= scheduler.maximum_concurrency(),
                   "active CPU work must stay within the configured limit");
  passed &= expect(batch.results.size() == inputs.size(),
                   "each CPU input must have one ordered result");
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    auto const& result = batch.results[index];
    passed &= expect(result.state == CpuComputationState::completed,
                     "independent CPU work must complete");
    passed &= expect(result.value == inputs[index] * inputs[index],
                     "CPU results must retain input order");
    passed &= expect(result.error == nullptr,
                     "completed CPU work must not report an exception");
  }

  return passed;
}

[[nodiscard]] bool verify_scheduler_cancellation() {
  bool passed = true;
  BoundedCpuScheduler const serial_scheduler{1};
  passed &= expect(serial_scheduler.maximum_concurrency() == 1,
                   "a one-worker scheduler must provide the serial test seam");

  constexpr std::array inputs{1, 2, 3};
  std::stop_source cancellation;
  std::promise<void> started;
  auto started_future = started.get_future();
  std::atomic_size_t calls{0};
  std::optional<azzs::application::CpuComputationBatch<int>> batch;

  std::jthread runner{[&] {
    batch = serial_scheduler.transform(
        std::span<int const>{inputs},
        [&](int input, std::stop_token stop) {
          calls.fetch_add(1);
          started.set_value();
          while (!stop.stop_requested()) {
            std::this_thread::yield();
          }
          return input;
        },
        cancellation.get_token());
  }};

  if (started_future.wait_for(std::chrono::seconds(5)) !=
      std::future_status::ready) {
    cancellation.request_stop();
    runner.join();
    return expect(false,
                  "cancelled CPU work must start within the test timeout");
  }
  cancellation.request_stop();
  runner.join();

  passed &= expect(calls.load() == 1,
                   "cancellation must prevent queued CPU work from starting");
  passed &= expect(batch.has_value(),
                   "a cancelled batch must still return observable results");
  if (batch.has_value()) {
    for (auto const& result : batch->results) {
      passed &= expect(result.state == CpuComputationState::cancelled,
                       "cancelled CPU work must have an explicit state");
      passed &= expect(!result.value.has_value(),
                       "cancelled CPU work must not publish a value");
      passed &= expect(result.error == nullptr,
                       "cancellation must not be reported as an exception");
    }
  }

  return passed;
}

[[nodiscard]] bool verify_scheduler_exception() {
  bool passed = true;
  BoundedCpuScheduler const serial_scheduler{1};
  constexpr std::array inputs{1, 2, 3};

  auto const batch = serial_scheduler.transform(
      std::span<int const>{inputs},
      [](int input, std::stop_token) {
        if (input == 2) {
          throw std::runtime_error{"expected CPU failure"};
        }
        return input * 10;
      });

  passed &= expect(
      batch.results[0].state == CpuComputationState::completed &&
          batch.results[0].value == 10,
      "work before an exception must retain its result");
  passed &= expect(batch.results[1].state == CpuComputationState::failed,
                   "a work exception must be observable per input");
  passed &= expect(batch.results[1].error != nullptr,
                   "failed CPU work must retain its exception");
  passed &= expect(
      batch.results[2].state == CpuComputationState::completed &&
          batch.results[2].value == 30,
      "independent work after an exception must still complete");

  bool matched_exception = false;
  if (batch.results[1].error != nullptr) {
    try {
      std::rethrow_exception(batch.results[1].error);
    } catch (std::runtime_error const& error) {
      matched_exception =
          std::string_view{error.what()} == "expected CPU failure";
    } catch (...) {
    }
  }
  passed &= expect(matched_exception,
                   "the original CPU exception must be preserved");

  return passed;
}

}  // namespace

int main() {
  bool passed = true;

  FixedPlatformInfo const supported_platform{SystemVersion{10, 0, 19045}};
  Workbench supported_workbench{supported_platform};
  auto snapshot = supported_workbench.snapshot();

  passed &= expect(snapshot.current_page == PageId::overview,
                   "the default page must be overview");
  passed &= expect(snapshot.minimum_version_risk == MinimumVersionRisk::none,
                   "Windows 10 22H2 must meet the target");

  constexpr std::array pages{
      PageId::overview,
      PageId::drivers,
      PageId::system_optimization,
      PageId::software_installation,
      PageId::software_optimization,
      PageId::history_and_logs,
      PageId::application_settings,
  };

  for (auto const page : pages) {
    supported_workbench.navigate(page);
    passed &= expect(supported_workbench.snapshot().current_page == page,
                     "each top-level page must be directly navigable");
  }

  FixedPlatformInfo const older_platform{SystemVersion{10, 0, 18363}};
  Workbench older_workbench{older_platform};
  passed &= expect(
      older_workbench.snapshot().minimum_version_risk ==
          MinimumVersionRisk::earlier_than_target,
      "an earlier Windows version must project a warning");
  older_workbench.navigate(PageId::drivers);
  passed &= expect(older_workbench.snapshot().current_page == PageId::drivers,
                   "a minimum-version warning must not block navigation");

  FixedPlatformInfo const unavailable_platform{std::nullopt};
  Workbench unavailable_workbench{unavailable_platform};
  passed &= expect(
      unavailable_workbench.snapshot().minimum_version_risk ==
          MinimumVersionRisk::version_unavailable,
      "an unavailable version must remain an explicit risk state");

  passed &= verify_scheduler_limit_and_order();
  passed &= verify_scheduler_cancellation();
  passed &= verify_scheduler_exception();

  if (!passed) {
    return EXIT_FAILURE;
  }

  std::cout << "core smoke passed\n";
  return EXIT_SUCCESS;
}
