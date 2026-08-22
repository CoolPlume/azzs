#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "settings_navigation_recovery.hpp"

namespace {

using azzs::ui::presentation::SettingsNavigationCallbacks;
using azzs::ui::presentation::SettingsNavigationFailure;
using azzs::ui::presentation::SettingsNavigationFailureStage;
using azzs::ui::presentation::SettingsNavigationPreparation;
using azzs::ui::presentation::SettingsNavigationBridge;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "settings navigation contract failed: " << message << '\n';
  }
  return condition;
}

[[noreturn]] void throw_commit_failure() {
  throw std::runtime_error{"injected commit failure"};
}

[[nodiscard]] bool verify_normal_navigation_and_duplicate_short_circuit() {
  bool passed = true;
  SettingsNavigationBridge bridge;
  int prepare_count = 0;
  int commit_count = 0;
  int clear_count = 0;
  int failure_count = 0;
  int core_page = 1;
  bool already_visible = false;

  auto callbacks = SettingsNavigationCallbacks{
      .already_visible = [&] { return already_visible; },
      .prepare = [&] {
        ++prepare_count;
        return SettingsNavigationPreparation{
            .commit = [&] {
              ++commit_count;
              core_page = 2;
            }};
      },
      .present_failure = [&](SettingsNavigationFailure const&) {
        ++failure_count;
      },
      .clear_failure = [&] { ++clear_count; },
  };

  passed &= expect(bridge.navigate(callbacks),
                   "a prepared settings page must commit successfully");
  passed &= expect(prepare_count == 1 && commit_count == 1 && core_page == 2,
                   "normal navigation must prepare and commit exactly once");
  passed &= expect(failure_count == 0 && clear_count == 1,
                   "normal navigation must clear stale failure state");

  already_visible = true;
  passed &= expect(bridge.navigate(callbacks),
                   "repeated navigation to the visible settings page must succeed");
  passed &= expect(prepare_count == 1 && commit_count == 1,
                   "duplicate navigation must not rebuild or rebind the page");
  return passed;
}

[[nodiscard]] bool verify_preparation_failures_do_not_commit() {
  bool passed = true;
  for (auto const stage : {SettingsNavigationFailureStage::optional_value_missing,
                           SettingsNavigationFailureStage::snapshot_read,
                           SettingsNavigationFailureStage::page_binding,
                           SettingsNavigationFailureStage::resource_projection}) {
    SettingsNavigationBridge bridge;
    int commit_count = 0;
    int failure_count = 0;
    int core_page = 1;
    int recovery_count = 0;
    auto const result = bridge.navigate(SettingsNavigationCallbacks{
        .prepare = [&, stage] {
          return SettingsNavigationPreparation{
              .failure = SettingsNavigationFailure{
                  .stage = stage, .detail = "injected preparation failure"},
              .recover = [&] { ++recovery_count; }};
        },
        .present_failure = [&](SettingsNavigationFailure const& failure) {
          ++failure_count;
          passed &= expect(failure.stage == stage,
                           "the injected preparation stage must be retained");
        },
        .clear_failure = [] {},
    });
    static_cast<void>(result);
    passed &= expect(!result && commit_count == 0 && core_page == 1,
                     "preparation failure must leave core page uncommitted");
    passed &= expect(failure_count == 1 && recovery_count == 1 && bridge.failed(),
                     "preparation failure must recover once and project one stable failure state");
  }
  return passed;
}

[[nodiscard]] bool verify_retry_and_return_paths() {
  bool passed = true;
  SettingsNavigationBridge bridge;
  bool fail_first = true;
  int prepare_count = 0;
  int commit_count = 0;
  int failure_count = 0;
  int clear_count = 0;
  int next_recovery_token = 0;
  std::vector<int> recovered_tokens;

  auto callbacks = SettingsNavigationCallbacks{
      .prepare = [&] {
        ++prepare_count;
        auto const recovery_token = ++next_recovery_token;
        if (fail_first) {
          return SettingsNavigationPreparation{
              .failure = SettingsNavigationFailure{
                  .stage = SettingsNavigationFailureStage::snapshot_read,
                  .detail = "retry fixture"},
              .recover = [&, recovery_token] {
                recovered_tokens.push_back(recovery_token);
              }};
        }
        return SettingsNavigationPreparation{
            .commit = [&] { ++commit_count; },
            .recover = [&, recovery_token] {
              recovered_tokens.push_back(recovery_token);
            }};
      },
      .present_failure = [&](SettingsNavigationFailure const&) {
        ++failure_count;
      },
      .clear_failure = [&] { ++clear_count; },
  };

  passed &= expect(!bridge.navigate(callbacks),
                   "the first injected read failure must remain recoverable");
  fail_first = false;
  passed &= expect(bridge.retry(), "retry must reuse the same preparation boundary");
  passed &= expect(prepare_count == 2 && commit_count == 1 &&
                       bridge.attempts() == 2,
                   "retry must prepare again once and commit once");
  passed &= expect(failure_count == 1 && recovered_tokens.size() == 1 &&
                       recovered_tokens.front() == 1 && clear_count == 1 &&
                       !bridge.failed(),
                   "successful retry must clear the prior failure");

  fail_first = true;
  passed &= expect(!bridge.navigate(callbacks),
                   "a later failure must be projectable again");
  bridge.return_to_current();
  passed &= expect(!bridge.failed() && !bridge.last_failure().has_value() &&
                       failure_count == 2 && recovered_tokens.size() == 2 &&
                       recovered_tokens.back() == 3 && clear_count == 2,
                   "return-to-current must only clear recovery state");
  passed &= expect(!bridge.retry() && prepare_count == 3 &&
                       next_recovery_token == 3 && commit_count == 1 &&
                       recovered_tokens.size() == 2,
                   "return-to-current must invalidate the stale retry transaction");
  return passed;
}

[[nodiscard]] bool verify_missing_commit_recovers_once() {
  SettingsNavigationBridge bridge;
  int recovery_count = 0;
  int failure_count = 0;
  bool stage_is_resource_projection = false;

  auto const result = bridge.navigate(SettingsNavigationCallbacks{
      .prepare = [&] {
        return SettingsNavigationPreparation{
            .recover = [&] { ++recovery_count; }};
      },
      .present_failure = [&](SettingsNavigationFailure const& failure) {
        ++failure_count;
        stage_is_resource_projection =
            failure.stage == SettingsNavigationFailureStage::resource_projection;
      },
      .clear_failure = [] {},
  });
  bridge.return_to_current();
  return expect(!result && failure_count == 1 && recovery_count == 1 &&
                    stage_is_resource_projection && !bridge.failed(),
                "a missing commit must recover once and remain returnable");
}

[[nodiscard]] bool verify_empty_callbacks_are_safe() {
  SettingsNavigationBridge bridge;
  auto const first = bridge.navigate(SettingsNavigationCallbacks{});
  auto const failed_after_first = bridge.failed();
  auto const failure_stage = bridge.last_failure();
  auto const retry = bridge.retry();
  const auto attempts = bridge.attempts();
  bridge.return_to_current();
  return expect(!first && failed_after_first && failure_stage.has_value() &&
                    failure_stage->stage == SettingsNavigationFailureStage::unknown &&
                    !retry && attempts == 2 && !bridge.failed() &&
                    !bridge.last_failure().has_value(),
                "empty navigation callbacks must not crash or retain a stale transaction");
}

[[nodiscard]] bool verify_success_invalidates_callbacks() {
  SettingsNavigationBridge bridge;
  int prepare_count = 0;
  int commit_count = 0;
  auto callbacks = SettingsNavigationCallbacks{
      .prepare = [&] {
        ++prepare_count;
        return SettingsNavigationPreparation{
            .commit = [&] { ++commit_count; }};
      },
      .present_failure = [](SettingsNavigationFailure const&) {},
      .clear_failure = [] {},
  };

  auto const first = bridge.navigate(callbacks);
  auto const stale_retry = bridge.retry();
  return expect(first && !stale_retry && prepare_count == 1 && commit_count == 1,
                "successful navigation must release its callbacks");
}

[[nodiscard]] bool verify_commit_failure_is_not_core_success() {
  SettingsNavigationBridge bridge;
  int commit_invocations = 0;
  int failure_count = 0;
  int recovery_count = 0;
  auto const result = bridge.navigate(SettingsNavigationCallbacks{
      .prepare = [&] {
        return SettingsNavigationPreparation{
            .commit = [&] {
              ++commit_invocations;
              throw_commit_failure();
            },
            .recover = [&] { ++recovery_count; }};
      },
      .present_failure = [&](SettingsNavigationFailure const& failure) {
        ++failure_count;
        return expect(failure.stage == SettingsNavigationFailureStage::commit,
                      "commit exceptions must become commit failures");
      },
      .clear_failure = [] {},
  });
  return expect(!result && commit_invocations == 1 && failure_count == 1 &&
                    recovery_count == 1,
                 "a failed commit must not be reported as a core page commit");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_normal_navigation_and_duplicate_short_circuit();
  passed &= verify_preparation_failures_do_not_commit();
  passed &= verify_retry_and_return_paths();
  passed &= verify_missing_commit_recovers_once();
  passed &= verify_empty_callbacks_are_safe();
  passed &= verify_success_invalidates_callbacks();
  passed &= verify_commit_failure_is_not_core_success();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "settings navigation contract passed\n";
  return EXIT_SUCCESS;
}
