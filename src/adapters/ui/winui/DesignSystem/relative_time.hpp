#pragma once

#include <string>

#include "azzs/application/clock.hpp"

namespace azzs::ui::presentation {

struct RelativeTimeLabels {
  std::wstring just_now;
  std::wstring minutes;
  std::wstring hours;
  std::wstring calendar;
  std::wstring unavailable;
};

// Formats a persisted wall-clock fact with caller-supplied localized templates.
// The explicit `now` parameter keeps boundary behavior deterministic in tests.
[[nodiscard]] std::wstring format_relative_time(
    application::WallClockTime checked_at,
    application::WallClockTime now,
    RelativeTimeLabels const& labels);

}  // namespace azzs::ui::presentation
