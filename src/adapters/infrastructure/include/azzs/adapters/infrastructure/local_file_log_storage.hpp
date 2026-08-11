#pragma once

#include <memory>
#include <string>

#include "azzs/adapters/infrastructure/structured_execution_log.hpp"

namespace azzs::adapters::infrastructure {

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
