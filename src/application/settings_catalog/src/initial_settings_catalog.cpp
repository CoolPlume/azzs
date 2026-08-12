#include "azzs/settings_catalog/initial_settings_catalog.hpp"

#include <utility>

namespace azzs::application::settings_catalog {
namespace {

namespace catalog_domain = domain::settings_catalog;

constexpr catalog_domain::WindowsVersion kWindows11_21H2{
    .generation = catalog_domain::WindowsGeneration::windows_11,
    .feature_update_year = 21,
    .feature_update_half = 2};
constexpr catalog_domain::WindowsVersion kWindows11_25H2{
    .generation = catalog_domain::WindowsGeneration::windows_11,
    .feature_update_year = 25,
    .feature_update_half = 2};

[[nodiscard]] catalog_domain::SettingDefinition setting(
    std::string id, std::string display_name, std::string description,
    std::string source_url, std::string identity, std::string apply,
    std::string detect, std::string recover) {
  return {
      .id = catalog_domain::StableId{std::move(id)},
      .display_name = std::move(display_name),
      .description = std::move(description),
      .source_url = std::move(source_url),
      .known_windows_range = {.minimum = kWindows11_21H2,
                              .maximum = kWindows11_25H2},
      .default_selected = false,
      .risk = catalog_domain::SettingRiskLevel::elevated,
      .force_attempt_rule =
          catalog_domain::ForceAttemptRule::allowed_with_explicit_confirmation,
      .recovery_requirement =
          catalog_domain::RecoveryRequirement::restore_record_required,
      .restart_requirement = catalog_domain::RestartRequirement::explorer,
      .semantics = {.identity = std::move(identity),
                    .apply_capability = std::move(apply),
                    .detect_capability = std::move(detect),
                    .recover_capability = std::move(recover)}};
}

}  // namespace

catalog_domain::SettingsCatalog initial_settings_catalog() {
  return {
      .revision = 1,
      .settings = {
          setting(
              "setting.classic-context-menu", "Windows 11 经典右键菜单",
              "切换为经典右键菜单；任务栏和资源管理器窗口会在重启资源管理器时短暂关闭。",
              "https://www.zhihu.com/question/480356710",
              "windows11.classic-context-menu",
              "windows.classic-context-menu.apply",
              "windows.classic-context-menu.detect",
              "windows.classic-context-menu.restore"),
          setting(
              "setting.windows10-explorer", "Windows 10 风格资源管理器",
              "切换为 Windows 10 风格资源管理器；任务栏和资源管理器窗口会在重启资源管理器时短暂关闭。",
              "https://zhuanlan.zhihu.com/p/690092810",
              "windows11.windows10-explorer",
              "windows.windows10-explorer.apply",
              "windows.windows10-explorer.detect",
              "windows.windows10-explorer.restore"),
      },
      .plans = {
          {.id = catalog_domain::StableId{"plan.recommended"},
           .display_name = "推荐总体优化",
           .description = "仅组合首版系统设置项；执行、检测、验证和恢复仍按各单项记录。",
           .members = {
               {.setting_id =
                    catalog_domain::StableId{"setting.classic-context-menu"},
                .order = 10,
                .default_selected = false},
               {.setting_id =
                    catalog_domain::StableId{"setting.windows10-explorer"},
                .order = 20,
                .default_selected = false},
           }},
      },
  };
}

}  // namespace azzs::application::settings_catalog
