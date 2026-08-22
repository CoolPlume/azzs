#pragma once

#include "app_resource_ids.h"

#include <windows.h>

#include <string>

#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/base.h>

namespace azzs::ui::winui {

[[nodiscard]] inline std::wstring compiled_resource_string(UINT resource_id) noexcept {
  try {
    auto const module = ::GetModuleHandleW(nullptr);
    if (module == nullptr) {
      return {};
    }

    std::wstring value(256, L'\0');
    auto const length = ::LoadStringW(module, resource_id, value.data(),
                                      static_cast<int>(value.size()));
    if (length <= 0) {
      return {};
    }
    value.resize(static_cast<std::size_t>(length));
    return value;
  } catch (...) {
    return {};
  }
}

[[nodiscard]] inline winrt::hstring localized_resource_or_compiled_fallback(
    wchar_t const* resource_key, UINT fallback_resource_id) noexcept {
  try {
    try {
      auto const value =
          winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceLoader{}
              .GetString(resource_key);
      if (!value.empty()) {
        return value;
      }
    } catch (...) {
      // The compiled string table below remains available before WinUI has
      // finished bootstrapping or when a PRI key is missing.
    }

    auto const fallback = compiled_resource_string(fallback_resource_id);
    return fallback.empty() ? winrt::hstring{} : winrt::hstring{fallback};
  } catch (...) {
    return {};
  }
}

}  // namespace azzs::ui::winui
