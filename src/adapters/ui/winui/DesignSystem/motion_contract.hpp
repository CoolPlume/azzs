#pragma once

#include <chrono>

namespace azzs::ui::presentation {

enum class MotionSemantic {
  immediate,
  keyboard_command,
  top_level_navigation,
  view_mode_switch,
  continuous_selection,
  progress_value_refresh,
  log_refresh,
  pointer_feedback,
  state_change,
  overlay_enter,
  overlay_exit,
};

enum class InputModality {
  mouse,
  touch,
  pen,
  keyboard,
  screen_reader,
};

[[nodiscard]] constexpr std::chrono::milliseconds motion_duration_for(
    MotionSemantic semantic,
    InputModality input,
    bool animations_enabled) noexcept {
  using namespace std::chrono_literals;

  if (!animations_enabled ||
      input == InputModality::keyboard ||
      input == InputModality::screen_reader) {
    return 0ms;
  }

  switch (semantic) {
    case MotionSemantic::immediate:
    case MotionSemantic::keyboard_command:
    case MotionSemantic::top_level_navigation:
    case MotionSemantic::view_mode_switch:
    case MotionSemantic::continuous_selection:
    case MotionSemantic::progress_value_refresh:
    case MotionSemantic::log_refresh:
      return 0ms;
    case MotionSemantic::pointer_feedback:
      return 83ms;
    case MotionSemantic::state_change:
    case MotionSemantic::overlay_exit:
      return 167ms;
    case MotionSemantic::overlay_enter:
      return 250ms;
  }

  return 0ms;
}

}  // namespace azzs::ui::presentation
