#pragma once

#include <optional>
#include <string>
#include <vector>

#include "azzs/application/clock.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/domain/emergency_withdrawal.hpp"

namespace azzs::application {

struct NoticeFetchResult final {
  bool succeeded{false};
  std::string document;
  std::string error;
};

class EmergencyWithdrawalNoticeSource {
 public:
  virtual ~EmergencyWithdrawalNoticeSource() = default;
  [[nodiscard]] virtual NoticeFetchResult fetch() = 0;
};

struct EmergencyWithdrawalLogContext final {
  ExecutionLog* log{nullptr};
  CorrelationId correlation;
};

enum class EmergencyWithdrawalServiceState {
  unknown,
  fresh,
  cached,
  read_only,
  failed,
};

struct EmergencyWithdrawalSnapshot final {
  EmergencyWithdrawalServiceState state{
      EmergencyWithdrawalServiceState::unknown};
  std::vector<domain::emergency_withdrawal::EmergencyWithdrawalNotice> notices;
  std::optional<WallClockTime> last_observed_at;
  bool possibly_stale{false};
  std::string error;
};

enum class EmergencyWithdrawalCheckCode {
  updated,
  stale_ignored,
  cached,
  unknown,
  rejected,
  read_only,
  outcome_unknown,
  failed,
};

struct EmergencyWithdrawalCheckResult final {
  EmergencyWithdrawalCheckCode code{EmergencyWithdrawalCheckCode::failed};
  EmergencyWithdrawalSnapshot snapshot;
  std::string error;
};

enum class OperationAuthorizationCode {
  allowed,
  allowed_unknown,
  blocked,
  invalid_request,
  read_only,
  failed,
};

struct OperationAuthorizationResult final {
  OperationAuthorizationCode code{OperationAuthorizationCode::failed};
  std::optional<domain::emergency_withdrawal::WithdrawalEntry> matching_notice;
  std::string notice_source;
  std::optional<WallClockTime> observed_at;
  std::string reason;
  std::uint64_t notice_revision{0};
  bool possibly_stale{false};
};

// Owns the device-level emergency-withdrawal safety fact. A notice is the full
// snapshot for its source and revision; a newer snapshot replaces only that
// source, while separate sources remain independently effective. It only
// fetches and applies notices; it never updates ordinary catalogs, starts
// operations, or infers a category from a UI page.
class EmergencyWithdrawalService final {
 public:
  EmergencyWithdrawalService(DeviceStateStore& states, Clock const& clock,
                             EmergencyWithdrawalNoticeSource& source,
                             EmergencyWithdrawalLogContext log = {}) noexcept;

  // Consumers call this for startup and each create/execute/retry/resume
  // boundary; the returned authorization is for one not-yet-started operation.
  [[nodiscard]] EmergencyWithdrawalCheckResult preflight_check();
  [[nodiscard]] EmergencyWithdrawalCheckResult check();
  [[nodiscard]] EmergencyWithdrawalSnapshot snapshot();
  [[nodiscard]] OperationAuthorizationResult authorize(
      domain::emergency_withdrawal::OperationTarget const& target);

 private:
  [[nodiscard]] domain::StateKey state_key() const;
  [[nodiscard]] EmergencyWithdrawalSnapshot load_snapshot(
      StateRead* read_result = nullptr);
  [[nodiscard]] EmergencyWithdrawalSnapshot load_snapshot(
      StateRead const& read) const;
  [[nodiscard]] StateCommitResult persist(
      EmergencyWithdrawalSnapshot const& snapshot,
      std::optional<domain::RevisionToken> expected_revision);
  [[nodiscard]] bool can_persist(
      EmergencyWithdrawalSnapshot const& snapshot) const;
  [[nodiscard]] bool has_version_scoped_withdrawal(
      EmergencyWithdrawalSnapshot const& snapshot,
      domain::emergency_withdrawal::OperationTarget const& target) const
      noexcept;
  [[nodiscard]] EmergencyWithdrawalCheckResult persist_notice(
      domain::emergency_withdrawal::EmergencyWithdrawalNotice notice);

  DeviceStateStore& states_;
  Clock const& clock_;
  EmergencyWithdrawalNoticeSource& source_;
  ExecutionLog* log_{};
  CorrelationId correlation_;
};

}  // namespace azzs::application
