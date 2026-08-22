#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/adapters/windows/windows_controlled_acquisition.hpp"

namespace {

using azzs::adapters::windows::WindowsRegisteredSourceResolver;
using azzs::adapters::windows::WindowsSourceResolutionRegistration;
using azzs::adapters::windows::WindowsControlledAddressIdentity;
using azzs::adapters::windows::initial_windows_source_registrations;
using azzs::adapters::windows::parse_windows_controlled_https_address;
using azzs::adapters::windows::windows_controlled_content_range_matches;
using azzs::adapters::windows::
    windows_controlled_github_release_redirect_matches;
using azzs::adapters::windows::windows_controlled_redirect_matches;
using azzs::domain::software_catalog::CatalogSource;
using azzs::domain::software_catalog::SourcePurpose;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows source resolution contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] CatalogSource declared(std::string address) {
  return {.purpose = SourcePurpose::primary, .address = std::move(address)};
}

[[nodiscard]] bool resolves_fixed_assets() {
  auto registrations = initial_windows_source_registrations();
  WindowsRegisteredSourceResolver resolver{registrations};
  struct Case final {
    std::string_view id;
    std::string_view address;
    std::string_view expected_asset_suffix;
  };
  constexpr Case cases[] = {
      {"sogou-input", "https://shurufa.sogou.com/windows", ".exe"},
      {"game-cheats-manager",
       "https://github.com/dyang886/Game-Cheats-Manager/releases", ".exe"},
      {"internet-download-manager",
       "https://www.internetdownloadmanager.com/download.html", ".exe"},
      {"java-runtime", "https://www.oracle.com/java/technologies/downloads/", ".msi"},
      {"powershell-7", "https://github.com/PowerShell/PowerShell/releases", ".msi"},
      {"qq", "https://im.qq.com/pcqq", ".exe"},
      {"dotnet-runtime", "https://dotnet.microsoft.com/download/dotnet", ".exe"},
      {"directx-runtime",
       "https://www.microsoft.com/en-us/download/details.aspx?id=35", ".exe"},
  };

  bool passed = true;
  for (auto const& test : cases) {
    auto const result = resolver.resolve(test.id, declared(std::string{test.address}));
    passed &= expect(result.resolved && result.snapshot.has_value(),
                     "a reviewed fixed installer asset must resolve");
    if (result.snapshot.has_value()) {
      passed &= expect(result.snapshot->actual_address.ends_with(test.expected_asset_suffix),
                       "a resolved source must expose an installer asset suffix");
      passed &= expect(result.snapshot->declared_address == test.address,
                       "a resolved source must retain the exact declared address");
    }
  }
  return passed;
}

[[nodiscard]] bool production_registrations_are_frozen() {
  auto const registrations = initial_windows_source_registrations();
  constexpr std::string_view unavailable_ids[] = {
      "cheat-engine", "office-tool-plus", "the-geometers-sketchpad",
  };
  bool passed = true;
  for (auto const& registration : registrations) {
    passed &= expect(!registration.actual_address.empty(),
                     "every production source registration must bind an installer asset");
  }
  for (auto const id : unavailable_ids) {
    passed &= expect(
        std::ranges::none_of(registrations,
                             [&](WindowsSourceResolutionRegistration const& registration) {
                               return registration.software_id == id;
                             }),
        "a source without a frozen installer asset must be absent from production registration");
  }
  return passed;
}

[[nodiscard]] bool fail_closed_entries_are_not_resolved() {
  auto const registrations = initial_windows_source_registrations();
  WindowsRegisteredSourceResolver resolver{registrations};
  constexpr std::pair<std::string_view, std::string_view> cases[] = {
      {"cheat-engine", "https://www.cheatengine.org/downloads.php"},
      {"office-tool-plus", "https://otp.landian.vip/en-us/download.html"},
      {"the-geometers-sketchpad",
       "https://www.dynamicgeometry.com/General_Resources/Sketchpad_About.html"},
  };

  bool passed = true;
  for (auto const& test : cases) {
    auto const result = resolver.resolve(test.first, declared(std::string{test.second}));
    passed &= expect(!result.resolved && !result.snapshot.has_value(),
                     "a page or unreviewed source must fail closed");
  }
  return passed;
}

[[nodiscard]] bool malformed_declarations_are_rejected() {
  WindowsRegisteredSourceResolver resolver{initial_windows_source_registrations()};
  bool passed = true;
  passed &= expect(!resolver.resolve(
                              "sogou-input",
                              CatalogSource{.purpose = SourcePurpose::primary,
                                            .address = "http://shurufa.sogou.com/windows"})
                           .resolved,
                   "HTTP declarations must not enter the controlled resolver");
  passed &= expect(!resolver.resolve(
                              "sogou-input",
                              CatalogSource{.purpose = SourcePurpose::primary,
                                            .address = "https://shurufa.sogou.com/windows#asset"})
                           .resolved,
                   "fragment-bearing declarations must be rejected");
  passed &= expect(!resolver.resolve(
                              "sogou-input",
                              CatalogSource{.purpose = std::nullopt,
                                            .address = "https://shurufa.sogou.com/windows"})
                           .resolved,
                   "declarations without a source purpose must be rejected");
  return passed;
}

[[nodiscard]] bool custom_dynamic_registration_is_rejected() {
  WindowsSourceResolutionRegistration registration;
  registration.software_id = "fixture";
  registration.declared_address = "https://fixture.example.test/download";
  registration.version = "1.0";
  registration.actual_address = "https://fixture.example.test/download?channel=x64";
  registration.hosting_mechanism = "fixture";
  registration.branch = "stable";
  registration.capability_version = "fixture-v1";
  registration.packages.push_back({});
  WindowsRegisteredSourceResolver resolver{{registration}};
  auto const result = resolver.resolve(
      "fixture", declared("https://fixture.example.test/download"));
  bool passed = expect(!result.resolved && !result.snapshot.has_value(),
                       "dynamic or query-based installer addresses must be rejected");

  registration.actual_address =
      "https://fixture.example.test/installer-1.0.exe?channel=x64";
  WindowsRegisteredSourceResolver query_resolver{{std::move(registration)}};
  auto const query_result = query_resolver.resolve(
      "fixture", declared("https://fixture.example.test/download"));
  passed &= expect(!query_result.resolved && !query_result.snapshot.has_value(),
                   "query-bearing installer assets must be rejected as mutable identities");
  return passed;
}

[[nodiscard]] bool redirect_identity_is_exact() {
  auto const registered = parse_windows_controlled_https_address(
      "https://github.com/example/project/releases/download/v1/installer.exe?arch=x64");
  bool passed = expect(registered.has_value(),
                       "a registered HTTPS asset must expose a complete identity");
  if (!registered.has_value()) {
    return false;
  }
  auto const same = parse_windows_controlled_https_address(
      "https://GITHUB.com/example/project/releases/download/v1/installer.exe?arch=x64");
  passed &= expect(same.has_value() &&
                       windows_controlled_redirect_matches(*registered, *same),
                   "a redirect may retain the exact registered asset identity");
  auto const other_host = parse_windows_controlled_https_address(
      "https://objects.githubusercontent.com/example/project/releases/download/v1/installer.exe?arch=x64");
  auto const other_port = parse_windows_controlled_https_address(
      "https://github.com:8443/example/project/releases/download/v1/installer.exe?arch=x64");
  auto const other_path = parse_windows_controlled_https_address(
      "https://github.com/example/project/releases/download/v2/installer.exe?arch=x64");
  auto const other_query = parse_windows_controlled_https_address(
      "https://github.com/example/project/releases/download/v1/installer.exe?arch=arm64");
  passed &= expect(other_host.has_value() &&
                       !windows_controlled_redirect_matches(*registered, *other_host),
                   "a redirect to another host must not reuse the registered asset identity");
  passed &= expect(other_port.has_value() &&
                       !windows_controlled_redirect_matches(*registered, *other_port),
                   "a redirect with another port must not reuse the registered asset identity");
  passed &= expect(other_path.has_value() &&
                       !windows_controlled_redirect_matches(*registered, *other_path),
                   "a redirect to another path must not reuse the registered asset identity");
  passed &= expect(other_query.has_value() &&
                       !windows_controlled_redirect_matches(*registered, *other_query),
                   "a redirect with another query must not reuse the registered asset identity");
  passed &= expect(!parse_windows_controlled_https_address(
                       "https://github.com/example/project/releases/download/v1/installer.exe#asset")
                       .has_value(),
                   "fragment-bearing redirect targets must be rejected");

  auto const github_cdn = parse_windows_controlled_https_address(
      "https://release-assets.githubusercontent.com/"
      "github-production-release-asset/123456/7656308a-849c-411f-b2c5-5846ba13ac66"
      "?sp=r&response-content-disposition=attachment%3B+filename%3Dinstaller.exe"
      "&sig=opaque");
  passed &= expect(github_cdn.has_value() &&
                       windows_controlled_github_release_redirect_matches(
                           *registered, *github_cdn),
                   "a signed GitHub release CDN redirect may bind the registered asset filename");
  auto const github_cdn_wrong_filename = parse_windows_controlled_https_address(
      "https://release-assets.githubusercontent.com/"
      "github-production-release-asset/123456/7656308a-849c-411f-b2c5-5846ba13ac66"
      "?sp=r&response-content-disposition=attachment%3B+filename%3Dother.exe"
      "&sig=opaque");
  passed &= expect(github_cdn_wrong_filename.has_value() &&
                       !windows_controlled_github_release_redirect_matches(
                           *registered, *github_cdn_wrong_filename),
                   "a GitHub CDN redirect for another asset must fail closed");
  auto const github_cdn_wrong_path = parse_windows_controlled_https_address(
      "https://release-assets.githubusercontent.com/"
      "other-prefix/123456/7656308a-849c-411f-b2c5-5846ba13ac66"
      "?response-content-disposition=attachment%3B+filename%3Dinstaller.exe");
  passed &= expect(github_cdn_wrong_path.has_value() &&
                       !windows_controlled_github_release_redirect_matches(
                           *registered, *github_cdn_wrong_path),
                   "an unrecognized GitHub CDN path must fail closed");
  return passed;
}

[[nodiscard]] bool content_range_is_strict() {
  bool passed = true;
  passed &= expect(windows_controlled_content_range_matches(
                       "bytes 100-199/500", 100),
                   "a Content-Range beginning at the resume offset must pass");
  passed &= expect(windows_controlled_content_range_matches(
                       " bytes 100-199/*\t", 100),
                   "optional header whitespace must not change a valid range");
  passed &= expect(!windows_controlled_content_range_matches("", 100),
                   "a missing Content-Range must fail closed");
  passed &= expect(!windows_controlled_content_range_matches(
                       "bytes 100-199", 100),
                   "a malformed Content-Range must fail closed");
  passed &= expect(!windows_controlled_content_range_matches(
                       "bytes 99-199/500", 100),
                   "a Content-Range with the wrong start must fail closed");
  passed &= expect(!windows_controlled_content_range_matches(
                       "bytes 100-99/500", 100),
                   "a reversed Content-Range must fail closed");
  passed &= expect(!windows_controlled_content_range_matches(
                       "bytes 100-199/199", 100),
                   "a Content-Range whose total excludes its end must fail closed");
  return passed;
}

}  // namespace

int main() {
  return resolves_fixed_assets() && production_registrations_are_frozen() &&
                 fail_closed_entries_are_not_resolved() &&
                 malformed_declarations_are_rejected() &&
                 custom_dynamic_registration_is_rejected() &&
                 redirect_identity_is_exact() && content_range_is_strict()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
