#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "azzs/adapters/windows/windows_controlled_acquisition.hpp"

namespace {

using azzs::adapters::windows::WindowsPresenceRegistryEntry;
using azzs::adapters::windows::WindowsPresenceRegistryHive;
using azzs::adapters::windows::WindowsPresenceRegistryQuery;
using azzs::adapters::windows::WindowsPresenceRegistryRead;
using azzs::adapters::windows::WindowsPresenceRegistryReadStatus;
using azzs::adapters::windows::WindowsPresenceRegistryView;
using azzs::adapters::windows::WindowsControlledPackageDownloader;
using azzs::adapters::windows::WindowsRegistrySoftwarePresenceDetector;
using azzs::domain::offline_package_cache::CacheArchitecture;
using azzs::domain::offline_package_cache::CacheAssetIdentity;
using azzs::domain::offline_package_cache::CacheLocationKind;
using azzs::domain::offline_package_cache::ControlledCacheRoot;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows software presence detector contract failed: "
              << message << '\n';
  }
  return condition;
}

class FakeRegistryQuery final : public WindowsPresenceRegistryQuery {
 public:
  using Root = std::pair<WindowsPresenceRegistryHive,
                         WindowsPresenceRegistryView>;

  [[nodiscard]] WindowsPresenceRegistryRead read(
      WindowsPresenceRegistryHive hive,
      WindowsPresenceRegistryView view) const override {
    calls.push_back({hive, view});
    if (throw_on_read) {
      throw std::runtime_error{"test registry access exception"};
    }
    auto const index = calls.size() - 1U;
    if (index >= responses.size()) {
      return {.detail = "fake registry response is missing"};
    }
    return responses[index];
  }

  mutable std::vector<Root> calls;
  std::array<WindowsPresenceRegistryRead, 4> responses{{
      {.status = WindowsPresenceRegistryReadStatus::root_absent},
      {.status = WindowsPresenceRegistryReadStatus::root_absent},
      {.status = WindowsPresenceRegistryReadStatus::root_absent},
      {.status = WindowsPresenceRegistryReadStatus::root_absent},
  }};
  bool throw_on_read{false};
};

[[nodiscard]] bool all_fixed_roots_are_read() {
  auto query = std::make_unique<FakeRegistryQuery>();
  auto* query_ptr = query.get();
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const result = detector.detect("qq");
  return expect(result.completed && !result.present,
                "an empty set of fixed roots is a completed absent result") &&
         expect(query_ptr->calls.size() == 4,
                "a known stable ID must read all four fixed roots") &&
         expect(query_ptr->calls[0] ==
                    FakeRegistryQuery::Root{
                        WindowsPresenceRegistryHive::current_user,
                        WindowsPresenceRegistryView::view_32},
                "the first root must be HKCU 32-bit") &&
         expect(query_ptr->calls[1] ==
                    FakeRegistryQuery::Root{
                        WindowsPresenceRegistryHive::current_user,
                        WindowsPresenceRegistryView::view_64},
                "the second root must be HKCU 64-bit") &&
         expect(query_ptr->calls[2] ==
                    FakeRegistryQuery::Root{
                        WindowsPresenceRegistryHive::local_machine,
                        WindowsPresenceRegistryView::view_32},
                "the third root must be HKLM 32-bit") &&
         expect(query_ptr->calls[3] ==
                    FakeRegistryQuery::Root{
                        WindowsPresenceRegistryHive::local_machine,
                        WindowsPresenceRegistryView::view_64},
                "the fourth root must be HKLM 64-bit");
}

[[nodiscard]] bool fixed_display_name_is_detected() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[2] = {
      .status = WindowsPresenceRegistryReadStatus::succeeded,
      .entries = {{.display_name = L"QQ"}},
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const result = detector.detect("qq");
  return expect(result.completed && result.present,
                "a fixed project-owned display name must report present");
}

[[nodiscard]] bool unknown_display_name_is_absent() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[0] = {
      .status = WindowsPresenceRegistryReadStatus::succeeded,
      .entries = {{.display_name = L"QQ Preview"}},
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const result = detector.detect("qq");
  return expect(result.completed && !result.present,
                "an unrelated display name must not match by substring");
}

[[nodiscard]] bool display_version_must_match_baseline() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[2] = {
      .status = WindowsPresenceRegistryReadStatus::succeeded,
      .entries = {{.display_name = L"QQ", .display_version = L"9.9.33"}},
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const matching = detector.detect_version("qq", "9.9.33");
  return expect(matching.completed && matching.present,
                "a fixed display name with the fixed baseline version must match");
}

[[nodiscard]] bool display_version_mismatch_is_not_present() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[2] = {
      .status = WindowsPresenceRegistryReadStatus::succeeded,
      .entries = {{.display_name = L"QQ", .display_version = L"3.2.31"}},
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const mismatched = detector.detect_version("qq", "9.9.33");
  return expect(mismatched.completed && !mismatched.present,
                "a same-name uninstall entry with a different version must not match");
}

[[nodiscard]] bool display_version_match_is_case_sensitive() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[2] = {
      .status = WindowsPresenceRegistryReadStatus::succeeded,
      .entries = {{.display_name = L"QQ", .display_version = L"9.9.33B"}},
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const mismatched = detector.detect_version("qq", "9.9.33b");
  return expect(mismatched.completed && !mismatched.present,
                "display version matching must not ignore case");
}

[[nodiscard]] bool missing_display_version_is_not_present() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[2] = {
      .status = WindowsPresenceRegistryReadStatus::succeeded,
      .entries = {{.display_name = L"QQ"}},
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const missing = detector.detect_version("qq", "9.9.33");
  return expect(missing.completed && !missing.present,
                "a same-name uninstall entry without DisplayVersion must not match");
}

[[nodiscard]] bool unknown_id_is_not_queried() {
  auto query = std::make_unique<FakeRegistryQuery>();
  auto* query_ptr = query.get();
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const result = detector.detect("user-supplied-registry-query");
  return expect(!result.completed && !result.present,
                "an unknown stable ID must fail closed") &&
         expect(query_ptr->calls.empty(),
                "an unknown stable ID must not touch the registry");
}

[[nodiscard]] bool registry_failure_is_incomplete() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[1] = {
      .status = WindowsPresenceRegistryReadStatus::failed,
      .detail = "access denied",
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const result = detector.detect("qq");
  return expect(!result.completed && !result.present,
                "a registry access failure must not become absent");
}

[[nodiscard]] bool malformed_registry_value_is_incomplete() {
  auto query = std::make_unique<FakeRegistryQuery>();
  query->responses[0] = {
      .status = WindowsPresenceRegistryReadStatus::failed,
      .detail = "DisplayName has an invalid type",
  };
  WindowsRegistrySoftwarePresenceDetector detector{std::move(query)};
  auto const result = detector.detect("sogou-input");
  return expect(!result.completed,
                "a malformed DisplayName value must fail closed");
}

[[nodiscard]] bool null_query_is_incomplete() {
  WindowsRegistrySoftwarePresenceDetector detector{
      std::unique_ptr<WindowsPresenceRegistryQuery>{}};
  auto const result = detector.detect("qq");
  return expect(!result.completed,
                "a missing registry query implementation must fail closed");
}

[[nodiscard]] bool downloader_registration_requires_fixed_installer_asset() {
  WindowsControlledPackageDownloader downloader{
      ControlledCacheRoot{.kind = CacheLocationKind::system_directory,
                           .id = "contract-root"}};
  CacheAssetIdentity const identity{
      .software_id = "qq",
      .version = "9.9.33",
      .architecture = CacheArchitecture::x64,
      .source_identity = "contract-source"};
  return expect(
             !downloader.register_source(identity,
                                         "https://example.test/download.php?type=x64"),
             "a redirect or dynamic download endpoint must not be registered") &&
         expect(!downloader.register_source(identity,
                                            "https://example.test/release.html"),
                "an HTML release page must not be registered") &&
         expect(downloader.register_source(identity,
                                           "https://example.test/installer.exe"),
                "a fixed executable asset must be registerable") &&
         expect(downloader.register_source(identity,
                                           "https://example.test/installer.exe"),
                "re-registering the exact fixed asset must be idempotent") &&
         expect(!downloader.register_source(identity,
                                            "https://example.test/other.exe"),
                "a duplicate identity must not change its registered asset");
}

}  // namespace

int main() {
  return all_fixed_roots_are_read() && fixed_display_name_is_detected() &&
                 unknown_display_name_is_absent() &&
                 display_version_must_match_baseline() &&
                 display_version_mismatch_is_not_present() &&
                 display_version_match_is_case_sensitive() &&
                 missing_display_version_is_not_present() &&
                 unknown_id_is_not_queried() &&
                  registry_failure_is_incomplete() &&
                  malformed_registry_value_is_incomplete() && null_query_is_incomplete()
                  && downloader_registration_requires_fixed_installer_asset()
              ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
