#include "relative_time.hpp"

#include <chrono>
#include <ctime>
#include <string_view>

namespace azzs::ui::presentation {
namespace {

void replace_token(std::wstring& value, std::wstring_view token,
                   std::wstring const& replacement) {
  auto const position = value.find(token);
  if (position != std::wstring::npos) {
    value.replace(position, token.size(), replacement);
  }
}

[[nodiscard]] bool local_calendar_time(std::time_t value, std::tm& result) {
#if defined(_WIN32)
  return ::localtime_s(&result, &value) == 0;
#else
  return ::localtime_r(&value, &result) != nullptr;
#endif
}

[[nodiscard]] std::wstring calendar_time_text(
    application::WallClockTime value, RelativeTimeLabels const& labels) {
  auto const time = std::chrono::system_clock::to_time_t(value);
  std::tm local{};
  if (!local_calendar_time(time, local)) {
    return labels.unavailable;
  }
  auto result = labels.calendar;
  replace_token(result, L"{month}", std::to_wstring(local.tm_mon + 1));
  replace_token(result, L"{day}", std::to_wstring(local.tm_mday));
  replace_token(result, L"{hour}", std::to_wstring(local.tm_hour));
  return result;
}

}  // namespace

std::wstring format_relative_time(application::WallClockTime checked_at,
                                  application::WallClockTime now,
                                  RelativeTimeLabels const& labels) {
  using namespace std::chrono;

  auto const elapsed = now - checked_at;
  if (elapsed < minutes{1}) {
    return labels.just_now;
  }
  if (elapsed < hours{1}) {
    auto result = labels.minutes;
    replace_token(result, L"{count}",
                  std::to_wstring(duration_cast<minutes>(elapsed).count()));
    return result;
  }
  if (elapsed < hours{24}) {
    auto result = labels.hours;
    replace_token(result, L"{count}",
                  std::to_wstring(duration_cast<hours>(elapsed).count()));
    return result;
  }
  return calendar_time_text(checked_at, labels);
}

}  // namespace azzs::ui::presentation
