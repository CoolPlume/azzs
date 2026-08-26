#pragma once

#include <string>
#include <vector>

#include "azzs/application/hardware_overview.hpp"

namespace azzs::ui::presentation {

enum class HardwareInformationField {
  model,
  operating_system,
  cpu,
  motherboard,
  memory,
  gpu,
  display,
  solid_state_storage,
  hard_disk_storage,
  wired_network,
  wireless_network,
  audio,
  npu,
  keyboard,
  mouse,
  touchpad,
};

struct HardwareInformationPresentationText final {
  std::string unrecognized{"unrecognized"};
  std::string display_internal{"internal"};
  std::string display_external{"external"};
  std::string keyboard_key_count_suffix{" keys"};
};

struct HardwareInformationRow final {
  HardwareInformationField field{HardwareInformationField::model};
  std::string value;
  bool visible{true};
};

struct HardwareInformationPresentation final {
  bool unrecognized{false};
  std::vector<HardwareInformationRow> rows;
};

[[nodiscard]] HardwareInformationPresentation
make_hardware_information_presentation(
    application::HardwareOverviewSnapshot const& snapshot,
    HardwareInformationPresentationText text = {});

}  // namespace azzs::ui::presentation
