#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "azzs/application/debug_log_policy/debug_log_policy.hpp"

namespace {

using azzs::application::DebugLogCoverage;
using azzs::application::DebugLogFilterField;
using azzs::application::DebugLogGranularity;
using azzs::application::DebugLogLocationSemantics;
using azzs::application::DebugLogPolicyProvider;
using azzs::application::DebugLogPolicyRead;
using azzs::application::DebugLogPolicyReader;
using azzs::application::DebugLogRetention;
using azzs::application::DebugModeState;

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
      .filterable_fields = {
          DebugLogFilterField::retry,
          DebugLogFilterField::command,
          DebugLogFilterField::correlation_id,
      },
      .locating_semantics =
          DebugLogLocationSemantics::event_sequence_and_correlation_id,
      .coverage = {
          DebugLogCoverage::recovery,
          DebugLogCoverage::installation,
          DebugLogCoverage::catalog_validation,
      },
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
  provider->policy.filterable_fields = {
      DebugLogFilterField::retry,
      DebugLogFilterField::command,
      DebugLogFilterField::retry,
      DebugLogFilterField::correlation_id,
      DebugLogFilterField::command,
  };
  provider->policy.coverage = {
      DebugLogCoverage::recovery,
      DebugLogCoverage::catalog_validation,
      DebugLogCoverage::recovery,
  };
  DebugLogPolicyReader reader{provider};

  auto const snapshot = reader.snapshot();
  return expect(snapshot.filterable_fields ==
                    std::vector<DebugLogFilterField>{
                        DebugLogFilterField::command,
                        DebugLogFilterField::retry,
                        DebugLogFilterField::correlation_id,
                    },
                "filterable fields must be ordered and deduplicated") &&
         expect(snapshot.coverage == std::vector<DebugLogCoverage>{
                    DebugLogCoverage::catalog_validation,
                    DebugLogCoverage::recovery,
                },
                "coverage must be ordered and deduplicated") &&
         expect(snapshot.locating_semantics ==
                    DebugLogLocationSemantics::event_sequence_and_correlation_id,
                "location must use stable event sequence and correlation id");
}

[[nodiscard]] bool disabling_debug_preserves_existing_log_records() {
  auto provider = std::make_shared<FakeDebugLogPolicyProvider>();
  provider->policy = available_policy(true);
  DebugLogPolicyReader reader{provider};

  auto const before = reader.snapshot();
  provider->policy = available_policy(false);
  auto const after = reader.snapshot();

  return expect(before.existing_log_retention ==
                    DebugLogRetention::preserves_existing_records &&
                    after.existing_log_retention ==
                        DebugLogRetention::preserves_existing_records,
                "changing debug mode must not delete or rewrite existing logs") &&
         expect(before.coverage == after.coverage &&
                    before.filterable_fields == after.filterable_fields,
                "disabling debug must preserve the existing log-view contract");
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
  passed &= disabling_debug_preserves_existing_log_records();
  passed &= missing_or_failed_providers_fail_closed();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
