#include "azzs/adapters/infrastructure/settings_catalog_file_adapter.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::adapters::infrastructure {
namespace {

namespace catalog_app = application::settings_catalog;
namespace catalog_domain = domain::settings_catalog;

constexpr std::size_t kMaxImportBytes = 16U * 1024U * 1024U;
constexpr std::uint32_t kPackageFormatVersion = 2;
constexpr std::uint32_t kStateFormatVersion = 2;
constexpr std::uint64_t kMaxIdentityTombstones = 100'000;
constexpr std::array<std::byte, 8> kStateMagic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'S'}, std::byte{'C'}, std::byte{'0'}, std::byte{'1'},
};

[[nodiscard]] domain::StateKey settings_catalog_key() {
  return domain::StateKey::machine(
      domain::AggregateId{"settings-catalog"});
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view line,
                                                   char delimiter) {
  std::vector<std::string_view> values;
  while (true) {
    auto const position = line.find(delimiter);
    values.push_back(line.substr(0, position));
    if (position == std::string_view::npos) {
      return values;
    }
    line.remove_prefix(position + 1);
  }
}

template <typename Value>
[[nodiscard]] bool parse_unsigned(std::string_view text, Value& value) {
  if (text.empty()) {
    return false;
  }
  auto const* begin = text.data();
  auto const* end = begin + text.size();
  auto const result = std::from_chars(begin, end, value);
  return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_boolean(std::string_view text, bool& value) {
  if (text == "0") {
    value = false;
    return true;
  }
  if (text == "1") {
    value = true;
    return true;
  }
  return false;
}

[[nodiscard]] std::optional<std::string> optional_text(
    std::string_view value) {
  return value == "-" ? std::nullopt
                      : std::optional<std::string>{value};
}

[[nodiscard]] bool parse_windows_version(
    std::string_view text,
    std::optional<catalog_domain::WindowsVersion>& version) {
  if (text == "-") {
    version.reset();
    return true;
  }
  auto const separator = text.find('-');
  auto const half_marker = text.find_first_of("hH", separator + 1);
  if (separator == std::string_view::npos ||
      half_marker == std::string_view::npos || half_marker + 2 != text.size()) {
    return false;
  }
  std::uint16_t generation{};
  std::uint16_t year{};
  std::uint8_t half{};
  if (!parse_unsigned(text.substr(0, separator), generation) ||
      !parse_unsigned(text.substr(separator + 1,
                                  half_marker - separator - 1),
                      year) ||
      !parse_unsigned(text.substr(half_marker + 1), half) ||
      (generation != 10 && generation != 11)) {
    return false;
  }
  catalog_domain::WindowsVersion parsed{
      .generation = static_cast<catalog_domain::WindowsGeneration>(generation),
      .feature_update_year = year,
      .feature_update_half = half,
  };
  if (!parsed.valid()) {
    return false;
  }
  version = parsed;
  return true;
}

[[nodiscard]] std::string windows_version_text(
    std::optional<catalog_domain::WindowsVersion> const& version) {
  if (!version.has_value()) {
    return "-";
  }
  return std::to_string(static_cast<std::uint8_t>(version->generation)) +
         "-" + std::to_string(version->feature_update_year) + "h" +
         std::to_string(version->feature_update_half);
}

struct ParsedPackage final {
  std::optional<catalog_domain::SettingsCatalog> catalog;
  std::string error;
};

[[nodiscard]] ParsedPackage parse_package(std::string_view bytes) {
  struct ParsedMember final {
    std::string plan_id;
    catalog_domain::PlanMember member;
  };

  catalog_domain::SettingsCatalog catalog;
  std::vector<ParsedMember> members;
  bool header_seen = false;
  std::uint32_t package_format{};
  std::istringstream stream{std::string{bytes}};
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(stream, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    auto fields = split(line, '\t');
    if (!header_seen) {
      if (fields.size() != 4 || fields[0] != "AZZS_SETTINGS_CATALOG" ||
          !parse_unsigned(fields[1], package_format) ||
          !parse_unsigned(fields[2], catalog.schema_version) ||
          !parse_unsigned(fields[3], catalog.revision)) {
        return {.error = "settings catalog package header is invalid"};
      }
      if (package_format == 1) {
        return {.error =
                    "settings catalog package format 1 lacks required "
                    "execution facts; export a format 2 package"};
      }
      if (package_format != kPackageFormatVersion) {
        return {.error = "settings catalog package header is invalid"};
      }
      header_seen = true;
      continue;
    }

    if (fields[0] == "SETTING") {
      bool default_selected{};
      std::optional<catalog_domain::WindowsVersion> minimum_windows;
      std::optional<catalog_domain::WindowsVersion> maximum_windows;
      if (fields.size() != 17 ||
          !parse_windows_version(fields[5], minimum_windows) ||
          !parse_windows_version(fields[6], maximum_windows) ||
          !parse_boolean(fields[7], default_selected)) {
        return {.error = "settings catalog SETTING record is invalid at line " +
                         std::to_string(line_number)};
      }
      catalog_domain::RecoveryRequirement recovery{};
      if (fields[8] == "none") {
        recovery = catalog_domain::RecoveryRequirement::unavailable;
      } else if (fields[8] == "restore") {
        recovery =
            catalog_domain::RecoveryRequirement::restore_record_required;
      } else {
        catalog.unknown_semantic_fields.push_back(
            "setting.recovery_requirement=" + std::string{fields[8]});
      }
      catalog_domain::RestartRequirement restart{};
      if (fields[9] == "none") {
        restart = catalog_domain::RestartRequirement::none;
      } else if (fields[9] == "explorer") {
        restart = catalog_domain::RestartRequirement::explorer;
      } else if (fields[9] == "windows") {
        restart = catalog_domain::RestartRequirement::windows;
      } else {
        catalog.unknown_semantic_fields.push_back(
            "setting.restart_requirement=" + std::string{fields[9]});
      }
      std::vector<catalog_domain::StableId> dependencies;
      auto risk = catalog_domain::SettingRiskLevel::low;
      auto force_attempt = catalog_domain::ForceAttemptRule::prohibited;
      if (fields[14] != "-") {
        for (auto dependency : split(fields[14], ',')) {
          dependencies.push_back(
              catalog_domain::StableId{std::string{dependency}});
        }
      }
      if (fields[15] == "low") {
        risk = catalog_domain::SettingRiskLevel::low;
      } else if (fields[15] == "elevated") {
        risk = catalog_domain::SettingRiskLevel::elevated;
      } else {
        catalog.unknown_semantic_fields.push_back(
            "setting.risk=" + std::string{fields[15]});
      }
      if (fields[16] == "prohibited") {
        force_attempt = catalog_domain::ForceAttemptRule::prohibited;
      } else if (fields[16] == "confirm-if-recoverable") {
        force_attempt = catalog_domain::ForceAttemptRule::
            allowed_with_explicit_confirmation;
      } else {
        catalog.unknown_semantic_fields.push_back(
            "setting.force_attempt_rule=" + std::string{fields[16]});
      }
      catalog.settings.push_back(catalog_domain::SettingDefinition{
          .id = catalog_domain::StableId{std::string{fields[1]}},
          .display_name = std::string{fields[2]},
          .description = std::string{fields[3]},
          .source_url = optional_text(fields[4]),
          .known_windows_range = {
              .minimum = minimum_windows,
              .maximum = maximum_windows,
          },
          .depends_on = std::move(dependencies),
          .default_selected = default_selected,
          .risk = risk,
          .force_attempt_rule = force_attempt,
          .recovery_requirement = recovery,
          .restart_requirement = restart,
          .semantics = {
              .identity = std::string{fields[10]},
              .apply_capability = std::string{fields[11]},
              .detect_capability = std::string{fields[12]},
              .recover_capability = optional_text(fields[13]),
          },
      });
      continue;
    }

    if (fields[0] == "PLAN") {
      if (fields.size() != 4) {
        return {.error = "settings catalog PLAN record is invalid at line " +
                         std::to_string(line_number)};
      }
      catalog.plans.push_back(catalog_domain::OptimizationPlan{
          .id = catalog_domain::StableId{std::string{fields[1]}},
          .display_name = std::string{fields[2]},
          .description = std::string{fields[3]},
      });
      continue;
    }

    if (fields[0] == "MEMBER") {
      catalog_domain::PlanMember member;
      bool default_selected{};
      if (fields.size() != 5 ||
          !parse_unsigned(fields[3], member.order) ||
          !parse_boolean(fields[4], default_selected)) {
        return {.error = "settings catalog MEMBER record is invalid at line " +
                         std::to_string(line_number)};
      }
      member.setting_id =
          catalog_domain::StableId{std::string{fields[2]}};
      member.default_selected = default_selected;
      members.push_back({.plan_id = std::string{fields[1]},
                         .member = std::move(member)});
      continue;
    }

    if (fields[0].starts_with("DISPLAY_")) {
      catalog.ignored_display_fields.push_back(std::string{fields[0]});
      continue;
    }

    catalog.unknown_semantic_fields.push_back(std::string{fields[0]});
  }

  if (!header_seen) {
    return {.error = "settings catalog package is empty"};
  }
  for (auto& parsed_member : members) {
    auto plan = std::ranges::find(catalog.plans, parsed_member.plan_id,
                                  [](catalog_domain::OptimizationPlan const& p) {
                                    return p.id.value;
                                  });
    if (plan == catalog.plans.end()) {
      return {.error =
                  "settings catalog MEMBER references an unknown plan"};
    }
    plan->members.push_back(std::move(parsed_member.member));
  }
  return {.catalog = std::move(catalog)};
}

[[nodiscard]] std::string text_or_dash(
    std::optional<std::string> const& value) {
  return value.value_or("-");
}

[[nodiscard]] std::string serialize_package(
    catalog_domain::SettingsCatalog const& catalog) {
  std::ostringstream output;
  output << "AZZS_SETTINGS_CATALOG\t" << kPackageFormatVersion << '\t'
         << catalog.schema_version << '\t' << catalog.revision << '\n';
  for (auto const& setting : catalog.settings) {
    auto const recovery =
        setting.recovery_requirement ==
                catalog_domain::RecoveryRequirement::restore_record_required
            ? "restore"
            : "none";
    auto restart = "none";
    if (setting.restart_requirement ==
        catalog_domain::RestartRequirement::explorer) {
      restart = "explorer";
    } else if (setting.restart_requirement ==
               catalog_domain::RestartRequirement::windows) {
      restart = "windows";
    }
    output << "SETTING\t" << setting.id.value << '\t'
           << setting.display_name << '\t' << setting.description << '\t'
           << text_or_dash(setting.source_url) << '\t'
           << windows_version_text(setting.known_windows_range.minimum) << '\t'
           << windows_version_text(setting.known_windows_range.maximum) << '\t'
           << (setting.default_selected ? 1 : 0) << '\t' << recovery << '\t'
           << restart << '\t' << setting.semantics.identity << '\t'
           << setting.semantics.apply_capability << '\t'
           << setting.semantics.detect_capability << '\t'
           << text_or_dash(setting.semantics.recover_capability) << '\t';
    if (setting.depends_on.empty()) {
      output << '-';
    } else {
      for (std::size_t index = 0; index < setting.depends_on.size(); ++index) {
        if (index != 0) {
          output << ',';
        }
        output << setting.depends_on[index].value;
      }
    }
    output << '\t'
           << (setting.risk == catalog_domain::SettingRiskLevel::low
                   ? "low"
                   : "elevated")
           << '\t'
           << (setting.force_attempt_rule ==
                       catalog_domain::ForceAttemptRule::prohibited
                   ? "prohibited"
                   : "confirm-if-recoverable")
           << '\n';
  }
  for (auto const& plan : catalog.plans) {
    output << "PLAN\t" << plan.id.value << '\t' << plan.display_name << '\t'
           << plan.description << '\n';
    for (auto const& member : plan.members) {
      output << "MEMBER\t" << plan.id.value << '\t'
             << member.setting_id.value << '\t' << member.order << '\t'
             << (member.default_selected ? 1 : 0) << '\n';
    }
  }
  return std::move(output).str();
}

class Encoder final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void u64(std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void raw(std::span<std::byte const> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }

  void text(std::string_view text) {
    u64(text.size());
    raw(std::as_bytes(std::span{text}));
  }

  [[nodiscard]] domain::StateBytes finish() { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class Decoder final {
 public:
  explicit Decoder(std::span<std::byte const> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t& value) {
    if (remaining() < 1) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(part) << shift;
    }
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) {
        return false;
      }
      value |= static_cast<std::uint64_t>(part) << shift;
    }
    return true;
  }

  [[nodiscard]] bool raw(std::span<std::byte> target) {
    if (remaining() < target.size()) {
      return false;
    }
    std::ranges::copy(bytes_.subspan(position_, target.size()),
                      target.begin());
    position_ += target.size();
    return true;
  }

  [[nodiscard]] bool text(std::string& value) {
    std::uint64_t size{};
    if (!u64(size) || size > kMaxImportBytes || remaining() < size) {
      return false;
    }
    value.assign(
        reinterpret_cast<char const*>(bytes_.data() + position_),
        static_cast<std::size_t>(size));
    position_ += static_cast<std::size_t>(size);
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

 private:
  std::span<std::byte const> bytes_;
  std::size_t position_{0};
};

[[nodiscard]] domain::StateBytes encode_state(
    catalog_app::SettingsCatalogState const& state) {
  Encoder encoder;
  encoder.raw(kStateMagic);
  encoder.u32(kStateFormatVersion);
  encoder.text(serialize_package(state.current));
  encoder.u8(state.previous.has_value() ? 1 : 0);
  if (state.previous.has_value()) {
    encoder.text(serialize_package(*state.previous));
  }
  encoder.u64(state.identity_tombstones.size());
  for (auto const& tombstone : state.identity_tombstones) {
    encoder.u8(static_cast<std::uint8_t>(tombstone.kind));
    encoder.text(tombstone.id.value);
    encoder.text(tombstone.semantic_fingerprint);
  }
  return encoder.finish();
}

struct DecodedState final {
  std::optional<catalog_app::SettingsCatalogState> state;
  std::string error;
};

[[nodiscard]] DecodedState decode_state(domain::StateBytes const& bytes) {
  Decoder decoder{bytes};
  std::array<std::byte, kStateMagic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t has_previous{};
  std::string current_bytes;
  if (!decoder.raw(magic) || magic != kStateMagic || !decoder.u32(version) ||
      !decoder.text(current_bytes) ||
      !decoder.u8(has_previous) || has_previous > 1) {
    return {.error = "settings catalog state payload is invalid or unsupported"};
  }
  if (version == 1) {
    return {.error =
                "settings catalog state format 1 lacks complete identity "
                "history; controlled state recovery is required"};
  }
  if (version != kStateFormatVersion) {
    return {.error = "settings catalog state payload is invalid or unsupported"};
  }
  auto current = parse_package(current_bytes);
  if (!current.catalog.has_value()) {
    return {.error = std::move(current.error)};
  }
  catalog_app::SettingsCatalogState state{
      .current = std::move(*current.catalog)};
  if (has_previous == 1) {
    std::string previous_bytes;
    if (!decoder.text(previous_bytes)) {
      return {.error = "settings catalog state payload is invalid or unsupported"};
    }
    auto previous = parse_package(previous_bytes);
    if (!previous.catalog.has_value()) {
      return {.error = std::move(previous.error)};
    }
    state.previous = std::move(*previous.catalog);
  }
  std::uint64_t tombstone_count{};
  if (!decoder.u64(tombstone_count) ||
      tombstone_count > kMaxIdentityTombstones) {
    return {.error = "settings catalog state payload is invalid or unsupported"};
  }
  state.identity_tombstones.reserve(static_cast<std::size_t>(tombstone_count));
  for (std::uint64_t index = 0; index < tombstone_count; ++index) {
    std::uint8_t kind{};
    catalog_domain::CatalogIdentityTombstone tombstone;
    if (!decoder.u8(kind) ||
        kind > static_cast<std::uint8_t>(
                   catalog_domain::CatalogItemKind::optimization_plan) ||
        !decoder.text(tombstone.id.value) ||
        !decoder.text(tombstone.semantic_fingerprint) || !tombstone.id.valid() ||
        tombstone.semantic_fingerprint.empty()) {
      return {.error = "settings catalog state payload is invalid or unsupported"};
    }
    tombstone.kind = static_cast<catalog_domain::CatalogItemKind>(kind);
    if (std::ranges::any_of(
            state.identity_tombstones,
            [&](catalog_domain::CatalogIdentityTombstone const& existing) {
              return existing.kind == tombstone.kind && existing.id == tombstone.id;
            })) {
      return {.error = "settings catalog state payload is invalid or unsupported"};
    }
    state.identity_tombstones.push_back(std::move(tombstone));
  }
  if (decoder.remaining() != 0) {
    return {.error = "settings catalog state payload is invalid or unsupported"};
  }
  return {.state = std::move(state)};
}

[[nodiscard]] catalog_app::CatalogStorageWriteStatus map_write_status(
    application::StateCommitStatus status) noexcept {
  using application::StateCommitStatus;
  switch (status) {
    case StateCommitStatus::committed:
      return catalog_app::CatalogStorageWriteStatus::committed;
    case StateCommitStatus::conflict:
      return catalog_app::CatalogStorageWriteStatus::conflict;
    case StateCommitStatus::read_only:
      return catalog_app::CatalogStorageWriteStatus::read_only;
    case StateCommitStatus::busy:
      return catalog_app::CatalogStorageWriteStatus::busy;
    case StateCommitStatus::failed:
      return catalog_app::CatalogStorageWriteStatus::failed;
    case StateCommitStatus::outcome_unknown:
      return catalog_app::CatalogStorageWriteStatus::outcome_unknown;
  }
  return catalog_app::CatalogStorageWriteStatus::failed;
}

}  // namespace

SettingsCatalogFileAdapter::SettingsCatalogFileAdapter(
    application::DeviceStateStore& states) noexcept
    : states_(states) {}

catalog_app::CatalogStorageRead SettingsCatalogFileAdapter::read() {
  auto stored = states_.inspect(settings_catalog_key());
  catalog_app::CatalogStorageReadStatus status{};
  switch (stored.mode) {
    case application::StateReadMode::uninitialized:
      return {.status = catalog_app::CatalogStorageReadStatus::uninitialized};
    case application::StateReadMode::writable:
      status = catalog_app::CatalogStorageReadStatus::writable;
      break;
    case application::StateReadMode::recovered_previous:
      status = catalog_app::CatalogStorageReadStatus::recovered_read_only;
      break;
    case application::StateReadMode::read_only_future:
    case application::StateReadMode::read_only_corrupt:
      return {.status = catalog_app::CatalogStorageReadStatus::read_only,
              .detail = std::move(stored.error)};
    case application::StateReadMode::busy:
      return {.status = catalog_app::CatalogStorageReadStatus::busy,
              .detail = std::move(stored.error)};
    case application::StateReadMode::failed:
      return {.status = catalog_app::CatalogStorageReadStatus::failed,
              .detail = std::move(stored.error)};
  }
  if (!stored.snapshot.has_value()) {
    return {.status = catalog_app::CatalogStorageReadStatus::failed,
            .detail = "state store returned no settings catalog snapshot"};
  }
  auto decoded = decode_state(stored.snapshot->state.value.payload);
  if (!decoded.state.has_value()) {
    return {.status = catalog_app::CatalogStorageReadStatus::read_only,
            .revision = stored.snapshot->revision,
            .detail = std::move(decoded.error)};
  }
  return {.status = status,
          .state = std::move(decoded.state),
          .revision = stored.snapshot->revision};
}

catalog_app::CatalogStorageWrite SettingsCatalogFileAdapter::write(
    std::optional<domain::RevisionToken> expected_revision,
    catalog_app::SettingsCatalogState state) {
  auto desired_payload = encode_state(state);
  auto canonical_desired = decode_state(desired_payload);
  if (!canonical_desired.state.has_value()) {
    return {.status = catalog_app::CatalogStorageWriteStatus::failed,
            .detail = "settings catalog state could not be encoded"};
  }
  domain::DeviceState desired_state{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = desired_payload},
  };
  application::StateCommitResult committed;
  if (expected_revision.has_value()) {
    committed = states_.commit(application::StateCommitRequest{
        .key = settings_catalog_key(),
        .expected_revision = *expected_revision,
        .state = std::move(desired_state),
    });
  } else {
    committed =
        states_.initialize(settings_catalog_key(), std::move(desired_state));
  }
  if (committed.status == application::StateCommitStatus::committed) {
    return {.status = catalog_app::CatalogStorageWriteStatus::committed,
            .revision = committed.snapshot.has_value()
                            ? std::optional{committed.snapshot->revision}
                            : std::nullopt};
  }

  // DeviceStateStore can explicitly report an unknown publication outcome.
  // Confirm only this aggregate by authoritative reread; never infer success
  // from a file operation alone.
  if (committed.status == application::StateCommitStatus::outcome_unknown) {
    auto confirmed = read();
    if (confirmed.status == catalog_app::CatalogStorageReadStatus::writable &&
        confirmed.state.has_value() &&
        *confirmed.state == *canonical_desired.state) {
      return {.status = catalog_app::CatalogStorageWriteStatus::committed,
              .revision = std::move(confirmed.revision),
              .detail =
                  "settings catalog commit confirmed by authoritative reread"};
    }
  }
  return {.status = map_write_status(committed.status),
          .revision = committed.snapshot.has_value()
                          ? std::optional{committed.snapshot->revision}
                          : std::nullopt,
          .detail = std::move(committed.error)};
}

catalog_app::CatalogImportRead SettingsCatalogFileAdapter::read_import(
    std::string const& path) {
  if (path.empty()) {
    return {.status = catalog_app::CatalogImportStatus::invalid,
            .detail = "settings catalog import path is empty"};
  }
  std::ifstream input{path, std::ios::binary | std::ios::ate};
  if (!input) {
    return {.status = catalog_app::CatalogImportStatus::not_found,
            .source_path = path,
            .detail = "settings catalog import file could not be opened"};
  }
  auto const end = input.tellg();
  if (end < 0 || static_cast<std::uint64_t>(end) > kMaxImportBytes) {
    return {.status = catalog_app::CatalogImportStatus::invalid,
            .source_path = path,
            .detail = "settings catalog import file is too large"};
  }
  std::string bytes(static_cast<std::size_t>(end), '\0');
  input.seekg(0, std::ios::beg);
  if (!bytes.empty() &&
      !input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    return {.status = catalog_app::CatalogImportStatus::failed,
            .source_path = path,
            .detail = "settings catalog import file could not be read"};
  }
  auto parsed = parse_package(bytes);
  if (!parsed.catalog.has_value()) {
    return {.status = catalog_app::CatalogImportStatus::invalid,
            .source_path = path,
            .detail = std::move(parsed.error)};
  }
  return {.status = catalog_app::CatalogImportStatus::loaded,
          .catalog = std::move(parsed.catalog),
          .source_path = path};
}

}  // namespace azzs::adapters::infrastructure
