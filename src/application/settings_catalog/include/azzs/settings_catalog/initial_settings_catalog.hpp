#pragma once

#include "azzs/settings_catalog/settings_catalog.hpp"

namespace azzs::application::settings_catalog {

// The first release catalog is data-shaped and validated by the same lifecycle
// as an updated catalog. It contains no executable or free-form platform data.
[[nodiscard]] domain::settings_catalog::SettingsCatalog
initial_settings_catalog();

}  // namespace azzs::application::settings_catalog
