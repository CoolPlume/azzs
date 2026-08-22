#include <cstdlib>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/adapters/windows/windows_controlled_acquisition.hpp"
#include "azzs/adapters/windows/windows_installation_batch_adapters.hpp"

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
using azzs::adapters::windows::inspect_windows_controlled_archive;
using azzs::adapters::windows::WindowsArchiveValidationCode;
using azzs::domain::offline_package_cache::CacheArchitecture;
using azzs::domain::architecture_selection::PackageArchitecture;
using azzs::domain::software_catalog::CatalogSource;
using azzs::domain::software_catalog::SourcePurpose;
using azzs::domain::software_selection::PackageType;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows source resolution contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] CatalogSource declared(std::string address) {
  return {.purpose = SourcePurpose::primary, .address = std::move(address)};
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(std::byte{static_cast<unsigned char>(value & 0xffU)});
  bytes.push_back(std::byte{static_cast<unsigned char>((value >> 8U) & 0xffU)});
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
  for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
    bytes.push_back(
        std::byte{static_cast<unsigned char>((value >> shift) & 0xffU)});
  }
}

[[nodiscard]] std::uint32_t fixture_crc32(
    std::span<std::byte const> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (auto byte : bytes) {
    crc ^= std::to_integer<unsigned char>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

struct FixtureZipEntry final {
  std::string name;
  std::vector<std::byte> bytes;
  std::uint16_t method{};
  std::uint32_t crc_override{};
  bool override_crc{};
  std::vector<std::byte> local_extra;
  std::vector<std::byte> central_extra;
};

[[nodiscard]] std::vector<std::byte> fixture_x64_pe() {
  std::vector<std::byte> bytes(512U, std::byte{0});
  bytes[0] = std::byte{'M'};
  bytes[1] = std::byte{'Z'};
  bytes[0x3cU] = std::byte{0x80};
  bytes[0x80U] = std::byte{'P'};
  bytes[0x81U] = std::byte{'E'};
  bytes[0x84U] = std::byte{0x64};
  bytes[0x85U] = std::byte{0x86};
  return bytes;
}

[[nodiscard]] std::vector<std::byte> fixture_stored_zip(
    std::span<FixtureZipEntry const> entries) {
  std::vector<std::byte> archive;
  std::vector<std::uint32_t> local_offsets;
  local_offsets.reserve(entries.size());
  for (auto const& entry : entries) {
    local_offsets.push_back(static_cast<std::uint32_t>(archive.size()));
    auto const crc = entry.override_crc ? entry.crc_override
                                        : fixture_crc32(entry.bytes);
    append_u32(archive, 0x04034b50U);
    append_u16(archive, 20U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, entry.method);
    append_u16(archive, 0U);
    append_u32(archive, crc);
    append_u32(archive, static_cast<std::uint32_t>(entry.bytes.size()));
    append_u32(archive, static_cast<std::uint32_t>(entry.bytes.size()));
    append_u16(archive, static_cast<std::uint16_t>(entry.name.size()));
    append_u16(archive,
               static_cast<std::uint16_t>(entry.local_extra.size()));
    for (auto character : entry.name) {
      archive.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    archive.insert(archive.end(), entry.local_extra.begin(),
                   entry.local_extra.end());
    archive.insert(archive.end(), entry.bytes.begin(), entry.bytes.end());
  }

  auto const central_offset = static_cast<std::uint32_t>(archive.size());
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    auto const& entry = entries[index];
    auto const crc = entry.override_crc ? entry.crc_override
                                        : fixture_crc32(entry.bytes);
    append_u32(archive, 0x02014b50U);
    append_u16(archive, 20U);
    append_u16(archive, 20U);
    append_u16(archive, 0U);
    append_u16(archive, entry.method);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, crc);
    append_u32(archive, static_cast<std::uint32_t>(entry.bytes.size()));
    append_u32(archive, static_cast<std::uint32_t>(entry.bytes.size()));
    append_u16(archive, static_cast<std::uint16_t>(entry.name.size()));
    append_u16(archive,
               static_cast<std::uint16_t>(entry.central_extra.size()));
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u16(archive, 0U);
    append_u32(archive, 0U);
    append_u32(archive, local_offsets[index]);
    for (auto character : entry.name) {
      archive.push_back(std::byte{static_cast<unsigned char>(character)});
    }
    archive.insert(archive.end(), entry.central_extra.begin(),
                   entry.central_extra.end());
  }
  auto const central_size = static_cast<std::uint32_t>(archive.size()) - central_offset;
  append_u32(archive, 0x06054b50U);
  append_u16(archive, 0U);
  append_u16(archive, 0U);
  append_u16(archive, static_cast<std::uint16_t>(entries.size()));
  append_u16(archive, static_cast<std::uint16_t>(entries.size()));
  append_u32(archive, central_size);
  append_u32(archive, central_offset);
  append_u16(archive, 0U);
  return archive;
}

[[nodiscard]] bool archive_member_policy_is_fail_closed() {
  const std::string allowed_name = "Office Tool/Office Tool Plus.exe";
  const std::string directory_name = "Office Tool/";
  const std::string trailing_directory_name = "Office Tool/bin/";
  const std::string extra_name = "Office Tool/README.txt";
  const std::vector<std::string> allowed{allowed_name};
  const std::vector<FixtureZipEntry> with_directory{
      {directory_name, {}},
      {allowed_name, fixture_x64_pe()},
      {trailing_directory_name, {}}};
  const auto directory_archive = fixture_stored_zip(with_directory);
  auto inspection = inspect_windows_controlled_archive(
      directory_archive, allowed, CacheArchitecture::x64);
  bool passed = expect(
      inspection.code == WindowsArchiveValidationCode::valid &&
          inspection.members.size() == 1U &&
          inspection.members.front().path == allowed_name,
      "directory markers before and after a reviewed file must advance the central cursor");
  passed &= expect(
      std::ranges::none_of(inspection.members, [](auto const& member) {
        return member.path.ends_with('/');
      }),
      "ZIP directory markers must not appear in the inspected file members");

  const std::vector<FixtureZipEntry> deflated_directory{
      {directory_name, {}, 8U}};
  inspection = inspect_windows_controlled_archive(
      fixture_stored_zip(deflated_directory), allowed, CacheArchitecture::x64);
  passed &= expect(
      inspection.code != WindowsArchiveValidationCode::valid,
      "a Deflate directory marker must fail closed");

  const std::vector<FixtureZipEntry> checksummed_directory{
      {directory_name, {}, 0U, 1U, true}};
  inspection = inspect_windows_controlled_archive(
      fixture_stored_zip(checksummed_directory), allowed,
      CacheArchitecture::x64);
  passed &= expect(
      inspection.code != WindowsArchiveValidationCode::valid,
      "a directory marker with a non-zero CRC must fail closed");

  const std::vector<FixtureZipEntry> sized_directory{
      {directory_name, {std::byte{'x'}}}};
  inspection = inspect_windows_controlled_archive(
      fixture_stored_zip(sized_directory), allowed, CacheArchitecture::x64);
  passed &= expect(
      inspection.code != WindowsArchiveValidationCode::valid,
      "a directory marker carrying file data must fail closed");

  const std::vector<FixtureZipEntry> with_extra{
      {allowed_name, fixture_x64_pe()}, {extra_name, {std::byte{'x'}}}};
  const auto extra_archive = fixture_stored_zip(with_extra);
  inspection = inspect_windows_controlled_archive(
      extra_archive, allowed, CacheArchitecture::x64);
  passed &= expect(
      inspection.code == WindowsArchiveValidationCode::member_mismatch,
      "an ordinary ZIP member outside the whitelist must fail closed");

  const std::vector<FixtureZipEntry> corrupted_extra{
      {allowed_name, fixture_x64_pe()},
      {extra_name, {std::byte{'x'}}, 0U, 1U, true}};
  inspection = inspect_windows_controlled_archive(
      fixture_stored_zip(corrupted_extra), allowed, CacheArchitecture::x64);
  passed &= expect(
      inspection.code != WindowsArchiveValidationCode::valid,
      "a non-whitelisted member with a damaged CRC must fail closed");

  const std::vector<FixtureZipEntry> corrupted_allowed{
      {allowed_name, fixture_x64_pe(), 0U, 1U, true}};
  inspection = inspect_windows_controlled_archive(
      fixture_stored_zip(corrupted_allowed), allowed, CacheArchitecture::x64);
  passed &= expect(
      inspection.code != WindowsArchiveValidationCode::valid,
      "a whitelisted member with a damaged CRC must fail closed");

  const std::vector<FixtureZipEntry> mismatched_extra{
      {allowed_name, fixture_x64_pe(), 0U, 0U, false,
       {std::byte{0x01}, std::byte{0x02}}, {}}};
  inspection = inspect_windows_controlled_archive(
      fixture_stored_zip(mismatched_extra), allowed, CacheArchitecture::x64);
  passed &= expect(
      inspection.code != WindowsArchiveValidationCode::valid,
      "local and central ZIP extra-field lengths must agree");

  const std::vector<std::string> directory_whitelist{directory_name};
  inspection = inspect_windows_controlled_archive(
      directory_archive, directory_whitelist, CacheArchitecture::x64);
  passed &= expect(
      inspection.code == WindowsArchiveValidationCode::member_mismatch,
      "a ZIP directory marker must not satisfy the executable whitelist");
  return passed;
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
      "cheat-engine", "the-geometers-sketchpad",
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

[[nodiscard]] bool resolves_office_archive_snapshot() {
  WindowsRegisteredSourceResolver resolver{initial_windows_source_registrations()};
  constexpr std::string_view declared_address =
      "https://otp.landian.vip/en-us/download.html";
  constexpr std::string_view actual_address =
      "https://github.com/YerongAI/Office-Tool/releases/download/v11.6.6.0/Office_Tool_v11.6.6.0_x64.zip";
  constexpr std::string_view expected_sha256 =
      "43ba169e4d07c8e45ed4846d7171bfbc521e8f61efff366112b7c6ef9dae627b";
  auto const result = resolver.resolve(
      "office-tool-plus", declared(std::string{declared_address}));
  bool passed = expect(result.resolved && result.snapshot.has_value(),
                       "Office Tool Plus must resolve its reviewed ZIP asset");
  if (!result.snapshot.has_value()) {
    return false;
  }
  auto const& snapshot = *result.snapshot;
  passed &= expect(snapshot.actual_address == actual_address,
                   "Office snapshot must retain the exact fixed ZIP address");
  passed &= expect(snapshot.packages.size() == 1,
                   "Office snapshot must expose exactly one package");
  if (snapshot.packages.size() != 1) {
    return false;
  }
  auto const& package = snapshot.packages.front();
  passed &= expect(package.package_type == PackageType::archive_package,
                   "Office source must be represented as an archive package");
  passed &= expect(package.complete_package,
                   "Office archive package must be marked complete");
  passed &= expect(package.candidate.architecture == PackageArchitecture::x64,
                   "Office archive package must be pinned to x64");
  passed &= expect(package.expected_sha256.has_value() &&
                       *package.expected_sha256 == expected_sha256,
                   "Office archive SHA-256 must use the canonical lowercase digest");
  passed &= expect(package.archive_members.size() == 1 &&
                       package.archive_members.front() ==
                           "Office Tool/Office Tool Plus.exe",
                   "Office archive must expose its exact executable allowlist");
  return passed;
}

[[nodiscard]] bool fail_closed_entries_are_not_resolved() {
  auto const registrations = initial_windows_source_registrations();
  WindowsRegisteredSourceResolver resolver{registrations};
  constexpr std::pair<std::string_view, std::string_view> cases[] = {
      {"cheat-engine", "https://www.cheatengine.org/downloads.php"},
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
  return archive_member_policy_is_fail_closed() && resolves_fixed_assets() &&
                 production_registrations_are_frozen() &&
                 resolves_office_archive_snapshot() &&
                 fail_closed_entries_are_not_resolved() &&
                 malformed_declarations_are_rejected() &&
                 custom_dynamic_registration_is_rejected() &&
                 redirect_identity_is_exact() && content_range_is_strict()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
