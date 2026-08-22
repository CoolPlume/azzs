#pragma once

#include <memory>
#include <string>

#include "azzs/adapters/infrastructure/structured_execution_log.hpp"

namespace azzs::adapters::infrastructure {

#ifdef _WIN32
// Kept at the Windows adapter boundary so the capacity policy can classify
// quota exhaustion without leaking a Win32 error code into the application.
[[nodiscard]] LogStorageWriteFailure classify_windows_log_storage_write_failure(
    unsigned long error) noexcept;
#endif

class LocalFileLogStorage final : public LogStorage {
 public:
  class Impl;

  LocalFileLogStorage(std::string device_root_utf8,
                      std::string subject_id);
  ~LocalFileLogStorage() override;

  LocalFileLogStorage(LocalFileLogStorage const&) = delete;
  LocalFileLogStorage& operator=(LocalFileLogStorage const&) = delete;
  LocalFileLogStorage(LocalFileLogStorage&&) noexcept;
  LocalFileLogStorage& operator=(LocalFileLogStorage&&) noexcept;

  [[nodiscard]] std::unique_ptr<LogStorageTransaction>
 begin_transaction() override;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace azzs::adapters::infrastructure
