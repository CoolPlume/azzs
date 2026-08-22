#pragma once

#include <memory>

namespace azzs::application {

enum class AdvancedViewPreferenceReadStatus {
  loaded,
  unavailable,
};

struct AdvancedViewPreferenceRead final {
  AdvancedViewPreferenceReadStatus status{
      AdvancedViewPreferenceReadStatus::unavailable};
  bool enabled{false};
};

enum class AdvancedViewPreferenceWriteStatus {
  saved,
  unavailable,
};

class AdvancedViewPreferenceStore {
 public:
  virtual ~AdvancedViewPreferenceStore() = default;

  [[nodiscard]] virtual AdvancedViewPreferenceRead read_advanced_view() = 0;
  [[nodiscard]] virtual AdvancedViewPreferenceWriteStatus write_advanced_view(
      bool enabled) = 0;
};

class AdvancedViewPreferences final {
 public:
  explicit AdvancedViewPreferences(
      std::shared_ptr<AdvancedViewPreferenceStore> store);

  [[nodiscard]] bool enabled() const noexcept;
  [[nodiscard]] bool set_enabled(bool enabled);

 private:
  std::shared_ptr<AdvancedViewPreferenceStore> store_;
  bool enabled_{false};
};

}  // namespace azzs::application
