#pragma once

#include <optional>
#include <string>

#include "azzs/application/device_state_store.hpp"
#include "azzs/settings_catalog/settings_catalog_lifecycle.hpp"

namespace azzs::adapters::infrastructure {

// Owns only the settings-catalog package and aggregate encodings. Durable
// publication remains delegated to DeviceStateStore; log and occupancy
// semantics remain application-owned issue-02 seams.
class SettingsCatalogFileAdapter final
    : public application::settings_catalog::SettingsCatalogStateStorage,
      public application::settings_catalog::SettingsCatalogImportSource {
 public:
  explicit SettingsCatalogFileAdapter(
      application::DeviceStateStore& states) noexcept;

  [[nodiscard]] application::settings_catalog::CatalogStorageRead read()
      override;
  [[nodiscard]] application::settings_catalog::CatalogStorageWrite write(
      std::optional<domain::RevisionToken> expected_revision,
      application::settings_catalog::SettingsCatalogState state) override;
  [[nodiscard]] application::settings_catalog::CatalogImportRead read_import(
      std::string const& path) override;

 private:
  application::DeviceStateStore& states_;
};

}  // namespace azzs::adapters::infrastructure
