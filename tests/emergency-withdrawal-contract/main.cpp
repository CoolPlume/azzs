#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/emergency_withdrawal_service.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/domain/emergency_withdrawal.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace withdrawal = azzs::domain::emergency_withdrawal;
using azzs::application::DeviceStateStore;
using azzs::application::CorrelationId;
using azzs::application::DiagnosticContext;
using azzs::application::DiagnosticExportReceipt;
using azzs::application::EmergencyWithdrawalCheckCode;
using azzs::application::EmergencyWithdrawalNoticeSource;
using azzs::application::EmergencyWithdrawalService;
using azzs::application::EmergencyWithdrawalServiceState;
using azzs::application::ExecutionEvent;
using azzs::application::ExecutionLog;
using azzs::application::ExecutionLogClearReceipt;
using azzs::application::ExecutionLogReceipt;
using azzs::application::OperationAuthorizationCode;
using azzs::application::OperationAuthorizationResult;
using azzs::application::NoticeFetchResult;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "emergency withdrawal contract failed: " << message << '\n';
  }
  return condition;
}

class SequenceNoticeSource final : public EmergencyWithdrawalNoticeSource {
 public:
  [[nodiscard]] NoticeFetchResult fetch() override {
    if (next_error.has_value()) {
      auto error = std::move(next_error);
      next_error.reset();
      return {.error = std::move(*error)};
    }
    if (next_document.empty()) {
      return {.error = "no notice configured"};
    }
    return {.succeeded = true, .document = next_document};
  }

  std::string next_document;
  std::optional<std::string> next_error;
};

class BlockingNoticeSource final : public EmergencyWithdrawalNoticeSource {
 public:
  BlockingNoticeSource(std::shared_future<void> release,
                       std::promise<void>& entered)
      : release_(std::move(release)), entered_(entered) {}

  [[nodiscard]] NoticeFetchResult fetch() override {
    entered_.set_value();
    release_.wait();
    return {.error = "offline"};
  }

 private:
  std::shared_future<void> release_;
  std::promise<void>& entered_;
};

class RecordingLog final : public ExecutionLog {
 public:
  [[nodiscard]] CorrelationId begin_correlation() override {
    return CorrelationId{"emergency-withdrawal-correlation"};
  }

  [[nodiscard]] ExecutionLogReceipt append(
      CorrelationId const&, ExecutionEvent const& event) override {
    events.push_back(event);
    return {.persisted = true,
            .segment = 1,
            .sequence = static_cast<std::uint64_t>(events.size())};
  }

  [[nodiscard]] ExecutionLogClearReceipt clear() override {
    events.clear();
    return {.cleared = true};
  }

  [[nodiscard]] DiagnosticExportReceipt export_diagnostic(
      DiagnosticContext const&) override {
    return {.produced = true, .file_count = 1};
  }

  std::vector<ExecutionEvent> events;
};

[[nodiscard]] std::string notice(std::uint64_t revision,
                                 std::string_view action = "withdraw") {
  return "source=project-security\nrevision=" + std::to_string(revision) +
         "\npublished_at_ms=1786422400000\nentry=sogou-input|software_optimization|14.0|15.0|critical installer risk|" +
         std::string{action} + "\n";
}

struct Fixture final {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'500'000}}};
  DeviceStateStore states{files, clock};
  SequenceNoticeSource source;
  EmergencyWithdrawalService service{states, clock, source};
};

[[nodiscard]] bool parse_contract() {
  auto parsed = withdrawal::parse_notice(notice(7));
  bool passed = expect(parsed.succeeded(), "a complete notice must parse");
  if (parsed.notice.has_value()) {
    passed &= expect(parsed.notice->revision == 7 &&
                         parsed.notice->entries.size() == 1 &&
                         parsed.notice->entries.front().category ==
                             withdrawal::OperationCategory::software_optimization,
                     "parsed notice must preserve revision and category");
  }
  auto unknown = withdrawal::parse_notice(
      notice(8) + "future_semantics=must-reject\n");
  passed &= expect(unknown.error == withdrawal::NoticeParseError::unknown_field,
                   "unknown semantic fields must fail closed");
  auto duplicate = withdrawal::parse_notice(notice(8) +
                                            "entry=sogou-input|software_optimization|14.0|15.0|other|withdraw\n");
  passed &= expect(duplicate.error == withdrawal::NoticeParseError::duplicate_entry,
                   "duplicate operation ranges must be rejected");
  auto contradictory_ranges = withdrawal::parse_notice(
      "source=project-security\nrevision=8\npublished_at_ms=1786422400000\n"
      "entry=sogou-input|software_optimization|14.0|15.0|withdrawn|withdraw\n"
      "entry=sogou-input|software_optimization|14.5|16.0|released|release\n");
  passed &= expect(
      contradictory_ranges.error == withdrawal::NoticeParseError::duplicate_entry,
      "overlapping withdraw and release ranges must be rejected");
  auto disjoint_ranges = withdrawal::parse_notice(
      "source=project-security\nrevision=8\npublished_at_ms=1786422400000\n"
      "entry=sogou-input|software_optimization|14.0|14.4|withdrawn|withdraw\n"
      "entry=sogou-input|software_optimization|14.5|16.0|released|release\n");
  passed &= expect(disjoint_ranges.succeeded(),
                   "disjoint withdraw and release ranges must remain expressible");
  auto unknown_category = withdrawal::parse_notice(
      "source=project-security\nrevision=8\npublished_at_ms=1786422400000\n"
      "entry=sogou-input|future_category|||risk|withdraw\n");
  passed &= expect(unknown_category.error ==
                       withdrawal::NoticeParseError::unknown_field,
                   "unknown operation categories must fail closed");
  auto unknown_action = withdrawal::parse_notice(
      "source=project-security\nrevision=8\npublished_at_ms=1786422400000\n"
      "entry=sogou-input|software_optimization|||risk|future_action\n");
  passed &= expect(unknown_action.error ==
                       withdrawal::NoticeParseError::unknown_field,
                   "unknown withdrawal actions must fail closed");
  auto oversized = withdrawal::parse_notice(std::string(4U * 1024U * 1024U + 1U,
                                                         'x'));
  passed &= expect(oversized.error == withdrawal::NoticeParseError::input_too_large,
                   "oversized notification input must fail closed");
  return passed;
}

[[nodiscard]] bool revision_and_blocking_contract() {
  Fixture fixture;
  fixture.source.next_document = notice(7);
  auto checked = fixture.service.preflight_check();
  bool passed = expect(checked.code == EmergencyWithdrawalCheckCode::updated,
                       "a valid online notice must be accepted");
  auto blocked = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  passed &= expect(blocked.code == OperationAuthorizationCode::blocked,
                   "a matching withdrawal must block the operation");
  auto unknown_version = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
  });
  passed &= expect(
      unknown_version.code == OperationAuthorizationCode::blocked &&
          unknown_version.matching_notice.has_value() &&
          unknown_version.notice_source == "project-security" &&
          unknown_version.notice_revision == 7 &&
          unknown_version.matching_notice->affected_versions.minimum == "14.0" &&
          unknown_version.matching_notice->affected_versions.maximum == "15.0",
      "a version-scoped withdrawal must block an operation whose version is unknown");

  fixture.source.next_document = notice(6, "release");
  auto stale = fixture.service.check();
  passed &= expect(stale.code == EmergencyWithdrawalCheckCode::stale_ignored,
                   "an older revision must not roll back accepted safety state");
  auto still_blocked = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  passed &= expect(still_blocked.code == OperationAuthorizationCode::blocked,
                   "an older release must not clear a newer withdrawal");

  fixture.source.next_document = notice(8, "release");
  auto released = fixture.service.check();
  passed &= expect(released.code == EmergencyWithdrawalCheckCode::updated,
                   "a newer revision may release the operation");
  auto allowed = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  passed &= expect(allowed.code == OperationAuthorizationCode::allowed,
                   "a newer release must allow new work");

  fixture.source.next_document =
      "source=project-security\nrevision=9\npublished_at_ms=1786422400002\n"
      "entry=sogou-input|software_optimization|14.0|14.4|narrower risk|withdraw\n";
  auto narrowed = fixture.service.check();
  auto outside_narrowed_range = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  passed &= expect(
      narrowed.code == EmergencyWithdrawalCheckCode::updated &&
          outside_narrowed_range.code == OperationAuthorizationCode::allowed,
      "a newer source snapshot may narrow an earlier withdrawal range");

  auto repeated_narrowed = fixture.service.check();
  passed &= expect(
      repeated_narrowed.code == EmergencyWithdrawalCheckCode::stale_ignored,
      "an identical source snapshot at the accepted revision must not conflict");

  fixture.source.next_document =
      "source=project-security\nrevision=10\npublished_at_ms=1786422400003\n"
      "entry=second-risk|software_optimization|||second risk|withdraw\n"
      "entry=sogou-input|software_optimization|14.0|14.4|narrower risk|withdraw\n";
  auto reordered = fixture.service.check();
  fixture.source.next_document =
      "source=project-security\nrevision=10\npublished_at_ms=1786422400003\n"
      "entry=sogou-input|software_optimization|14.0|14.4|narrower risk|withdraw\n"
      "entry=second-risk|software_optimization|||second risk|withdraw\n";
  auto same_semantics = fixture.service.check();
  passed &= expect(
      reordered.code == EmergencyWithdrawalCheckCode::updated &&
          same_semantics.code == EmergencyWithdrawalCheckCode::stale_ignored,
      "entry ordering must not turn the same revision snapshot into a conflict");

  fixture.source.next_document =
      "source=project-security\nrevision=10\npublished_at_ms=1786422400001\n"
      "entry=another-risk|software_optimization|||conflicting same revision|withdraw\n";
  auto conflicting = fixture.service.check();
  passed &= expect(conflicting.code == EmergencyWithdrawalCheckCode::rejected,
                   "the same revision with different content must be rejected");
  auto still_allowed = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  passed &= expect(still_allowed.code == OperationAuthorizationCode::allowed,
                   "a rejected same-revision conflict must retain the accepted state");
  return passed;
}

[[nodiscard]] bool independent_sources_and_observation_contract() {
  Fixture fixture;
  fixture.source.next_document = notice(7);
  auto first = fixture.service.check();
  bool passed = expect(first.code == EmergencyWithdrawalCheckCode::updated,
                       "the first source must initialize the safety state");
  fixture.clock.advance(std::chrono::minutes{1});
  fixture.source.next_document =
      "source=secondary-security\nrevision=1\npublished_at_ms=1786422400001\n"
      "entry=windows-risk|system_optimization|||windows risk|withdraw\n";
  auto second = fixture.service.check();
  passed &= expect(second.code == EmergencyWithdrawalCheckCode::updated,
                   "a second source must retain the first source's facts");
  auto first_source_block = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  auto second_source_block = fixture.service.authorize({
      .stable_id = "windows-risk",
      .category = withdrawal::OperationCategory::system_optimization,
  });
  passed &= expect(first_source_block.code == OperationAuthorizationCode::blocked &&
                       second_source_block.code == OperationAuthorizationCode::blocked,
                   "different sources must not overwrite each other's withdrawals");

  fixture.clock.advance(std::chrono::minutes{1});
  fixture.source.next_document = notice(7);
  auto repeated = fixture.service.check();
  auto snapshot = fixture.service.snapshot();
  passed &= expect(repeated.code == EmergencyWithdrawalCheckCode::stale_ignored &&
                       snapshot.last_observed_at == fixture.clock.now(),
                   "a same revision check must refresh only observed_at");
  return passed;
}

[[nodiscard]] bool offline_and_first_failure_contract() {
  Fixture fixture;
  fixture.source.next_error = "offline";
  auto first = fixture.service.check();
  bool passed = expect(
      first.code == EmergencyWithdrawalCheckCode::unknown &&
          first.snapshot.state == EmergencyWithdrawalServiceState::unknown &&
          !first.snapshot.persistence_pending,
      "first fetch failure without cache must remain unknown rather than successful");
  auto allowed = fixture.service.authorize({
      .stable_id = "new-install",
      .category = withdrawal::OperationCategory::software_installation,
  });
  passed &= expect(allowed.code == OperationAuthorizationCode::allowed_unknown,
                   "unknown first state must allow new operations with warning");

  fixture.source.next_document = "source=project-security\nrevision=1\n";
  auto malformed_first = fixture.service.check();
  auto malformed_authorization = fixture.service.authorize({
      .stable_id = "new-install",
      .category = withdrawal::OperationCategory::software_installation,
  });
  passed &= expect(
      malformed_first.code == EmergencyWithdrawalCheckCode::rejected &&
          malformed_first.snapshot.state == EmergencyWithdrawalServiceState::unknown &&
          malformed_first.snapshot.notices.empty() &&
          !malformed_first.snapshot.persistence_pending &&
          malformed_authorization.code == OperationAuthorizationCode::allowed_unknown,
      "first parse failure must remain unknown and must not enter a safety cache");

  fixture.source.next_document = notice(1);
  [[maybe_unused]] auto const cached_seed = fixture.service.check();
  fixture.source.next_error = "offline";
  auto cached = fixture.service.check();
  passed &= expect(cached.code == EmergencyWithdrawalCheckCode::cached,
                   "failed refresh must retain the cached notice");
  auto blocked = fixture.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  passed &= expect(blocked.code == OperationAuthorizationCode::blocked,
                   "cached withdrawals remain effective offline");
  auto snapshot = fixture.service.snapshot();
  passed &= expect(snapshot.state == EmergencyWithdrawalServiceState::cached &&
                       snapshot.possibly_stale && snapshot.last_observed_at.has_value(),
                   "cached state must expose staleness and observation time");

  fixture.source.next_document = "source=project-security\nrevision=2\n";
  auto malformed_cached = fixture.service.check();
  passed &= expect(
      malformed_cached.code == EmergencyWithdrawalCheckCode::rejected &&
          fixture.service.authorize({
              .stable_id = "sogou-input",
              .category = withdrawal::OperationCategory::software_optimization,
              .version = "14.5",
          }).code == OperationAuthorizationCode::blocked,
      "a malformed refresh must not displace an effective cached withdrawal");
  return passed;
}

[[nodiscard]] bool cross_category_contract() {
  Fixture fixture;
  fixture.source.next_document =
      "source=project-security\nrevision=4\npublished_at_ms=1786422400000\n"
      "entry=shared-risk|software_installation|||installer risk|withdraw\n"
      "entry=windows-risk|system_optimization|||windows risk|withdraw\n";
  [[maybe_unused]] auto const checked = fixture.service.check();
  bool passed = true;
  for (auto category : {withdrawal::OperationCategory::software_installation,
                        withdrawal::OperationCategory::system_optimization}) {
    auto result = fixture.service.authorize({
        .stable_id = category == withdrawal::OperationCategory::software_installation
                         ? "shared-risk"
                         : "windows-risk",
        .category = category,
    });
    passed &= expect(result.code == OperationAuthorizationCode::blocked,
                     "category is part of the stable operation identity");
  }
  auto mismatch = fixture.service.authorize({
      .stable_id = "windows-risk",
      .category = withdrawal::OperationCategory::software_optimization,
  });
  passed &= expect(mismatch.code == OperationAuthorizationCode::allowed,
                   "a page/category mismatch must not over-block another category");
  auto invalid = fixture.service.authorize({
      .stable_id = "windows-risk",
      .category = static_cast<withdrawal::OperationCategory>(99),
  });
  passed &= expect(invalid.code == OperationAuthorizationCode::invalid_request,
                   "an unknown operation category must fail closed");
  auto malformed_version = fixture.service.authorize({
      .stable_id = "windows-risk",
      .category = withdrawal::OperationCategory::system_optimization,
      .version = "14.invalid",
  });
  passed &= expect(
      malformed_version.code == OperationAuthorizationCode::invalid_request,
      "an unparseable operation version must fail closed");
  return passed;
}

[[nodiscard]] bool structured_logging_contract() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'500'000}}};
  DeviceStateStore states{files, clock};
  SequenceNoticeSource source;
  RecordingLog log;
  EmergencyWithdrawalService service{
      states, clock, source,
      {.log = &log, .correlation = log.begin_correlation()}};
  source.next_document = notice(12);
  auto checked = service.preflight_check();
  bool passed = expect(checked.code == EmergencyWithdrawalCheckCode::updated,
                       "logged notice fixture must be accepted");
  auto const expected_time =
      std::to_string(clock.now().time_since_epoch().count());
  auto has_field = [](ExecutionEvent const& event, std::string_view key,
                      std::string_view value) {
    return std::ranges::any_of(event.fields, [&](auto const& field) {
      return field.key == key && field.value == value;
    });
  };
  auto const logged = std::ranges::find_if(
      log.events, [](ExecutionEvent const& event) {
        return event.component == "emergency-withdrawal" &&
               event.stage == "apply-notice";
      });
  passed &= expect(logged != log.events.end() &&
                       has_field(*logged, "notice_source", "project-security") &&
                       has_field(*logged, "notice_revision", "12") &&
                       has_field(*logged, "notice_observed_at_ms", expected_time) &&
                       has_field(*logged, "affected_stable_id", "sogou-input") &&
                       has_field(*logged, "affected_category",
                                 "software_optimization") &&
                       has_field(*logged, "affected_version_min", "14.0") &&
                       has_field(*logged, "affected_version_max", "15.0") &&
                       has_field(*logged, "withdrawal_action", "withdraw") &&
                       has_field(*logged, "withdrawal_reason",
                                 "critical installer risk"),
                   "accepted notices must log source, revision, observation and affected identity");
  auto authorization = service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  auto const authorized = std::ranges::find_if(
      log.events, [](ExecutionEvent const& event) {
        return event.component == "emergency-withdrawal" &&
               event.stage == "authorize";
      });
  passed &= expect(
      authorization.code == OperationAuthorizationCode::blocked &&
          authorized != log.events.end() &&
          has_field(*authorized, "authorization", "blocked") &&
          has_field(*authorized, "operation_stable_id", "sogou-input") &&
          has_field(*authorized, "operation_category", "software_optimization") &&
          has_field(*authorized, "operation_version", "14.5") &&
          has_field(*authorized, "notice_source", "project-security") &&
          has_field(*authorized, "notice_revision", "12") &&
          has_field(*authorized, "notice_observed_at_ms", expected_time) &&
          has_field(*authorized, "affected_stable_id", "sogou-input") &&
          has_field(*authorized, "affected_category", "software_optimization") &&
          has_field(*authorized, "affected_version_min", "14.0") &&
          has_field(*authorized, "affected_version_max", "15.0") &&
          has_field(*authorized, "withdrawal_action", "withdraw") &&
          has_field(*authorized, "withdrawal_reason", "critical installer risk"),
      "authorization decisions must log the matching withdrawal evidence");
  return passed;
}

[[nodiscard]] bool persistence_and_corruption_contract() {
  Fixture fixture;
  fixture.source.next_document = notice(9);
  [[maybe_unused]] auto const checked = fixture.service.check();
  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"emergency-withdrawals"});
  auto persisted = fixture.files.raw_file(key, azzs::application::StateFileSlot::current);
  bool passed = expect(persisted.has_value(),
                       "accepted notices must persist in a dedicated device aggregate");
  DeviceStateStore restarted_states{fixture.files, fixture.clock};
  SequenceNoticeSource offline_source;
  offline_source.next_error = "offline";
  EmergencyWithdrawalService restarted{restarted_states, fixture.clock,
                                       offline_source};
  auto cached = restarted.check();
  passed &= expect(cached.code == EmergencyWithdrawalCheckCode::cached &&
                       restarted.authorize({"sogou-input",
                                            withdrawal::OperationCategory::software_optimization,
                                            "14.5"})
                           .code == OperationAuthorizationCode::blocked,
                   "the device aggregate must survive service recreation");
  fixture.files.corrupt(key, azzs::application::StateFileSlot::current);
  fixture.files.corrupt(key, azzs::application::StateFileSlot::previous);
  auto corrupt = restarted.snapshot();
  passed &= expect(corrupt.state == EmergencyWithdrawalServiceState::read_only,
                   "dual corruption must fail closed and reject writes");
  return passed;
}

[[nodiscard]] bool persistence_limits_contract() {
  Fixture fixture;
  for (std::size_t index = 0; index < 65; ++index) {
    fixture.source.next_document =
        "source=source-" + std::to_string(index) +
        "\nrevision=1\npublished_at_ms=1786422400000\nentry=risk-" +
        std::to_string(index) +
        "|software_optimization|||risk|withdraw\n";
    auto checked = fixture.service.check();
    if (index < 64 && checked.code != EmergencyWithdrawalCheckCode::updated) {
      return expect(false, "each source within the persistence limit must be accepted");
    }
    if (index == 64) {
      return expect(
          checked.code == EmergencyWithdrawalCheckCode::failed &&
              fixture.service.snapshot().notices.size() == 64,
          "a notice beyond the persistent source limit must leave the prior authority intact");
    }
  }
  return false;
}

[[nodiscard]] bool release_only_and_commit_failure_contract() {
  Fixture fixture;
  fixture.source.next_document = notice(1, "release");
  auto release_only = fixture.service.check();
  bool passed = expect(release_only.code == EmergencyWithdrawalCheckCode::updated,
                       "a release-only notice must establish a checked safe state");
  passed &= expect(fixture.service.authorize({
                        .stable_id = "new-install",
                        .category = withdrawal::OperationCategory::software_installation,
                    })
                       .code == OperationAuthorizationCode::allowed,
                   "a persisted release-only notice must not be reported as unknown");

  Fixture failure;
  failure.files.fail_next(azzs::testing::StateFileOperation::write,
                          azzs::application::StateFileSlot::candidate);
  failure.source.next_document = notice(1);
  auto failed = failure.service.check();
  auto retained = failure.service.authorize({
      .stable_id = "sogou-input",
      .category = withdrawal::OperationCategory::software_optimization,
      .version = "14.5",
  });
  auto retained_snapshot = failure.service.snapshot();
  passed &= expect(
      failed.code == EmergencyWithdrawalCheckCode::failed &&
          failed.snapshot.notices.empty() &&
          retained_snapshot.state == EmergencyWithdrawalServiceState::unpersisted &&
          retained_snapshot.persistence_pending &&
          retained_snapshot.possibly_stale &&
          retained.code == OperationAuthorizationCode::blocked,
      "a parsed withdrawal with a failed commit must remain blocked in this process");

  failure.source.next_document =
      "source=project-security\nrevision=2\n"
      "entry=invalid-risk|future_category|||must not retain|withdraw\n";
  auto malformed = failure.service.check();
  auto invalid_target = failure.service.authorize({
      .stable_id = "invalid-risk",
      .category = withdrawal::OperationCategory::software_optimization,
  });
  passed &= expect(
      malformed.code == EmergencyWithdrawalCheckCode::rejected &&
          invalid_target.code == OperationAuthorizationCode::allowed_unknown &&
          failure.service.authorize({
              .stable_id = "sogou-input",
              .category = withdrawal::OperationCategory::software_optimization,
              .version = "14.5",
          }).code == OperationAuthorizationCode::blocked,
      "invalid input must not become an in-memory withdrawal while retained rules stay blocking");

  DeviceStateStore restarted_states{failure.files, failure.clock};
  SequenceNoticeSource restarted_source;
  restarted_source.next_error = "offline";
  EmergencyWithdrawalService restarted{restarted_states, failure.clock,
                                       restarted_source};
  auto restarted_check = restarted.preflight_check();
  passed &= expect(failure.service.authorize({
                        .stable_id = "sogou-input",
                        .category = withdrawal::OperationCategory::software_optimization,
                        .version = "14.5",
                    })
                       .code == OperationAuthorizationCode::blocked &&
                       restarted_check.code == EmergencyWithdrawalCheckCode::unknown &&
                       restarted.authorize({
                           .stable_id = "sogou-input",
                           .category = withdrawal::OperationCategory::software_optimization,
                           .version = "14.5",
                       }).code == OperationAuthorizationCode::allowed_unknown,
                   "unpersisted protection must end on restart, which returns to the first-failure unknown rule");
  return passed;
}

[[nodiscard]] bool serialized_public_entry_contract() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{
      std::chrono::milliseconds{1'786'422'500'000}}};
  DeviceStateStore states{files, clock};
  std::promise<void> release;
  auto release_signal = release.get_future().share();
  std::promise<void> entered;
  auto entered_signal = entered.get_future();
  BlockingNoticeSource source{release_signal, entered};
  EmergencyWithdrawalService service{states, clock, source};
  auto check = std::async(std::launch::async, [&] {
    return service.check();
  });
  entered_signal.wait();
  std::promise<void> authorization_invoking;
  auto authorization_invoking_signal = authorization_invoking.get_future();
  auto authorization = std::async(std::launch::async, [&] {
    authorization_invoking.set_value();
    return service.authorize({
        .stable_id = "new-install",
        .category = withdrawal::OperationCategory::software_installation,
    });
  });
  authorization_invoking_signal.wait();
  auto const serialized = authorization.wait_for(std::chrono::milliseconds{100}) ==
                          std::future_status::timeout;
  release.set_value();
  auto const checked = check.get();
  auto const authorized = authorization.get();
  return expect(serialized && checked.code == EmergencyWithdrawalCheckCode::unknown &&
                    authorized.code == OperationAuthorizationCode::allowed_unknown,
                "concurrent check and authorization must serialize on one safety state");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= parse_contract();
  passed &= revision_and_blocking_contract();
  passed &= independent_sources_and_observation_contract();
  passed &= offline_and_first_failure_contract();
  passed &= cross_category_contract();
  passed &= structured_logging_contract();
  passed &= persistence_and_corruption_contract();
  passed &= persistence_limits_contract();
  passed &= release_only_and_commit_failure_contract();
  passed &= serialized_public_entry_contract();
  if (!passed) return EXIT_FAILURE;
  std::cout << "emergency withdrawal contract passed\n";
  return EXIT_SUCCESS;
}
