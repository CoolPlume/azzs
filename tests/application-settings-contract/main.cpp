#include <cstdlib>
#include <iostream>
#include <memory>

#include "azzs/application/application_settings.hpp"

namespace {

using azzs::application::ApplicationSettingsConfirmationAction;
using azzs::application::ArchitecturePreferenceRead;
using azzs::application::ArchitecturePreferenceReadStatus;
using azzs::application::ArchitecturePreferenceStore;
using azzs::application::ArchitecturePreferenceWriteStatus;
using azzs::application::ArchitecturePreferences;
using azzs::application::CacheRetentionPreferenceRead;
using azzs::application::CacheRetentionPreferenceReadStatus;
using azzs::application::CacheRetentionPreferenceStore;
using azzs::application::CacheRetentionPreferenceWriteStatus;
using azzs::application::CacheRetentionPreferences;
using azzs::application::requires_explicit_confirmation;
using azzs::domain::architecture_selection::ArchitecturePreference;
using azzs::domain::offline_package_cache::CacheRetentionPolicy;

class InMemoryArchitecturePreferenceStore final
    : public ArchitecturePreferenceStore {
 public:
  [[nodiscard]] ArchitecturePreferenceRead read_architecture_preference()
      override {
    return read;
  }

  [[nodiscard]] ArchitecturePreferenceWriteStatus write_architecture_preference(
      ArchitecturePreference preference) override {
    last_written = preference;
    return write_status;
  }

  ArchitecturePreferenceRead read{
      .status = ArchitecturePreferenceReadStatus::loaded};
  ArchitecturePreferenceWriteStatus write_status{
      ArchitecturePreferenceWriteStatus::saved};
  ArchitecturePreference last_written{
      ArchitecturePreference::prefer_arm64_prompt_fallback};
};

class InMemoryCacheRetentionPreferenceStore final
    : public CacheRetentionPreferenceStore {
 public:
  [[nodiscard]] CacheRetentionPreferenceRead read_cache_retention() override {
    return read;
  }

  [[nodiscard]] CacheRetentionPreferenceWriteStatus write_cache_retention(
      CacheRetentionPolicy retention) override {
    last_written = retention;
    return write_status;
  }

  CacheRetentionPreferenceRead read{
      .status = CacheRetentionPreferenceReadStatus::loaded};
  CacheRetentionPreferenceWriteStatus write_status{
      CacheRetentionPreferenceWriteStatus::saved};
  CacheRetentionPolicy last_written{CacheRetentionPolicy::retain_seven_days};
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "application settings contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] bool verify_architecture_preferences_are_fail_closed() {
  bool passed = true;
  auto store = std::make_shared<InMemoryArchitecturePreferenceStore>();
  store->read.preference = ArchitecturePreference::prefer_x64;
  ArchitecturePreferences preferences{store};
  passed &= expect(preferences.preference() == ArchitecturePreference::prefer_x64,
                   "a loaded architecture preference must initialize the owner");

  auto const saved = preferences.set_preference(
      ArchitecturePreference::prefer_arm64_auto_fallback);
  passed &= expect(saved == ArchitecturePreference::prefer_arm64_auto_fallback &&
                       store->last_written ==
                           ArchitecturePreference::prefer_arm64_auto_fallback,
                   "a successful architecture preference write must update the owner");

  store->write_status = ArchitecturePreferenceWriteStatus::unavailable;
  auto const retained = preferences.set_preference(
      ArchitecturePreference::prefer_arm64_prompt_fallback);
  passed &= expect(retained == ArchitecturePreference::prefer_arm64_auto_fallback &&
                       preferences.preference() ==
                           ArchitecturePreference::prefer_arm64_auto_fallback,
                   "a failed architecture preference write must retain the prior value");

  auto unavailable = std::make_shared<InMemoryArchitecturePreferenceStore>();
  unavailable->read.status = ArchitecturePreferenceReadStatus::unavailable;
  unavailable->read.preference = ArchitecturePreference::prefer_x64;
  ArchitecturePreferences fallback{unavailable};
  passed &= expect(
      fallback.preference() == ArchitecturePreference::prefer_arm64_prompt_fallback,
      "an unavailable architecture preference read must keep the safe default");
  return passed;
}

[[nodiscard]] bool verify_cache_preferences_are_fail_closed() {
  bool passed = true;
  auto store = std::make_shared<InMemoryCacheRetentionPreferenceStore>();
  store->read.retention = CacheRetentionPolicy::retain_thirty_days;
  CacheRetentionPreferences preferences{store};
  passed &= expect(preferences.retention() == CacheRetentionPolicy::retain_thirty_days,
                   "a loaded cache retention preference must initialize the owner");

  auto const saved =
      preferences.set_retention(CacheRetentionPolicy::retain_indefinitely);
  passed &= expect(saved == CacheRetentionPolicy::retain_indefinitely &&
                       store->last_written == CacheRetentionPolicy::retain_indefinitely,
                   "a successful cache retention write must update the owner");

  store->write_status = CacheRetentionPreferenceWriteStatus::unavailable;
  auto const retained =
      preferences.set_retention(CacheRetentionPolicy::delete_immediately);
  passed &= expect(retained == CacheRetentionPolicy::retain_indefinitely &&
                       preferences.retention() ==
                           CacheRetentionPolicy::retain_indefinitely,
                   "a failed cache retention write must retain the prior value");

  auto unavailable = std::make_shared<InMemoryCacheRetentionPreferenceStore>();
  unavailable->read.status = CacheRetentionPreferenceReadStatus::unavailable;
  unavailable->read.retention = CacheRetentionPolicy::delete_immediately;
  CacheRetentionPreferences fallback{unavailable};
  passed &= expect(fallback.retention() == CacheRetentionPolicy::retain_seven_days,
                   "an unavailable cache retention read must keep the safe default");
  return passed;
}

[[nodiscard]] bool verify_dangerous_operations_require_confirmation() {
  constexpr ApplicationSettingsConfirmationAction actions[]{
      ApplicationSettingsConfirmationAction::clear_cache,
      ApplicationSettingsConfirmationAction::clear_logs,
      ApplicationSettingsConfirmationAction::delete_recovery_record,
      ApplicationSettingsConfirmationAction::catalog_change,
  };
  bool passed = true;
  for (auto const action : actions) {
    passed &= expect(requires_explicit_confirmation(action),
                     "a destructive settings operation must require confirmation");
  }
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_architecture_preferences_are_fail_closed();
  passed &= verify_cache_preferences_are_fail_closed();
  passed &= verify_dangerous_operations_require_confirmation();
  if (!passed) {
    return EXIT_FAILURE;
  }

  std::cout << "application settings contract passed\n";
  return EXIT_SUCCESS;
}
