#pragma once

#include <memory>

#include "azzs/application/software_selection.hpp"
#include "presentation_contract.hpp"

namespace azzs::ui::presentation {

struct SoftwareSelectionPresentationText final {
  std::string accessible_name{"Software selection status"};
  std::string available_title{"Software selection"};
  std::string available_body_prefix{"Retained "};
  std::string available_body_suffix{
      " software selections. Catalog changes require explicit reconfirmation."};
  std::string absent_catalog_title{"No current effective catalog"};
  std::string absent_catalog_body{
      "No current effective catalog is loaded. Software selection, source "
      "resolution, and external handoff do not start automatically."};
  std::string not_restored_body{
      "Software selection state is not restored. Network and software "
      "detection do not start automatically."};
  std::string restore_failed_body{"Software selection state could not be restored."};
  std::string advanced_available{
      "Standard and advanced views share the same selection state."};
  std::string advanced_absent_catalog{
      "This page does not treat the built-in draft catalog as current, and "
      "does not start source resolution, a browser, or software detection."};
};

[[nodiscard]] std::shared_ptr<PresentationSnapshot const>
make_software_selection_presentation(
    application::software_selection::SoftwareSelectionSnapshot const& source,
    SoftwareSelectionPresentationText text = {});

}  // namespace azzs::ui::presentation
