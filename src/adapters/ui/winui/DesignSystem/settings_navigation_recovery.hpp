#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace azzs::ui::presentation {

// These stages are intentionally UI-facing. The application core owns the
// underlying error; the bridge only records where the presentation attempt
// stopped so a headless fixture can exercise the same recovery contract.
enum class SettingsNavigationFailureStage {
  optional_value_missing,
  snapshot_read,
  page_binding,
  resource_projection,
  commit,
  unknown,
};

struct SettingsNavigationFailure final {
  SettingsNavigationFailureStage stage{
      SettingsNavigationFailureStage::unknown};
  std::string detail;
};

struct SettingsNavigationPreparation final {
  // A prepared transaction owns all work that may replace the visible page.
  // It is invoked only after prepare() reports success.
  std::function<void()> commit;
  std::optional<SettingsNavigationFailure> failure;
  // The caller captures the state that was current for this attempt. The
  // bridge invokes this exactly once before presenting a failure, including a
  // partially completed commit.
  std::function<void()> recover;
};

struct SettingsNavigationCallbacks final {
  std::function<bool()> already_visible;
  std::function<SettingsNavigationPreparation()> prepare;
  std::function<void(SettingsNavigationFailure const&)> present_failure;
  std::function<void()> clear_failure;
};

// UI-independent transaction and recovery boundary for application-settings
// navigation. Preparation must finish before commit, so a failed snapshot or
// binding cannot publish a new page or change the core page state.
class SettingsNavigationBridge final {
 public:
  [[nodiscard]] bool navigate(SettingsNavigationCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
    attempts_ = 0;
    return execute();
  }

  [[nodiscard]] bool retry() {
    if (!callbacks_.has_value()) {
      return false;
    }
    return execute();
  }

  void return_to_current() noexcept {
    // Detach callbacks before projecting the InfoBar. A projection callback
    // must not be able to re-enter a stale retry transaction.
    auto callbacks = std::move(callbacks_);
    callbacks_.reset();
    failed_ = false;
    last_failure_.reset();
    if (callbacks.has_value() && callbacks->clear_failure) {
      try {
        callbacks->clear_failure();
      } catch (...) {
        // Clearing an informational surface is best effort.
      }
    }
  }

  // A completed navigation or an explicit return ends the transaction. This
  // releases callbacks that capture the previous page and prevents a stale
  // retry from mutating a later navigation state.
  void invalidate() noexcept {
    callbacks_.reset();
    failed_ = false;
    last_failure_.reset();
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }

  [[nodiscard]] std::size_t attempts() const noexcept { return attempts_; }

  [[nodiscard]] std::optional<SettingsNavigationFailure> const&
  last_failure() const noexcept {
    return last_failure_;
  }

 private:
  [[nodiscard]] bool execute() {
    if (!callbacks_.has_value()) {
      failed_ = true;
      last_failure_ = SettingsNavigationFailure{
          .stage = SettingsNavigationFailureStage::unknown,
          .detail = "settings navigation has no active transaction"};
      return false;
    }

    bool already_visible = false;
    try {
      already_visible = callbacks_->already_visible &&
                        callbacks_->already_visible();
    } catch (...) {
      fail({.stage = SettingsNavigationFailureStage::unknown,
            .detail = "settings navigation visibility check threw"});
      return false;
    }
    if (already_visible) {
      failed_ = false;
      last_failure_.reset();
      clear_failure_noexcept();
      invalidate();
      return true;
    }
    ++attempts_;
    if (!callbacks_->prepare || !callbacks_->present_failure ||
        !callbacks_->clear_failure) {
      fail({.stage = SettingsNavigationFailureStage::unknown,
            .detail = "settings navigation callbacks are incomplete"});
      return false;
    }

    SettingsNavigationPreparation preparation;
    try {
      preparation = callbacks_->prepare();
    } catch (...) {
      fail({.stage = SettingsNavigationFailureStage::unknown,
            .detail = "settings navigation preparation threw"});
      return false;
    }

    if (preparation.failure.has_value()) {
      fail(*preparation.failure, std::move(preparation.recover));
      return false;
    }
    if (!preparation.commit) {
      fail({.stage = SettingsNavigationFailureStage::resource_projection,
            .detail = "settings navigation preparation returned no commit"},
           std::move(preparation.recover));
      return false;
    }

    try {
      preparation.commit();
    } catch (...) {
      fail({.stage = SettingsNavigationFailureStage::commit,
            .detail = "settings navigation commit threw"},
           std::move(preparation.recover));
      return false;
    }

    failed_ = false;
    last_failure_.reset();
    clear_failure_noexcept();
    invalidate();
    return true;
  }

  void fail(SettingsNavigationFailure failure,
            std::function<void()> recover = {}) noexcept {
    failed_ = true;
    last_failure_ = std::move(failure);
    if (recover) {
      try {
        recover();
      } catch (...) {
        // Recovery must not turn the original navigation failure into an
        // unhandled exception or prevent the user-facing error state.
      }
    }
    if (!callbacks_.has_value() || !callbacks_->present_failure) {
      return;
    }
    try {
      auto const failure_copy = *last_failure_;
      callbacks_->present_failure(failure_copy);
    } catch (...) {
      // The recovery boundary itself must not turn a projection failure into
      // an unhandled exception. The retained state still exposes failure.
    }
  }

  void clear_failure_noexcept() noexcept {
    if (!callbacks_.has_value() || !callbacks_->clear_failure) {
      return;
    }
    try {
      callbacks_->clear_failure();
    } catch (...) {
      // Clearing an informational surface is best effort and never owns the
      // core navigation result.
    }
  }

  std::optional<SettingsNavigationCallbacks> callbacks_;
  std::optional<SettingsNavigationFailure> last_failure_;
  std::size_t attempts_{0};
  bool failed_{false};
};

}  // namespace azzs::ui::presentation
