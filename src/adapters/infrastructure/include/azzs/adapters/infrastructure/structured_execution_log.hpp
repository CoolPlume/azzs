#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "azzs/application/clock.hpp"
#include "azzs/application/execution_log.hpp"

namespace azzs::adapters::infrastructure {

struct LogStorageWriteResult final {
  // committed may be true when publication happened but its durability or
  // reread could not be confirmed. Only verified is a successful log commit.
  bool committed{false};
  bool verified{false};
  std::string error;
};

struct LogStorageExportResult final {
  // written records a visible artifact; callers expose it only when verified.
  bool written{false};
  bool verified{false};
  std::string file_name;
  std::string error;
};

class LogStorageTransaction {
 public:
  virtual ~LogStorageTransaction() = default;

  [[nodiscard]] virtual std::string_view bytes() const noexcept = 0;
  [[nodiscard]] virtual LogStorageWriteResult replace(
      std::string bytes) = 0;
  [[nodiscard]] virtual LogStorageExportResult write_diagnostic(
      std::string bytes) = 0;
};

class LogStorage {
 public:
  virtual ~LogStorage() = default;

  // The transaction holds the storage's cross-instance serialization lock.
  [[nodiscard]] virtual std::unique_ptr<LogStorageTransaction>
  begin_transaction() = 0;
};

class StructuredExecutionLog final : public application::ExecutionLog {
 public:
  StructuredExecutionLog(LogStorage& storage, application::Clock const& clock);

  [[nodiscard]] application::CorrelationId begin_correlation() override;
  [[nodiscard]] application::ExecutionLogReceipt append(
      application::CorrelationId const& correlation,
      application::ExecutionEvent const& event) override;
  [[nodiscard]] application::ExecutionLogClearReceipt clear() override;
  [[nodiscard]] application::DiagnosticExportReceipt export_diagnostic(
      application::DiagnosticContext const& context) override;

 private:
  LogStorage& storage_;
  application::Clock const& clock_;
};

}  // namespace azzs::adapters::infrastructure
