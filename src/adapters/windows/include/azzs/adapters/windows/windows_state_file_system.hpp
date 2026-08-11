#pragma once

#include <memory>
#include <span>

#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/application/state_file_system.hpp"

namespace azzs::adapters::windows {

class WindowsStateFileSystem final : public application::StateFileSystem {
 public:
  explicit WindowsStateFileSystem(DeviceDataEnvironment environment);
  ~WindowsStateFileSystem() override;

  WindowsStateFileSystem(WindowsStateFileSystem const&) = delete;
  WindowsStateFileSystem& operator=(WindowsStateFileSystem const&) = delete;
  WindowsStateFileSystem(WindowsStateFileSystem&&) noexcept;
  WindowsStateFileSystem& operator=(WindowsStateFileSystem&&) noexcept;

  [[nodiscard]] application::StateFileLockResult try_lock(
      domain::StateKey const& key) override;
  [[nodiscard]] application::StateFileRead read(
      domain::StateKey const& key,
      application::StateFileSlot slot) override;
  [[nodiscard]] application::StateIoResult write(
      domain::StateKey const& key,
      application::StateFileSlot slot,
      std::span<std::byte const> bytes) override;
  [[nodiscard]] application::StateIoResult flush(
      domain::StateKey const& key,
      application::StateFileSlot slot) override;
  [[nodiscard]] application::StateIoResult replace(
      domain::StateKey const& key,
      application::StateFileSlot source,
      application::StateFileSlot target) override;
  [[nodiscard]] application::StateIoResult remove(
      domain::StateKey const& key,
      application::StateFileSlot slot) override;
  [[nodiscard]] application::StateIoResult flush_volume(
      domain::StateKey const& key) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace azzs::adapters::windows
