#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "azzs/application/hardware_overview.hpp"
#include "azzs/testing/fixed_clock.hpp"

namespace {

using namespace std::chrono_literals;
using azzs::application::HardwareObservation;
using azzs::application::HardwareObservationCode;
using azzs::application::HardwareObservationResult;
using azzs::application::HardwareObserver;
using azzs::application::HardwareOverviewService;
using azzs::application::HardwareOverviewState;
using azzs::application::HardwareOverviewTrigger;
using azzs::testing::FixedClock;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "hardware overview contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] HardwareObservation observation(
    std::string cpu = "AMD Ryzen 7",
    std::string gpu = "NVIDIA GeForce RTX",
    std::string motherboard = "ASUS PRIME",
    std::string network_adapter = "Intel Ethernet",
    std::string oem_model = "ASUS Desktop") {
  return {
      .cpu = std::move(cpu),
      .gpu = std::move(gpu),
      .motherboard = std::move(motherboard),
      .network_adapter = std::move(network_adapter),
      .oem_model = std::move(oem_model),
  };
}

class FakeHardwareObserver final : public HardwareObserver {
 public:
  std::vector<HardwareObservationResult> results;
  std::optional<HardwareObservation> current_model;
  std::size_t observe_calls{0};
  std::size_t current_model_calls{0};
  std::size_t next_result{0};

  [[nodiscard]] HardwareObservationResult observe(
      std::stop_token cancellation) override {
    ++observe_calls;
    if (cancellation.stop_requested()) {
      return {.code = HardwareObservationCode::cancelled,
              .error = "cancelled by test"};
    }
    if (next_result < results.size()) {
      return results[next_result++];
    }
    return {.code = HardwareObservationCode::failed,
            .error = "no fake observation configured"};
  }

  [[nodiscard]] std::optional<HardwareObservation> current_model_observation(
      std::stop_token cancellation) override {
    ++current_model_calls;
    if (cancellation.stop_requested()) {
      return std::nullopt;
    }
    return current_model;
  }
};

[[nodiscard]] bool successful_and_partial_observations_are_presented() {
  FixedClock clock{azzs::application::WallClockTime{1'000ms}};
  FakeHardwareObserver observer;
  observer.results = {
      {.code = HardwareObservationCode::succeeded,
       .observation = observation()},
      {.code = HardwareObservationCode::partial,
       .observation = observation({}, "Intel Arc", {}, {}, "OEM")},
  };
  HardwareOverviewService service{observer, clock};

  auto const first =
      service.observe(HardwareOverviewTrigger::page_entered);
  auto const partial = service.refresh();

  return expect(first.state == HardwareOverviewState::ready &&
                    first.observation.has_value() &&
                    first.observation->cpu == "AMD Ryzen 7" &&
                    first.observation->gpu == "NVIDIA GeForce RTX",
                "a successful observation must expose all supplied model facts") &&
         expect(partial.state == HardwareOverviewState::ready &&
                    partial.observation.has_value() &&
                    partial.observation->gpu == "Intel Arc" &&
                    partial.observation->cpu.empty(),
                "a partial observation with usable facts must remain available");
}

[[nodiscard]] bool unsuccessful_observations_clear_cached_models() {
  FixedClock clock{azzs::application::WallClockTime{2'000ms}};
  FakeHardwareObserver observer;
  observer.results = {
      {.code = HardwareObservationCode::succeeded,
       .observation = observation()},
      {.code = HardwareObservationCode::failed, .error = "WMI failed"},
      {.code = HardwareObservationCode::permission_denied},
      {.code = HardwareObservationCode::timed_out},
  };
  HardwareOverviewService service{observer, clock};
  auto const initial =
      service.observe(HardwareOverviewTrigger::page_entered);
  auto const failed = service.refresh();
  auto const denied = service.refresh();
  auto const timed_out = service.refresh();

  return expect(initial.state == HardwareOverviewState::ready,
                "the cache-clearing fixture must begin with a usable model") &&
         expect(failed.state == HardwareOverviewState::unrecognized &&
                    !failed.observation.has_value(),
                "a failed refresh must replace the old model with unrecognized") &&
         expect(denied.state == HardwareOverviewState::unrecognized &&
                    !denied.observation.has_value(),
                "permission denial must present unrecognized") &&
         expect(timed_out.state == HardwareOverviewState::unrecognized &&
                    !timed_out.observation.has_value(),
                "a timeout must present unrecognized");
}

[[nodiscard]] bool cancellation_presents_unrecognized_without_probing() {
  FixedClock clock{azzs::application::WallClockTime{3'000ms}};
  FakeHardwareObserver observer;
  observer.results = {
      {.code = HardwareObservationCode::succeeded,
       .observation = observation()},
  };
  HardwareOverviewService service{observer, clock};
  std::stop_source cancellation;
  cancellation.request_stop();

  auto const snapshot =
      service.observe(HardwareOverviewTrigger::page_entered,
                      cancellation.get_token());

  return expect(snapshot.state == HardwareOverviewState::unrecognized &&
                    !snapshot.observation.has_value(),
                "a cancelled request must present unrecognized") &&
         expect(observer.observe_calls == 0 && observer.current_model_calls == 0,
                "a pre-cancelled request must not start a probe");
}

[[nodiscard]] bool cache_expires_refreshes_and_detects_model_changes() {
  FixedClock clock{azzs::application::WallClockTime{4'000ms}};
  auto const first = observation();
  auto const changed = observation("AMD Ryzen 9");
  FakeHardwareObserver observer;
  observer.results = {
      {.code = HardwareObservationCode::succeeded, .observation = first},
      {.code = HardwareObservationCode::succeeded, .observation = first},
      {.code = HardwareObservationCode::succeeded, .observation = first},
      {.code = HardwareObservationCode::succeeded, .observation = changed},
  };
  observer.current_model = first;
  HardwareOverviewService service{observer, clock};

  auto const detected =
      service.observe(HardwareOverviewTrigger::page_entered);
  auto const cached =
      service.observe(HardwareOverviewTrigger::page_entered);
  auto const refreshed = service.refresh();
  clock.advance(HardwareOverviewService::kSessionCacheLifetime);
  auto const expired =
      service.observe(HardwareOverviewTrigger::page_entered);
  observer.current_model = changed;
  auto const hardware_changed =
      service.observe(HardwareOverviewTrigger::page_entered);

  return expect(detected.state == HardwareOverviewState::ready &&
                    !detected.cache_hit && observer.observe_calls == 4,
                "the sequence must perform the initial, refresh, expiry, and "
                "hardware-change observations") &&
         expect(cached.state == HardwareOverviewState::ready && cached.cache_hit,
                "a matching model inside ten minutes must use the session cache") &&
         expect(refreshed.state == HardwareOverviewState::ready &&
                    !refreshed.cache_hit,
                "a user refresh must bypass the session cache") &&
         expect(expired.state == HardwareOverviewState::ready &&
                    !expired.cache_hit,
                "a ten-minute-old result must refresh on page entry") &&
         expect(hardware_changed.state == HardwareOverviewState::ready &&
                    !hardware_changed.cache_hit &&
                    hardware_changed.observation.has_value() &&
                    hardware_changed.observation->cpu == "AMD Ryzen 9",
                "a changed model probe must invalidate and replace the cache") &&
         expect(observer.current_model_calls == 2,
                "only cache revisits before expiry and after refresh may probe "
                "the model fingerprint");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= successful_and_partial_observations_are_presented();
  passed &= unsuccessful_observations_clear_cached_models();
  passed &= cancellation_presents_unrecognized_without_probing();
  passed &= cache_expires_refreshes_and_detects_model_changes();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
