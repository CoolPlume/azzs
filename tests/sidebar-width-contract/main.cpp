#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

#include "azzs/application/sidebar_width_preferences.hpp"

namespace {

using azzs::application::SidebarWidthPreferenceRead;
using azzs::application::SidebarWidthPreferenceReadStatus;
using azzs::application::SidebarWidthPreferenceStore;
using azzs::application::SidebarWidthPreferenceWriteStatus;
using azzs::application::SidebarWidthPreferences;

class InMemoryStore final : public SidebarWidthPreferenceStore {
 public:
  [[nodiscard]] SidebarWidthPreferenceRead read_sidebar_width() override {
    if (throw_on_read) {
      throw 1;
    }
    return read;
  }

  [[nodiscard]] SidebarWidthPreferenceWriteStatus write_sidebar_width(
      double width_dip) override {
    if (throw_on_write) {
      throw 1;
    }
    last_written = width_dip;
    return write_status;
  }

  SidebarWidthPreferenceRead read{
      .status = SidebarWidthPreferenceReadStatus::loaded};
  SidebarWidthPreferenceWriteStatus write_status{
      SidebarWidthPreferenceWriteStatus::saved};
  double last_written{-1.0};
  bool throw_on_read{false};
  bool throw_on_write{false};
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "sidebar width contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool verify_bounds_and_fallbacks() {
  bool passed = true;
  passed &= expect(SidebarWidthPreferences::clamp(100.0) ==
                       azzs::application::kSidebarWidthMinimumDip,
                   "values below the minimum must clamp");
  passed &= expect(SidebarWidthPreferences::clamp(500.0) ==
                       azzs::application::kSidebarWidthMaximumDip,
                   "values above the maximum must clamp");
  passed &= expect(SidebarWidthPreferences::clamp(
                       std::numeric_limits<double>::quiet_NaN()) ==
                       azzs::application::kSidebarWidthDefaultDip,
                   "non-finite values must use the default");

  auto invalid = std::make_shared<InMemoryStore>();
  invalid->read.width_dip = 400.0;
  SidebarWidthPreferences invalid_owner{invalid};
  passed &= expect(invalid_owner.width_dip() ==
                       azzs::application::kSidebarWidthDefaultDip,
                   "an out-of-range persisted value must use the default");

  auto unavailable = std::make_shared<InMemoryStore>();
  unavailable->read.status = SidebarWidthPreferenceReadStatus::unavailable;
  unavailable->read.width_dip = 216.0;
  SidebarWidthPreferences unavailable_owner{unavailable};
  passed &= expect(unavailable_owner.width_dip() ==
                       azzs::application::kSidebarWidthDefaultDip,
                   "an unavailable read must use the default");

  auto throwing = std::make_shared<InMemoryStore>();
  throwing->throw_on_read = true;
  SidebarWidthPreferences throwing_owner{throwing};
  passed &= expect(throwing_owner.width_dip() ==
                       azzs::application::kSidebarWidthDefaultDip,
                   "a read exception must not escape startup");
  return passed;
}

[[nodiscard]] bool verify_persistence_and_write_failure() {
  bool passed = true;
  auto store = std::make_shared<InMemoryStore>();
  store->read.width_dip = 300.0;
  SidebarWidthPreferences owner{store};
  passed &= expect(owner.width_dip() == 300.0,
                   "a valid persisted width must be restored");

  auto const saved = owner.set_width_dip(359.5);
  passed &= expect(saved == 359.5 && store->last_written == 359.5,
                   "a valid write must update the owner and store");

  store->write_status = SidebarWidthPreferenceWriteStatus::unavailable;
  auto const failed = owner.set_width_dip(216.0);
  passed &= expect(failed == azzs::application::kSidebarWidthDefaultDip &&
                       owner.width_dip() ==
                           azzs::application::kSidebarWidthDefaultDip,
                   "a failed write must reset the display projection");

  store->write_status = SidebarWidthPreferenceWriteStatus::saved;
  store->throw_on_write = true;
  auto const throwing = owner.set_width_dip(360.0);
  passed &= expect(throwing == azzs::application::kSidebarWidthDefaultDip,
                   "a write exception must be converted to a default");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_bounds_and_fallbacks();
  passed &= verify_persistence_and_write_failure();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "sidebar width contract passed\n";
  return EXIT_SUCCESS;
}
