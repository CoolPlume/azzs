#pragma once

#include <string>

#include "azzs/application/clock.hpp"

namespace azzs::ui::presentation {

// Formats a persisted wall-clock fact for the Chinese settings surface. The
// explicit `now` parameter keeps the boundary behavior deterministic in tests.
[[nodiscard]] std::wstring format_relative_time(
    application::WallClockTime checked_at,
    application::WallClockTime now);

}  // namespace azzs::ui::presentation
