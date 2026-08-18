#include "pch.h"

#include "motion_preferences.hpp"

#include <utility>
#include <vector>

namespace azzs::ui::winui {

std::shared_ptr<MotionPreferences> MotionPreferences::create() {
  auto preferences =
      std::shared_ptr<MotionPreferences>{new MotionPreferences{false}};
  preferences->start_listening();
  return preferences;
}

std::shared_ptr<MotionPreferences> MotionPreferences::create_static() {
  return std::shared_ptr<MotionPreferences>{new MotionPreferences{true}};
}

MotionPreferences::MotionPreferences(bool static_presentation)
    : animations_enabled_{!static_presentation} {
  if (static_presentation) {
    return;
  }

  settings_.emplace();
  dispatcher_queue_ =
      winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
  animations_enabled_.store(settings_->AnimationsEnabled(),
                            std::memory_order_relaxed);
}

MotionPreferences::~MotionPreferences() {
  try {
    if (settings_.has_value() && animations_enabled_changed_token_.value != 0) {
      settings_->AnimationsEnabledChanged(animations_enabled_changed_token_);
    }
  } catch (...) {
  }
}

bool MotionPreferences::animations_enabled() const noexcept {
  return animations_enabled_.load(std::memory_order_relaxed);
}

std::chrono::milliseconds MotionPreferences::duration_for(
    presentation::MotionSemantic semantic,
    presentation::InputModality input) const noexcept {
  return presentation::motion_duration_for(semantic, input,
                                           animations_enabled());
}

std::size_t MotionPreferences::register_cancellation_handler(
    CancellationHandler handler) {
  if (!handler) {
    return 0;
  }

  std::scoped_lock lock{handlers_mutex_};
  auto const token = next_handler_token_++;
  handlers_.emplace(token, std::move(handler));
  return token;
}

void MotionPreferences::unregister_cancellation_handler(
    std::size_t token) noexcept {
  std::scoped_lock lock{handlers_mutex_};
  handlers_.erase(token);
}

void MotionPreferences::start_listening() {
  std::weak_ptr<MotionPreferences> weak_preferences{shared_from_this()};
  animations_enabled_changed_token_ = settings_->AnimationsEnabledChanged(
      [weak_preferences](auto const& sender, auto const&) noexcept {
        try {
          if (auto preferences = weak_preferences.lock()) {
            preferences->on_animations_enabled_changed(
                sender.AnimationsEnabled());
          }
        } catch (...) {
        }
      });
}

void MotionPreferences::on_animations_enabled_changed(bool enabled) noexcept {
  try {
    if (!dispatcher_queue_ || dispatcher_queue_->HasThreadAccess()) {
      apply_animations_enabled(enabled);
      return;
    }

    std::weak_ptr<MotionPreferences> weak_preferences{shared_from_this()};
    dispatcher_queue_->TryEnqueue([weak_preferences, enabled] {
      if (auto preferences = weak_preferences.lock()) {
        preferences->apply_animations_enabled(enabled);
      }
    });
  } catch (...) {
    animations_enabled_.store(enabled, std::memory_order_relaxed);
  }
}

void MotionPreferences::apply_animations_enabled(bool enabled) {
  auto const was_enabled =
      animations_enabled_.exchange(enabled, std::memory_order_relaxed);
  if (!was_enabled || enabled) {
    return;
  }

  std::vector<CancellationHandler> handlers;
  {
    std::scoped_lock lock{handlers_mutex_};
    handlers.reserve(handlers_.size());
    for (auto const& [_, handler] : handlers_) {
      handlers.push_back(handler);
    }
  }

  for (auto const& handler : handlers) {
    try {
      handler();
    } catch (...) {
    }
  }
}

}  // namespace azzs::ui::winui
