#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "motion_contract.hpp"

namespace azzs::ui::winui {

class MotionPreferences final
    : public std::enable_shared_from_this<MotionPreferences> {
 public:
  using CancellationHandler = std::function<void()>;

  [[nodiscard]] static std::shared_ptr<MotionPreferences> create();

  MotionPreferences(MotionPreferences const&) = delete;
  MotionPreferences& operator=(MotionPreferences const&) = delete;
  ~MotionPreferences();

  [[nodiscard]] bool animations_enabled() const noexcept;
  [[nodiscard]] std::chrono::milliseconds duration_for(
      presentation::MotionSemantic semantic,
      presentation::InputModality input) const noexcept;

  [[nodiscard]] std::size_t register_cancellation_handler(
      CancellationHandler handler);
  void unregister_cancellation_handler(std::size_t token) noexcept;

 private:
  MotionPreferences();

  void start_listening();
  void on_animations_enabled_changed(bool enabled) noexcept;
  void apply_animations_enabled(bool enabled);

  winrt::Windows::UI::ViewManagement::UISettings settings_;
  winrt::Microsoft::UI::Dispatching::DispatcherQueue dispatcher_queue_;
  winrt::event_token animations_enabled_changed_token_{};
  std::atomic_bool animations_enabled_{true};
  std::mutex handlers_mutex_;
  std::unordered_map<std::size_t, CancellationHandler> handlers_;
  std::size_t next_handler_token_{1};
};

}  // namespace azzs::ui::winui
