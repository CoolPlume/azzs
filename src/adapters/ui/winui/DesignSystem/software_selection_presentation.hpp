#pragma once

#include <memory>

#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_selection.hpp"
#include "presentation_contract.hpp"

namespace azzs::ui::presentation {

struct SoftwareSelectionPresentationText final {
  std::string accessible_name{"软件选择状态"};
  std::string available_title{"软件选择"};
  std::string available_body_prefix{"已保留 "};
  std::string available_body_suffix{
      " 个软件选择。受目录变化影响的项目需要重新确认。"};
  std::string absent_catalog_title{"当前有效目录尚未加载"};
  std::string absent_catalog_body{
      "尚未加载当前有效目录。软件选择、来源解析和外部交接均不会自动开始。"};
  std::string not_restored_body{
      "软件选择状态尚未恢复；不会自动访问网络或检测软件。"};
  std::string restore_failed_body{"无法恢复软件选择状态。"};
  std::string advanced_available{
      "标准与高级视图共享相同的软件选择状态。"};
  std::string advanced_absent_catalog{
      "当前页面不会将内置草稿目录当作当前有效目录，也不会启动来源解析、浏览器或软件检测。"};
};

[[nodiscard]] std::shared_ptr<PresentationSnapshot const>
make_software_selection_presentation(
    application::software_selection::SoftwareSelectionSnapshot const& source,
    SoftwareSelectionPresentationText text = {});

struct OfflinePackageCachePresentationText final {
  std::string accessible_name{"离线资源缓存状态"};
  std::string available_title{"离线资源缓存"};
  std::string unavailable_title{"离线资源缓存不可用"};
  std::string available_body_prefix{"受控缓存位置： "};
  std::string unavailable_body_prefix{
      "受控缓存位置不可用： "};
  std::string item_suffix{" 个已缓存资源"};
  std::string network_suffix{"；当前无法联网"};
};

[[nodiscard]] std::shared_ptr<PresentationSnapshot const>
make_offline_package_cache_presentation(
    application::offline_package_cache::OfflinePackageCacheSnapshot const& source,
    OfflinePackageCachePresentationText text = {});

}  // namespace azzs::ui::presentation
