#pragma once

#include <string>
#include <string_view>

#include "azzs/application/software_selection.hpp"

namespace azzs::adapters::windows {

// Opens only a catalog-declared HTTP(S) address through the system shell. This
// adapter cannot accept an arbitrary URL from a UI command and does not fetch,
// download, execute, or inspect the target.
class WindowsExternalAddressLauncher final
    : public application::software_selection::ExternalAddressLauncher {
 public:
  [[nodiscard]] bool open_declared_address(
      std::string_view software_id,
      domain::software_catalog::CatalogSource const& declared_source,
      std::string& error) override;
};

}  // namespace azzs::adapters::windows
