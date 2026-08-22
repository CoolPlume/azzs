#pragma once

#include "../presentation_contract.hpp"

namespace azzs::ui::presentation {

[[nodiscard]] std::shared_ptr<PresentationSnapshot const>
make_design_system_fixture();

}  // namespace azzs::ui::presentation
