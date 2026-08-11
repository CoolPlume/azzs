#pragma once

#include <optional>
#include <string>
#include <vector>

#include "azzs/application/clock.hpp"
#include "azzs/application/state_file_system.hpp"
#include "azzs/domain/device_state.hpp"

namespace azzs::application {

enum class StateReadMode {
  uninitialized,
  writable,
  recovered_previous,
  read_only_future,
  read_only_corrupt,
  busy,
  failed,
};

enum class StateEvidenceKind {
  invalid_current,
  invalid_previous,
  future_current,
  future_previous,
  transaction_residual,
};

struct StateEvidence final {
  StateEvidenceKind kind{};
  StateFileSlot slot{};
  domain::StateBytes raw_bytes;
  std::string detail;
};

struct StateRead final {
  StateReadMode mode{StateReadMode::failed};
  std::optional<domain::DeviceStateSnapshot> snapshot;
  std::vector<StateEvidence> evidence;
  std::string error;
};

enum class StateCommitStatus {
  committed,
  conflict,
  read_only,
  busy,
  failed,
  outcome_unknown,
};

struct StateCommitResult final {
  StateCommitStatus status{StateCommitStatus::failed};
  std::optional<domain::DeviceStateSnapshot> snapshot;
  std::string failed_stage;
  std::string error;
};

struct StateCommitRequest final {
  domain::StateKey key;
  domain::RevisionToken expected_revision;
  domain::DeviceState state;
};

struct StateReinitializeRequest final {
  domain::StateKey key;
  domain::VersionedStateValue replacement;
  std::vector<domain::DataImpactCategory> affected_categories;
  std::string confirmation_reference;
};

enum class CheckpointStatus {
  available,
  absent,
  conflict,
  consumed,
  busy,
  failed,
  read_only,
};

struct StateCheckpoint final {
  domain::RevisionToken base_revision;
  domain::StateBytes payload;
};

struct CheckpointResult final {
  CheckpointStatus status{CheckpointStatus::failed};
  std::optional<StateCheckpoint> checkpoint;
  std::string error;
};

class DeviceStateStore final {
 public:
  DeviceStateStore(StateFileSystem& files, Clock const& clock) noexcept;

  [[nodiscard]] StateRead inspect(domain::StateKey const& key);
  [[nodiscard]] StateCommitResult initialize(domain::StateKey const& key,
                                             domain::DeviceState state);
  [[nodiscard]] StateCommitResult commit(StateCommitRequest request);
  [[nodiscard]] StateCommitResult reinitialize(
      StateReinitializeRequest request);

  [[nodiscard]] CheckpointResult write_checkpoint(
      domain::StateKey const& key, StateCheckpoint checkpoint);
  [[nodiscard]] CheckpointResult read_checkpoint(
      domain::StateKey const& key,
      domain::RevisionToken const& current_revision);
  [[nodiscard]] CheckpointResult consume_checkpoint(
      domain::StateKey const& key,
      domain::RevisionToken const& current_revision);

 private:
  StateFileSystem& files_;
  Clock const& clock_;
};

}  // namespace azzs::application
