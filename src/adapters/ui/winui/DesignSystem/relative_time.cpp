#include "relative_time.hpp"

#include <chrono>
#include <ctime>

namespace azzs::ui::presentation {
namespace {

[[nodiscard]] bool local_calendar_time(std::time_t value, std::tm& result) {
#if defined(_WIN32)
  return ::localtime_s(&result, &value) == 0;
#else
  return ::localtime_r(&value, &result) != nullptr;
#endif
}

[[nodiscard]] std::wstring calendar_time_text(application::WallClockTime value) {
  auto const time = std::chrono::system_clock::to_time_t(value);
  std::tm local{};
  if (!local_calendar_time(time, local)) {
    return L"日期不可用";
  }
  return std::to_wstring(local.tm_mon + 1) + L"月" +
         std::to_wstring(local.tm_mday) + L"日" +
         std::to_wstring(local.tm_hour) + L"时";
}

}  // namespace

std::wstring format_relative_time(application::WallClockTime checked_at,
                                  application::WallClockTime now) {
  using namespace std::chrono;

  auto const elapsed = now - checked_at;
  if (elapsed < minutes{1}) {
    return L"刚刚";
  }
  if (elapsed < hours{1}) {
    return std::to_wstring(duration_cast<minutes>(elapsed).count()) + L"分钟前";
  }
  if (elapsed < hours{24}) {
    return std::to_wstring(duration_cast<hours>(elapsed).count()) + L"小时前";
  }
  return calendar_time_text(checked_at);
}

}  // namespace azzs::ui::presentation
