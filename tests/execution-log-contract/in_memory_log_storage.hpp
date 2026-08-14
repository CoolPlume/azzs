#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/structured_execution_log.hpp"

namespace azzs::testing {

class InMemoryLogStorage final
    : public adapters::infrastructure::LogStorage {
 public:
  [[nodiscard]] std::unique_ptr<
      adapters::infrastructure::LogStorageTransaction>
  begin_transaction() override {
    class Transaction final
        : public adapters::infrastructure::LogStorageTransaction {
     public:
      explicit Transaction(InMemoryLogStorage& owner)
          : owner_(owner), lock_(owner.mutex_) {}

      [[nodiscard]] std::string_view bytes() const noexcept override {
        return owner_.bytes_;
      }

      [[nodiscard]] std::string_view read_error() const noexcept override {
        return owner_.read_error_;
      }

      [[nodiscard]] adapters::infrastructure::LogStorageWriteResult replace(
          std::string bytes) override {
        if (owner_.next_failure_.has_value()) {
          if (owner_.failure_occurrence_ > 1) {
            --owner_.failure_occurrence_;
          } else {
            auto error = std::move(*owner_.next_failure_);
            owner_.next_failure_.reset();
            owner_.failure_occurrence_ = 1;
            return {.error = std::move(error)};
          }
        }
        owner_.bytes_ = std::move(bytes);
        owner_.revisions_.push_back(owner_.bytes_);
        if (owner_.next_unverified_replace_.has_value()) {
          auto error = std::move(*owner_.next_unverified_replace_);
          owner_.next_unverified_replace_.reset();
          return {.committed = true,
                  .verified = false,
                  .error = std::move(error)};
        }
        return {.committed = true, .verified = true};
      }

      [[nodiscard]] adapters::infrastructure::LogStorageExportResult
      write_diagnostic(std::string bytes) override {
        if (owner_.next_export_failure_.has_value()) {
          auto error = std::move(*owner_.next_export_failure_);
          owner_.next_export_failure_.reset();
          return {.error = std::move(error)};
        }
        owner_.exports_.push_back(std::move(bytes));
        if (owner_.next_unverified_export_.has_value()) {
          auto error = std::move(*owner_.next_unverified_export_);
          owner_.next_unverified_export_.reset();
          return {.written = true,
                  .verified = false,
                  .file_name = "diagnostic-unverified.azzsdiag",
                  .error = std::move(error)};
        }
        return {.written = true,
                .verified = true,
                .file_name = "diagnostic-" +
                             std::to_string(owner_.exports_.size()) +
                             ".azzsdiag"};
      }

     private:
      InMemoryLogStorage& owner_;
      std::unique_lock<std::mutex> lock_;
    };

    return std::make_unique<Transaction>(*this);
  }

  [[nodiscard]] std::string bytes() const {
    std::scoped_lock lock{mutex_};
    return bytes_;
  }

  [[nodiscard]] std::vector<std::string> revisions() const {
    std::scoped_lock lock{mutex_};
    return revisions_;
  }

  void set_unrelated_aggregate(std::string bytes) {
    std::scoped_lock lock{mutex_};
    unrelated_aggregate_ = std::move(bytes);
  }

  [[nodiscard]] std::string unrelated_aggregate() const {
    std::scoped_lock lock{mutex_};
    return unrelated_aggregate_;
  }

  void fail_next_replace(std::string error) {
    std::scoped_lock lock{mutex_};
    next_failure_ = std::move(error);
    failure_occurrence_ = 1;
  }

  void fail_replace_on(std::size_t occurrence, std::string error) {
    std::scoped_lock lock{mutex_};
    next_failure_ = std::move(error);
    failure_occurrence_ = occurrence == 0 ? 1 : occurrence;
  }

  void fail_next_export(std::string error) {
    std::scoped_lock lock{mutex_};
    next_export_failure_ = std::move(error);
  }

  void set_read_error(std::string error) {
    std::scoped_lock lock{mutex_};
    read_error_ = std::move(error);
  }

  void publish_unverified_on_next_replace(std::string error) {
    std::scoped_lock lock{mutex_};
    next_unverified_replace_ = std::move(error);
  }

  void publish_unverified_on_next_export(std::string error) {
    std::scoped_lock lock{mutex_};
    next_unverified_export_ = std::move(error);
  }

  [[nodiscard]] std::vector<std::string> exports() const {
    std::scoped_lock lock{mutex_};
    return exports_;
  }

 private:
  mutable std::mutex mutex_;
  std::string bytes_;
  std::vector<std::string> revisions_;
  std::vector<std::string> exports_;
  std::string unrelated_aggregate_;
  std::string read_error_;
  std::optional<std::string> next_failure_;
  std::size_t failure_occurrence_{1};
  std::optional<std::string> next_export_failure_;
  std::optional<std::string> next_unverified_replace_;
  std::optional<std::string> next_unverified_export_;
};

}  // namespace azzs::testing
