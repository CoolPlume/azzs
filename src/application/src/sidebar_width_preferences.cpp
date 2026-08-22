#include "azzs/application/sidebar_width_preferences.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace azzs::application {

SidebarWidthPreferences::SidebarWidthPreferences(
    std::shared_ptr<SidebarWidthPreferenceStore> store)
    : store_(std::move(store)) {
  if (!store_) {
    return;
  }

  try {
    auto const read = store_->read_sidebar_width();
    if (read.status == SidebarWidthPreferenceReadStatus::loaded &&
        std::isfinite(read.width_dip) &&
        read.width_dip >= kSidebarWidthMinimumDip &&
        read.width_dip <= kSidebarWidthMaximumDip) {
      width_dip_ = read.width_dip;
    }
  } catch (...) {
    // A display preference must never make workbench startup fail.
    width_dip_ = kSidebarWidthDefaultDip;
  }
}

double SidebarWidthPreferences::clamp(double width_dip) noexcept {
  if (!std::isfinite(width_dip)) {
    return kSidebarWidthDefaultDip;
  }
  return std::clamp(width_dip, kSidebarWidthMinimumDip,
                    kSidebarWidthMaximumDip);
}

double SidebarWidthPreferences::width_dip() const noexcept { return width_dip_; }

double SidebarWidthPreferences::set_width_dip(double width_dip) noexcept {
  auto const candidate = clamp(width_dip);
  if (!store_) {
    width_dip_ = kSidebarWidthDefaultDip;
    return width_dip_;
  }

  try {
    if (store_->write_sidebar_width(candidate) ==
        SidebarWidthPreferenceWriteStatus::saved) {
      width_dip_ = candidate;
      return width_dip_;
    }
  } catch (...) {
    // Fall through to the safe display default.
  }

  width_dip_ = kSidebarWidthDefaultDip;
  return width_dip_;
}

}  // namespace azzs::application
