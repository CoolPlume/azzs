#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "azzs/domain/system_architecture.hpp"

namespace azzs::domain::application_update {

enum class ReleaseChannel {
  stable,
  prerelease,
};

enum class ReleaseEdition {
  standard,
  rescue,
  large_offline,
};

enum class ReleaseForm {
  portable,
  installed,
};

struct BuildIdentity final {
  std::string version;
  ReleaseChannel channel{ReleaseChannel::stable};
  SystemArchitecture architecture{SystemArchitecture::unknown};
  ReleaseEdition edition{ReleaseEdition::standard};
  ReleaseForm form{ReleaseForm::portable};

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(BuildIdentity const&, BuildIdentity const&) = default;
};

struct GithubApplicationAsset final {
  std::string asset_id;
  BuildIdentity target;
  // These fields are retained only to present a controlled download entry.
  // They are never treated as executable instructions or arbitrary paths.
  std::string name;
  std::string download_url;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(GithubApplicationAsset const&,
                         GithubApplicationAsset const&) = default;
};

struct GithubApplicationRelease final {
  std::string release_id;
  std::string tag_name;
  std::string title;
  bool draft{false};
  bool prerelease{false};
  std::vector<GithubApplicationAsset> assets;
  std::string html_url;
  std::string published_at;
  std::string body;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(GithubApplicationRelease const&,
                         GithubApplicationRelease const&) = default;
};

struct Candidate final {
  std::string release_id;
  std::string release_tag;
  std::string asset_id;
  BuildIdentity target;
  std::string release_title;
  std::string release_url;
  std::string published_at;
  std::string summary;

  friend bool operator==(Candidate const&, Candidate const&) = default;
};

enum class VersionComparison {
  lower,
  equal,
  higher,
  incomparable,
};

[[nodiscard]] VersionComparison compare_versions(
    std::string_view current, std::string_view candidate) noexcept;

[[nodiscard]] constexpr std::optional<ReleaseChannel> parse_release_channel(
    std::string_view value) noexcept {
  if (value == "stable") {
    return ReleaseChannel::stable;
  }
  if (value == "prerelease") {
    return ReleaseChannel::prerelease;
  }
  return std::nullopt;
}

[[nodiscard]] bool is_formal_stable_release(
    GithubApplicationRelease const& release) noexcept;

[[nodiscard]] std::optional<Candidate> latest_matching_stable_candidate(
    BuildIdentity const& current,
    std::vector<GithubApplicationRelease> const& releases) noexcept;

[[nodiscard]] bool has_matching_formal_stable_asset(
    BuildIdentity const& current,
    std::vector<GithubApplicationRelease> const& releases) noexcept;

}  // namespace azzs::domain::application_update
