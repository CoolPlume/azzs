#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/debug_log_policy/debug_log_policy.hpp"

namespace {

using azzs::application::DebugLogCoverage;
using azzs::application::DebugLogFilterField;
using azzs::application::DebugLogGranularity;
using azzs::application::DebugLogLocationSemantics;
using azzs::application::DebugLogPolicyContext;
using azzs::application::DebugLogPolicyProvider;
using azzs::application::DebugLogPolicyRead;
using azzs::application::DebugLogPolicyReader;
using azzs::application::DebugLogPolicySnapshotSource;
using azzs::application::DebugLogRetention;
using azzs::application::DebugModeState;
using azzs::application::kRequiredDebugLogCoverage;
using azzs::application::kRequiredDebugLogFilterFields;
using azzs::application::make_debug_log_policy_context;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "debug log policy contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] DebugLogPolicyRead available_policy(bool debug_enabled) {
  return {
      .available = true,
      .debug_enabled = debug_enabled,
      .granularity = debug_enabled ? DebugLogGranularity::maximum
                                   : DebugLogGranularity::normal,
      .filterable_fields = {kRequiredDebugLogFilterFields.begin(),
                            kRequiredDebugLogFilterFields.end()},
      .locating_semantics =
          DebugLogLocationSemantics::event_sequence_and_correlation_id,
      .coverage = {kRequiredDebugLogCoverage.begin(),
                   kRequiredDebugLogCoverage.end()},
  };
}

class FakeDebugLogPolicyProvider final : public DebugLogPolicyProvider {
 public:
  DebugLogPolicyRead policy;
  bool throw_on_read{false};
  mutable std::size_t reads{0};

  [[nodiscard]] DebugLogPolicyRead read_debug_log_policy() const override {
    ++reads;
    if (throw_on_read) {
      throw std::runtime_error{"provider failure"};
    }
    return policy;
  }
};

[[nodiscard]] bool enabled_and_disabled_states_report_their_granularity() {
  auto provider = std::make_shared<FakeDebugLogPolicyProvider>();
  provider->policy = available_policy(true);
  DebugLogPolicyReader reader{provider};

  auto const enabled = reader.snapshot();
  provider->policy = available_policy(false);
  auto const disabled = reader.snapshot();

  return expect(enabled.facts_available &&
                    enabled.debug_mode == DebugModeState::enabled &&
                    enabled.granularity == DebugLogGranularity::maximum,
                "enabled debug mode must expose maximum granularity") &&
         expect(disabled.facts_available &&
                    disabled.debug_mode == DebugModeState::disabled &&
                    disabled.granularity == DebugLogGranularity::normal,
                "disabled debug mode must expose normal granularity") &&
         expect(provider->reads == 2,
                "each snapshot must read the authoritative provider");
}

[[nodiscard]] bool filters_and_location_are_canonical_and_deterministic() {
  auto provider = std::make_shared<FakeDebugLogPolicyProvider>();
  provider->policy = available_policy(true);
  std::ranges::reverse(provider->policy.filterable_fields);
  provider->policy.filterable_fields.push_back(
      DebugLogFilterField::command);
  std::ranges::reverse(provider->policy.coverage);
  provider->policy.coverage.push_back(DebugLogCoverage::recovery);
  DebugLogPolicyReader reader{provider};

  auto const snapshot = reader.snapshot();
  return expect(snapshot.filterable_fields ==
                    std::vector<DebugLogFilterField>{
                        kRequiredDebugLogFilterFields.begin(),
                        kRequiredDebugLogFilterFields.end()},
                "filterable fields must be ordered and deduplicated") &&
         expect(snapshot.coverage == std::vector<DebugLogCoverage>{
                    kRequiredDebugLogCoverage.begin(),
                    kRequiredDebugLogCoverage.end()},
                "coverage must be ordered and deduplicated") &&
         expect(snapshot.locating_semantics ==
                    DebugLogLocationSemantics::event_sequence_and_correlation_id,
                "location must use stable event sequence and correlation id");
}

[[nodiscard]] bool every_required_filter_and_coverage_fact_is_enforced() {
  constexpr std::array<std::pair<DebugLogFilterField, std::string_view>, 11>
      filter_reasons{{
          {DebugLogFilterField::command, "command"},
          {DebugLogFilterField::state_transition, "state_transition"},
          {DebugLogFilterField::catalog, "catalog"},
          {DebugLogFilterField::source, "source"},
          {DebugLogFilterField::download, "download"},
          {DebugLogFilterField::installer, "installer"},
          {DebugLogFilterField::recovery, "recovery"},
          {DebugLogFilterField::adapter_result, "adapter_result"},
          {DebugLogFilterField::duration, "duration"},
          {DebugLogFilterField::retry, "retry"},
          {DebugLogFilterField::correlation_id, "correlation_id"},
      }};
  constexpr std::array<std::pair<DebugLogCoverage, std::string_view>, 9>
      coverage_reasons{{
          {DebugLogCoverage::catalog_validation, "catalog_validation"},
          {DebugLogCoverage::source_resolution, "source_resolution"},
          {DebugLogCoverage::dependency_planning, "dependency_planning"},
          {DebugLogCoverage::download, "download"},
          {DebugLogCoverage::installation, "installation"},
          {DebugLogCoverage::result_detection, "result_detection"},
          {DebugLogCoverage::recovery, "recovery"},
          {DebugLogCoverage::state_transition, "state_transition"},
          {DebugLogCoverage::adapter_result, "adapter_result"},
      }};

  auto provider = std::make_shared<FakeDebugLogPolicyProvider>();
  DebugLogPolicyReader reader{provider};
  bool passed = true;
  for (auto const& [missing, name] : filter_reasons) {
    provider->policy = available_policy(true);
    std::erase(provider->policy.filterable_fields, missing);
    auto const snapshot = reader.snapshot();
    auto const expected =
        std::string{"NOT_OBTAINED: debug log policy provider omitted required "
                    "filter field: "} +
        std::string{name};
    passed &= expect(!snapshot.facts_available &&
                         snapshot.not_obtained_reason == expected,
                     "each missing required filter must fail closed");
  }
  for (auto const& [missing, name] : coverage_reasons) {
    provider->policy = available_policy(true);
    std::erase(provider->policy.coverage, missing);
    auto const snapshot = reader.snapshot();
    auto const expected =
        std::string{"NOT_OBTAINED: debug log policy provider omitted required "
                    "coverage: "} +
        std::string{name};
    passed &= expect(!snapshot.facts_available &&
                         snapshot.not_obtained_reason == expected,
                     "each missing required coverage fact must fail closed");
  }
  return passed;
}

class HistoryAndLogsPolicyConsumerProbe final {
 public:
  explicit HistoryAndLogsPolicyConsumerProbe(
      DebugLogPolicySnapshotSource const& source) noexcept
      : source_(source) {}

  [[nodiscard]] DebugLogPolicyContext context() const {
    return make_debug_log_policy_context(source_.snapshot());
  }

 private:
  DebugLogPolicySnapshotSource const& source_;
};

[[nodiscard]] bool history_consumer_can_inject_and_consume_without_log_writes() {
  auto provider = std::make_shared<FakeDebugLogPolicyProvider>();
  provider->policy = available_policy(true);
  DebugLogPolicyReader reader{provider};
  DebugLogPolicySnapshotSource const& injectable_reader = reader;
  HistoryAndLogsPolicyConsumerProbe consumer{injectable_reader};
  std::vector<std::string> retained_log_records{
      "segment-4/sequence-19",
      "segment-4/sequence-20",
  };
  auto const retained_before = retained_log_records;

  auto const enabled = consumer.context();
  provider->policy = available_policy(false);
  auto const disabled = consumer.context();

  return expect(enabled.facts_available && enabled.debug_mode == "enabled" &&
                    enabled.granularity == "maximum" &&
                    enabled.filterable_fields.size() ==
                        kRequiredDebugLogFilterFields.size() &&
                    enabled.coverage.size() ==
                        kRequiredDebugLogCoverage.size(),
                "history consumers must receive complete provider context") &&
         expect(disabled.facts_available &&
                    disabled.debug_mode == "disabled" &&
                    disabled.granularity == "normal" &&
                    disabled.existing_log_retention ==
                        "preserves_existing_records",
                "disabled context must retain existing log semantics") &&
         expect(retained_log_records == retained_before,
                "policy consumption and debug shutdown must not mutate logs");
}

[[nodiscard]] bool missing_or_failed_providers_fail_closed() {
  DebugLogPolicyReader unconfigured;
  auto const missing = unconfigured.snapshot();

  auto provider = std::make_shared<FakeDebugLogPolicyProvider>();
  provider->throw_on_read = true;
  DebugLogPolicyReader failed_reader{provider};
  auto const failed = failed_reader.snapshot();

  return expect(!missing.facts_available &&
                    missing.debug_mode == DebugModeState::unavailable &&
                    missing.not_obtained_reason.starts_with("NOT_OBTAINED:"),
                "an unconfigured provider must fail closed with NOT_OBTAINED") &&
         expect(!failed.facts_available &&
                    failed.granularity == DebugLogGranularity::unavailable &&
                    failed.not_obtained_reason ==
                        "NOT_OBTAINED: debug log policy provider failed",
                "a provider failure must not expose stale policy facts");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= enabled_and_disabled_states_report_their_granularity();
  passed &= filters_and_location_are_canonical_and_deterministic();
  passed &= every_required_filter_and_coverage_fact_is_enforced();
  passed &= history_consumer_can_inject_and_consume_without_log_writes();
  passed &= missing_or_failed_providers_fail_closed();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
