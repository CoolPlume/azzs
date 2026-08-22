#pragma once

#include <memory>

namespace azzs::application {

// Sidebar width is a display preference. It is deliberately kept outside the
// workbench snapshot so navigation presentation cannot become business state.
inline constexpr double kSidebarWidthMinimumDip = 216.0;
inline constexpr double kSidebarWidthDefaultDip = 248.0;
inline constexpr double kSidebarWidthMaximumDip = 360.0;

enum class SidebarWidthPreferenceReadStatus {
  loaded,
  unavailable,
};

struct SidebarWidthPreferenceRead final {
  SidebarWidthPreferenceReadStatus status{
      SidebarWidthPreferenceReadStatus::unavailable};
  double width_dip{kSidebarWidthDefaultDip};
};

enum class SidebarWidthPreferenceWriteStatus {
  saved,
  unavailable,
};

class SidebarWidthPreferenceStore {
 public:
  virtual ~SidebarWidthPreferenceStore() = default;

  [[nodiscard]] virtual SidebarWidthPreferenceRead read_sidebar_width() = 0;
  [[nodiscard]] virtual SidebarWidthPreferenceWriteStatus
  write_sidebar_width(double width_dip) = 0;
};

class SidebarWidthPreferences final {
 public:
  explicit SidebarWidthPreferences(
      std::shared_ptr<SidebarWidthPreferenceStore> store);

  [[nodiscard]] static double clamp(double width_dip) noexcept;
  [[nodiscard]] double width_dip() const noexcept;

  // A failed write resets the projection to the documented safe default. The
  // caller can therefore update the NavigationView without guessing whether
  // persistence succeeded.
  [[nodiscard]] double set_width_dip(double width_dip) noexcept;

 private:
  std::shared_ptr<SidebarWidthPreferenceStore> store_;
  double width_dip_{kSidebarWidthDefaultDip};
};

}  // namespace azzs::application
