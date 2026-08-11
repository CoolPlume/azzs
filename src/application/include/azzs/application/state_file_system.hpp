#pragma once

#include <memory>
#include <span>
#include <string>

#include "azzs/domain/device_state.hpp"

namespace azzs::application {

enum class StateFileSlot {
  current,
  previous,
  candidate,
  previous_staging,
  intent,
  checkpoint,
  checkpoint_staging,
  checkpoint_consumed,
  checkpoint_consumed_staging,
  corrupt_archive,
  corrupt_archive_staging,
  corrupt_current,
  corrupt_previous,
  corrupt_candidate,
  corrupt_intent,
  corrupt_previous_staging,
};

enum class StateIoStatus {
  succeeded,
  not_found,
  failed,
};

struct StateFileRead final {
  StateIoStatus status{StateIoStatus::not_found};
  domain::StateBytes bytes;
  std::string error;
};

struct StateIoResult final {
  StateIoStatus status{StateIoStatus::succeeded};
  std::string error;

  [[nodiscard]] bool succeeded() const noexcept {
    return status == StateIoStatus::succeeded;
  }
};

class StateFileLock {
 public:
  virtual ~StateFileLock() = default;
};

enum class StateFileLockStatus {
  acquired,
  busy,
  failed,
};

struct StateFileLockResult final {
  StateFileLockStatus status{StateFileLockStatus::failed};
  std::unique_ptr<StateFileLock> lock;
  std::string error;
};

class StateFileSystem {
 public:
  virtual ~StateFileSystem() = default;

  [[nodiscard]] virtual StateFileLockResult try_lock(
      domain::StateKey const& key) = 0;
  [[nodiscard]] virtual StateFileRead read(domain::StateKey const& key,
                                           StateFileSlot slot) = 0;
  [[nodiscard]] virtual StateIoResult write(
      domain::StateKey const& key, StateFileSlot slot,
      std::span<std::byte const> bytes) = 0;
  [[nodiscard]] virtual StateIoResult flush(domain::StateKey const& key,
                                            StateFileSlot slot) = 0;
  [[nodiscard]] virtual StateIoResult replace(domain::StateKey const& key,
                                              StateFileSlot source,
                                              StateFileSlot target) = 0;
  [[nodiscard]] virtual StateIoResult remove(domain::StateKey const& key,
                                             StateFileSlot slot) = 0;
  [[nodiscard]] virtual StateIoResult flush_volume(
      domain::StateKey const& key) = 0;
};

}  // namespace azzs::application
