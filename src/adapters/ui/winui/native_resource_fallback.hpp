#pragma once

#include <windows.h>

#include <string>

#include "resource_ids.h"

namespace azzs::ui::winui::native_resources {

[[nodiscard]] inline std::wstring load_string(unsigned int id) noexcept {
  try {
    auto const module = ::GetModuleHandleW(nullptr);
    if (module == nullptr) {
      return {};
    }

    std::wstring value(256, L'\0');
    for (;;) {
      auto const length = ::LoadStringW(
          module, id, value.data(), static_cast<int>(value.size()));
      if (length <= 0) {
        return {};
      }
      if (length < static_cast<int>(value.size() - 1)) {
        value.resize(static_cast<std::size_t>(length));
        return value;
      }
      if (value.size() >= 32768) {
        return {};
      }
      value.resize(value.size() * 2);
    }
  } catch (...) {
    // Native fallback is used while handling startup/resource failures and
    // must never turn an allocation failure into process termination.
    return {};
  }
}

}  // namespace azzs::ui::winui::native_resources
