#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/domain/device_state.hpp"

namespace azzs::application {
class Clock;
class DeviceStateStore;
}

namespace azzs::application::guided_initialization {

// The four stages are part of the user-visible product contract. Their order
// is fixed; individual stage work remains owned by its existing service.
enum class Stage : std::uint8_t {
  drivers,
  system_optimization,
  software_installation,
  software_optimization,
};

enum class StageState : std::uint8_t {
  pending,
  active,
  completed,
  skipped,
  no_applicable_items,
  partial,
  failed,
  result_confirmation_pending,
  waiting_explorer_restart,
  waiting_for_restart,
  emergency_withdrawn,
  external_handoff,
  not_executed,
};

enum class FlowState : std::uint8_t {
  active,
  waiting_for_restart,
  awaiting_restart_continue,
  completed,
  cancelled,
};

enum class LifecycleMode : std::uint8_t {
  not_restored,
  ready,
  read_only,
  failed,
};

enum class RestartGateState : std::uint8_t {
  none,
  waiting_for_windows_restart,
  awaiting_read_only_verification,
  awaiting_user_continue,
  read_only,
};

enum class ExternalHandoffState : std::uint8_t {
  waiting_for_external_install,
  externally_recognized,
  skipped,
};

struct StageEvidence final {
  StageState state{StageState::pending};
  std::string detail;
};

struct ExternalHandoffEvidence final {
  std::string software_id;
  ExternalHandoffState state{ExternalHandoffState::waiting_for_external_install};
};

// This is a read-only projection of the services that already own drivers,
// settings, selection, batches, catalog identity, and restart barriers.
struct Evidence final {
  StageEvidence drivers;
  StageEvidence system_optimization;
  StageEvidence software_installation;
  StageEvidence software_optimization;
  RestartGateState restart_gate{RestartGateState::none};
  bool local_trial_software_catalog{false};
  std::vector<ExternalHandoffEvidence> external_handoffs;
};

class EvidenceSource {
 public:
  virtual ~EvidenceSource() = default;

  [[nodiscard]] virtual Evidence observe() = 0;
  [[nodiscard]] virtual bool continue_external_handoff(
      std::string_view software_id, std::string& error) = 0;
  [[nodiscard]] virtual bool continue_after_restart(std::string& error) = 0;
};

struct StageRecord final {
  Stage stage{Stage::drivers};
  StageState state{StageState::pending};
  std::string detail;
};

struct FlowRecord final {
  std::string id;
  std::int64_t created_at_milliseconds{};
  std::int64_t updated_at_milliseconds{};
  FlowState state{FlowState::active};
  Stage current_stage{Stage::drivers};
  std::array<StageRecord, 4> stages;
  // The selection lifecycle owns each handoff's URL, detection timeline and
  // mutable current projection. A flow retains only stable references.
  std::vector<std::string> external_handoff_software_ids;
};

struct Summary final {
  std::size_t completed{};
  std::size_t externally_recognized{};
  std::size_t partial{};
  std::size_t failed{};
  std::size_t skipped{};
  std::size_t no_applicable_items{};
  std::size_t not_executed{};
  std::size_t result_confirmation_pending{};
  std::size_t waiting_for_restart{};
  std::size_t waiting_explorer_restart{};
  std::size_t emergency_withdrawn{};
  bool retry_available{false};
};

struct Snapshot final {
  LifecycleMode mode{LifecycleMode::not_restored};
  bool writable{false};
  std::optional<FlowRecord> active;
  std::vector<FlowRecord> history;
  Evidence evidence;
  Summary summary;
  std::string error;
};

enum class ActionCode : std::uint8_t {
  succeeded,
  not_restored,
  read_only,
  rejected,
  no_active_flow,
  continuation_required,
  persistence_failed,
  source_rejected,
};

struct ActionResult final {
  ActionCode code{ActionCode::rejected};
  Snapshot snapshot;
  std::string message;

  [[nodiscard]] bool succeeded() const noexcept {
    return code == ActionCode::succeeded;
  }
};

// Owns durable recommended-flow records. It never creates a settings run,
// installation batch, optimization batch, source handoff, or restart gate.
// Those services remain the only writers of their corresponding business data.
class GuidedInitializationService final {
 public:
  GuidedInitializationService(DeviceStateStore& states, Clock const& clock,
                              EvidenceSource& evidence_source);

  [[nodiscard]] ActionResult restore();
  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] ActionResult refresh();
  [[nodiscard]] ActionResult start();
  [[nodiscard]] ActionResult mark_driver_completed();
  [[nodiscard]] ActionResult skip_current_stage();
  [[nodiscard]] ActionResult continue_current_stage();
  [[nodiscard]] ActionResult continue_external_handoff(
      std::string_view software_id);
  [[nodiscard]] ActionResult continue_after_restart();
  [[nodiscard]] ActionResult retry_current_stage();
  [[nodiscard]] ActionResult cancel();

 private:
  [[nodiscard]] ActionResult make_result(ActionCode code,
                                          std::string message = {}) const;
  [[nodiscard]] bool persist(std::string& error);
  [[nodiscard]] bool synchronize_from_evidence(std::string& error,
                                                bool& changed);
  [[nodiscard]] bool apply_stage_evidence(FlowRecord& record,
                                          StageEvidence const& evidence);
  [[nodiscard]] bool advance(FlowRecord& record);
  [[nodiscard]] StageEvidence const& evidence_for(Stage stage) const noexcept;
  [[nodiscard]] StageRecord& stage_for(FlowRecord& record) noexcept;
  [[nodiscard]] StageRecord const& stage_for(FlowRecord const& record) const
      noexcept;
  [[nodiscard]] FlowRecord* active_record() noexcept;
  [[nodiscard]] FlowRecord const* active_record() const noexcept;
  [[nodiscard]] std::int64_t now_milliseconds() const noexcept;
  [[nodiscard]] Summary summarize(FlowRecord const& record) const;

  DeviceStateStore& states_;
  Clock const& clock_;
  EvidenceSource& evidence_source_;
  LifecycleMode mode_{LifecycleMode::not_restored};
  bool writable_{false};
  std::optional<domain::RevisionToken> revision_;
  std::vector<FlowRecord> records_;
  std::optional<std::string> active_id_;
  std::uint64_t next_sequence_{1};
  Evidence evidence_;
  std::string error_;
};

[[nodiscard]] char const* to_string(Stage value) noexcept;
[[nodiscard]] char const* to_string(StageState value) noexcept;
[[nodiscard]] char const* to_string(FlowState value) noexcept;
[[nodiscard]] char const* to_string(ActionCode value) noexcept;

}  // namespace azzs::application::guided_initialization
