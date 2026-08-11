#pragma once

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "azzs/application/state_file_system.hpp"

namespace azzs::testing {

enum class StateFileOperation {
  lock,
  read,
  write,
  flush,
  replace,
  remove,
  flush_volume,
};

class InMemoryStateFileSystem final : public application::StateFileSystem {
 public:
  struct Fault final {
    StateFileOperation operation{};
    std::optional<application::StateFileSlot> slot;
    std::size_t occurrence{1};
    std::string error{"injected state file failure"};
  };

  [[nodiscard]] application::StateFileLockResult try_lock(
      domain::StateKey const& key) override {
    std::scoped_lock lock{mutex_};
    if (auto error =
            take_failure_locked(StateFileOperation::lock, std::nullopt)) {
      return {.status = application::StateFileLockStatus::failed,
              .error = std::move(*error)};
    }

    auto const id = address_id(key);
    if (locked_.contains(id)) {
      return {.status = application::StateFileLockStatus::busy};
    }
    locked_.insert(id);
    return {.status = application::StateFileLockStatus::acquired,
            .lock = std::make_unique<MemoryLock>(*this, id)};
  }

  [[nodiscard]] application::StateFileRead read(
      domain::StateKey const& key,
      application::StateFileSlot slot) override {
    std::scoped_lock lock{mutex_};
    if (auto error = take_failure_locked(StateFileOperation::read, slot)) {
      return {.status = application::StateIoStatus::failed,
              .error = std::move(*error)};
    }
    auto const id = file_id(key, slot);
    auto const found = files_.find(id);
    if (found == files_.end()) {
      return {};
    }
    return {.status = application::StateIoStatus::succeeded,
            .bytes = found->second};
  }

  [[nodiscard]] application::StateIoResult write(
      domain::StateKey const& key, application::StateFileSlot slot,
      std::span<std::byte const> bytes) override {
    std::scoped_lock lock{mutex_};
    if (auto error = take_failure_locked(StateFileOperation::write, slot)) {
      return {.status = application::StateIoStatus::failed,
              .error = std::move(*error)};
    }
    files_[file_id(key, slot)] = domain::StateBytes{bytes.begin(), bytes.end()};
    return {};
  }

  [[nodiscard]] application::StateIoResult flush(
      domain::StateKey const&, application::StateFileSlot slot) override {
    std::scoped_lock lock{mutex_};
    if (auto error = take_failure_locked(StateFileOperation::flush, slot)) {
      return {.status = application::StateIoStatus::failed,
              .error = std::move(*error)};
    }
    return {};
  }

  [[nodiscard]] application::StateIoResult replace(
      domain::StateKey const& key, application::StateFileSlot source,
      application::StateFileSlot target) override {
    std::scoped_lock lock{mutex_};
    if (auto error = take_failure_locked(StateFileOperation::replace, target)) {
      return {.status = application::StateIoStatus::failed,
              .error = std::move(*error)};
    }

    auto const source_id = file_id(key, source);
    auto const found = files_.find(source_id);
    if (found == files_.end()) {
      return {.status = application::StateIoStatus::failed,
              .error = "state replace source is absent"};
    }
    files_[file_id(key, target)] = std::move(found->second);
    files_.erase(found);
    return {};
  }

  [[nodiscard]] application::StateIoResult remove(
      domain::StateKey const& key,
      application::StateFileSlot slot) override {
    std::scoped_lock lock{mutex_};
    if (auto error = take_failure_locked(StateFileOperation::remove, slot)) {
      return {.status = application::StateIoStatus::failed,
              .error = std::move(*error)};
    }
    files_.erase(file_id(key, slot));
    return {};
  }

  [[nodiscard]] application::StateIoResult flush_volume(
      domain::StateKey const&) override {
    std::scoped_lock lock{mutex_};
    if (auto error =
            take_failure_locked(StateFileOperation::flush_volume,
                                std::nullopt)) {
      return {.status = application::StateIoStatus::failed,
              .error = std::move(*error)};
    }
    return {};
  }

  void fail_on(Fault fault) {
    std::scoped_lock lock{mutex_};
    faults_.push_back(std::move(fault));
  }

  void fail_next(StateFileOperation operation,
                 std::optional<application::StateFileSlot> slot =
                     std::nullopt,
                 std::string error = "injected state file failure") {
    fail_on(Fault{.operation = operation,
                  .slot = slot,
                  .error = std::move(error)});
  }

  void seed(domain::StateKey const& key, application::StateFileSlot slot,
            domain::StateBytes bytes) {
    std::scoped_lock lock{mutex_};
    files_[file_id(key, slot)] = std::move(bytes);
  }

  [[nodiscard]] std::optional<domain::StateBytes> raw_file(
      domain::StateKey const& key,
      application::StateFileSlot slot) const {
    std::scoped_lock lock{mutex_};
    auto const found = files_.find(file_id(key, slot));
    if (found == files_.end()) {
      return std::nullopt;
    }
    return found->second;
  }

  void corrupt(domain::StateKey const& key,
               application::StateFileSlot slot) {
    std::scoped_lock lock{mutex_};
    auto found = files_.find(file_id(key, slot));
    if (found == files_.end() || found->second.empty()) {
      return;
    }
    auto& target = found->second.back();
    target ^= std::byte{0x5a};
  }

 private:
  class MemoryLock final : public application::StateFileLock {
   public:
    MemoryLock(InMemoryStateFileSystem& owner, std::string id)
        : owner_(&owner), id_(std::move(id)) {}

    ~MemoryLock() override {
      if (owner_ != nullptr) {
        owner_->release(id_);
      }
    }

    MemoryLock(MemoryLock const&) = delete;
    MemoryLock& operator=(MemoryLock const&) = delete;

   private:
    InMemoryStateFileSystem* owner_;
    std::string id_;
  };

  [[nodiscard]] static std::string address_id(domain::StateKey const& key) {
    std::string result =
        key.partition == domain::StatePartition::device ? "device/" :
                                                          "subject/";
    if (key.subject.has_value()) {
      result += key.subject->value;
      result.push_back('/');
    }
    result += key.aggregate.value;
    return result;
  }

  [[nodiscard]] static std::string file_id(
      domain::StateKey const& key, application::StateFileSlot slot) {
    auto result = address_id(key);
    result.push_back('#');
    result += std::to_string(static_cast<int>(slot));
    return result;
  }

  void release(std::string const& id) {
    std::scoped_lock lock{mutex_};
    locked_.erase(id);
  }

  [[nodiscard]] std::optional<std::string> take_failure_locked(
      StateFileOperation operation,
      std::optional<application::StateFileSlot> slot) {
    auto const found = std::find_if(
        faults_.begin(), faults_.end(), [&](Fault const& fault) {
          return fault.operation == operation && fault.slot == slot;
        });
    if (found == faults_.end()) {
      return std::nullopt;
    }
    if (found->occurrence > 1) {
      --found->occurrence;
      return std::nullopt;
    }
    auto error = std::move(found->error);
    faults_.erase(found);
    return error;
  }

  mutable std::mutex mutex_;
  std::map<std::string, domain::StateBytes> files_;
  std::set<std::string> locked_;
  std::vector<Fault> faults_;
};

}  // namespace azzs::testing
