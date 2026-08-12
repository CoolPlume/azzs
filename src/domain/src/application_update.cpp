#include "azzs/domain/application_update.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <system_error>

namespace azzs::domain::application_update {
namespace {

constexpr std::size_t kMaxTextBytes = 256;
constexpr std::size_t kMaxVersionParts = 8;
constexpr std::size_t kMaxAssets = 64;

[[nodiscard]] bool safe_text(std::string_view value,
                             std::size_t maximum = kMaxTextBytes) noexcept {
  return !value.empty() && value.size() <= maximum &&
         std::ranges::all_of(value, [](unsigned char byte) {
           return byte >= 0x20 && byte < 0x7f;
         });
}

struct ParsedVersion final {
  std::array<std::uint32_t, kMaxVersionParts> parts{};
  std::size_t size{};
};

[[nodiscard]] std::optional<ParsedVersion> parse_version(
    std::string_view value) noexcept {
  if (value.starts_with('v') || value.starts_with('V')) {
    value.remove_prefix(1);
  }
  if (value.empty()) {
    return std::nullopt;
  }
  ParsedVersion result;
  while (!value.empty()) {
    if (result.size == kMaxVersionParts) {
      return std::nullopt;
    }
    auto const dot = value.find('.');
    auto const part = value.substr(0, dot);
    if (part.empty() || !std::ranges::all_of(part, [](unsigned char byte) {
          return std::isdigit(byte) != 0;
        })) {
      return std::nullopt;
    }
    auto value_part = std::uint32_t{};
    auto const [end, error] =
        std::from_chars(part.data(), part.data() + part.size(), value_part);
    if (error != std::errc{} || end != part.data() + part.size()) {
      return std::nullopt;
    }
    result.parts[result.size++] = value_part;
    if (dot == std::string_view::npos) {
      break;
    }
    value.remove_prefix(dot + 1);
  }
  return result;
}

[[nodiscard]] bool matching_form(BuildIdentity const& left,
                                 BuildIdentity const& right) noexcept {
  return left.architecture == right.architecture &&
         left.edition == right.edition && left.form == right.form;
}

}  // namespace

bool BuildIdentity::valid() const noexcept {
  return safe_text(version, 64) && parse_version(version).has_value() &&
         architecture != SystemArchitecture::unknown;
}

bool GithubApplicationAsset::valid() const noexcept {
  return safe_text(asset_id) && target.valid();
}

bool GithubApplicationRelease::valid() const noexcept {
  if (!safe_text(release_id) || !safe_text(tag_name) || title.size() > 512 ||
      assets.empty() || assets.size() > kMaxAssets) {
    return false;
  }
  return std::ranges::all_of(assets,
                             [](GithubApplicationAsset const& asset) {
                               return asset.valid();
                             });
}

VersionComparison compare_versions(std::string_view current,
                                   std::string_view candidate) noexcept {
  auto const left = parse_version(current);
  auto const right = parse_version(candidate);
  if (!left.has_value() || !right.has_value()) {
    return VersionComparison::incomparable;
  }
  auto const size = std::max(left->size, right->size);
  for (std::size_t index = 0; index < size; ++index) {
    auto const left_part = index < left->size ? left->parts[index] : 0;
    auto const right_part = index < right->size ? right->parts[index] : 0;
    if (left_part < right_part) {
      return VersionComparison::higher;
    }
    if (left_part > right_part) {
      return VersionComparison::lower;
    }
  }
  return VersionComparison::equal;
}

bool is_formal_stable_release(GithubApplicationRelease const& release) noexcept {
  if (!release.valid() || release.draft || release.prerelease) {
    return false;
  }
  auto const contains_test_marker = [](std::string_view text) {
    constexpr std::array markers{"beta", "preview", "nightly", "alpha", "rc"};
    std::string lowered;
    lowered.reserve(text.size());
    for (auto const character : text) {
      lowered.push_back(
          static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return std::ranges::any_of(markers, [&](std::string_view marker) {
      return lowered.find(marker) != std::string::npos;
    });
  };
  return !contains_test_marker(release.tag_name) &&
         !contains_test_marker(release.title) &&
         std::ranges::all_of(release.assets,
                             [](GithubApplicationAsset const& asset) {
                               return asset.target.channel ==
                                      ReleaseChannel::stable;
                             });
}

std::optional<Candidate> latest_matching_stable_candidate(
    BuildIdentity const& current,
    std::vector<GithubApplicationRelease> const& releases) noexcept {
  if (!current.valid()) {
    return std::nullopt;
  }
  std::optional<Candidate> selected;
  for (auto const& release : releases) {
    if (!is_formal_stable_release(release)) {
      continue;
    }
    for (auto const& asset : release.assets) {
      if (!asset.valid() || !matching_form(current, asset.target)) {
        continue;
      }
      if (current.channel == ReleaseChannel::stable &&
          compare_versions(current.version, asset.target.version) !=
              VersionComparison::higher) {
        continue;
      }
      if (selected.has_value() &&
          compare_versions(selected->target.version, asset.target.version) !=
              VersionComparison::higher) {
        continue;
      }
      selected = Candidate{
          .release_id = release.release_id,
          .release_tag = release.tag_name,
          .asset_id = asset.asset_id,
          .target = asset.target,
      };
    }
  }
  return selected;
}

bool has_matching_formal_stable_asset(
    BuildIdentity const& current,
    std::vector<GithubApplicationRelease> const& releases) noexcept {
  if (!current.valid()) {
    return false;
  }
  return std::ranges::any_of(
      releases, [&](GithubApplicationRelease const& release) {
        return is_formal_stable_release(release) &&
               std::ranges::any_of(
                   release.assets, [&](GithubApplicationAsset const& asset) {
                     return asset.valid() &&
                            matching_form(current, asset.target);
                   });
      });
}

}  // namespace azzs::domain::application_update
