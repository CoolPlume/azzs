#pragma once

#include <windows.h>

#include <string>

#include "resource_ids.h"

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/base.h>

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

// ResourceLoader is not guaranteed to be usable while WinUI is bootstrapping
// or when a PRI key is absent. Keep the lookup boundary no-throw and fall back
// to the executable string table without moving the fallback value.
[[nodiscard]] inline winrt::hstring localized_or_native_string(
    wchar_t const* resource_key, unsigned int fallback_id) noexcept {
  try {
    try {
      auto const value =
          winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader{}
              .GetString(resource_key);
      if (!value.empty()) {
        return value;
      }
    } catch (...) {
      // The compiled string table remains available before PRI resources load.
    }

    auto const fallback = load_string(fallback_id);
    return fallback.empty() ? winrt::hstring{} : winrt::hstring{fallback};
  } catch (...) {
    return {};
  }
}

}  // namespace azzs::ui::winui::native_resources
