#include "azzs/application/software_selection.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::application::software_selection {
namespace {

constexpr std::array<std::byte, 8> k_subject_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'S'}, std::byte{'E'},
    std::byte{'L'}, std::byte{'S'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::array<std::byte, 8> k_machine_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'S'}, std::byte{'E'},
    std::byte{'L'}, std::byte{'H'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t k_subject_payload_version = 2;
constexpr std::uint32_t k_legacy_subject_payload_version = 1;
constexpr std::uint32_t k_machine_payload_version = 2;
constexpr std::uint32_t k_legacy_machine_payload_version = 1;
constexpr std::size_t k_max_payload_text = 4U * 1024U * 1024U;
constexpr std::uint32_t k_max_handoff_facts = 4096;

class ByteWriter final {
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

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (auto const character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }

  template <std::size_t Size>
  void raw(std::array<std::byte, Size> const& value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  [[nodiscard]] domain::StateBytes finish() && { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class ByteReader final {
 public:
  explicit ByteReader(std::span<std::byte const> bytes) : bytes_(bytes) {}

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
      std::uint8_t byte{};
      if (!u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) {
      std::uint8_t byte{};
      if (!u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
  }

  [[nodiscard]] bool text(std::string& value) {
    std::uint32_t size{};
    if (!u32(size) || size > k_max_payload_text || remaining() < size) {
      return false;
    }
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
      std::uint8_t character{};
      if (!u8(character)) {
        return false;
      }
      value.push_back(static_cast<char>(character));
    }
    return true;
  }

  template <std::size_t Size>
  [[nodiscard]] bool raw(std::array<std::byte, Size>& value) {
    if (remaining() < Size) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_), Size,
                value.begin());
    position_ += Size;
    return true;
  }

  [[nodiscard]] bool complete() const noexcept { return position_ == bytes_.size(); }

 private:
  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - position_;
  }

  std::span<std::byte const> bytes_;
  std::size_t position_{};
};

[[nodiscard]] std::optional<std::uint8_t> encode_source_purpose(
    catalog_domain::SourcePurpose purpose) {
  switch (purpose) {
    case catalog_domain::SourcePurpose::primary:
      return 1;
    case catalog_domain::SourcePurpose::alternative:
      return 2;
    case catalog_domain::SourcePurpose::project_backup:
      return 3;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<catalog_domain::SourcePurpose> decode_source_purpose(
    std::uint8_t value) {
  switch (value) {
    case 1:
      return catalog_domain::SourcePurpose::primary;
    case 2:
      return catalog_domain::SourcePurpose::alternative;
    case 3:
      return catalog_domain::SourcePurpose::project_backup;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_architecture(
    architecture_selection::selection_domain::PackageArchitecture architecture) {
  switch (architecture) {
    case architecture_selection::selection_domain::PackageArchitecture::x86:
      return 5;
    case architecture_selection::selection_domain::PackageArchitecture::x64:
      return 1;
    case architecture_selection::selection_domain::PackageArchitecture::arm64:
      return 2;
    case architecture_selection::selection_domain::PackageArchitecture::
        architecture_independent:
      return 3;
    case architecture_selection::selection_domain::PackageArchitecture::unknown:
      return 4;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<architecture_selection::selection_domain::
                                PackageArchitecture>
decode_architecture(std::uint8_t value) {
  using Architecture = architecture_selection::selection_domain::
      PackageArchitecture;
  switch (value) {
    case 5:
      return Architecture::x86;
    case 1:
      return Architecture::x64;
    case 2:
      return Architecture::arm64;
    case 3:
      return Architecture::architecture_independent;
    case 4:
      return Architecture::unknown;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_package_type(
    selection_domain::PackageType type) {
  switch (type) {
    case selection_domain::PackageType::full_package:
      return 1;
    case selection_domain::PackageType::online_installer:
      return 2;
    case selection_domain::PackageType::external_handoff:
      return 3;
    case selection_domain::PackageType::archive_package:
      return 4;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<selection_domain::PackageType> decode_package_type(
    std::uint8_t value) {
  switch (value) {
    case 1:
      return selection_domain::PackageType::full_package;
    case 2:
      return selection_domain::PackageType::online_installer;
    case 3:
      return selection_domain::PackageType::external_handoff;
    case 4:
      return selection_domain::PackageType::archive_package;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_handoff_status(
    selection_domain::ExternalHandoffStatus status) {
  switch (status) {
    case selection_domain::ExternalHandoffStatus::none:
      return 1;
    case selection_domain::ExternalHandoffStatus::waiting_for_external_install:
      return 2;
    case selection_domain::ExternalHandoffStatus::externally_recognized:
      return 3;
    case selection_domain::ExternalHandoffStatus::skipped:
      return 4;
    case selection_domain::ExternalHandoffStatus::awaiting_user_confirmation:
      return 5;
    case selection_domain::ExternalHandoffStatus::completed:
      return 6;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<selection_domain::ExternalHandoffStatus>
decode_handoff_status(std::uint8_t value) {
  switch (value) {
    case 1:
      return selection_domain::ExternalHandoffStatus::none;
    case 2:
      return selection_domain::ExternalHandoffStatus::waiting_for_external_install;
    case 3:
      return selection_domain::ExternalHandoffStatus::externally_recognized;
    case 4:
      return selection_domain::ExternalHandoffStatus::skipped;
    case 5:
      return selection_domain::ExternalHandoffStatus::awaiting_user_confirmation;
    case 6:
      return selection_domain::ExternalHandoffStatus::completed;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_handoff_fact_kind(
    selection_domain::ExternalHandoffFactKind kind) {
  using FactKind = selection_domain::ExternalHandoffFactKind;
  switch (kind) {
    case FactKind::source_resolution_failed:
      return 1;
    case FactKind::declared_address_opened:
      return 2;
    case FactKind::returned_for_recheck:
      return 3;
    case FactKind::skipped:
      return 4;
    case FactKind::continued:
      return 5;
    case FactKind::awaiting_user_confirmation:
      return 6;
    case FactKind::user_confirmed:
      return 7;
    case FactKind::completed:
      return 8;
    case FactKind::legacy_record_imported:
      return 9;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<selection_domain::ExternalHandoffFactKind>
decode_handoff_fact_kind(std::uint8_t value) {
  using FactKind = selection_domain::ExternalHandoffFactKind;
  switch (value) {
    case 1:
      return FactKind::source_resolution_failed;
    case 2:
      return FactKind::declared_address_opened;
    case 3:
      return FactKind::returned_for_recheck;
    case 4:
      return FactKind::skipped;
    case 5:
      return FactKind::continued;
    case 6:
      return FactKind::awaiting_user_confirmation;
    case 7:
      return FactKind::user_confirmed;
    case 8:
      return FactKind::completed;
    case 9:
      return FactKind::legacy_record_imported;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_fact_availability(
    selection_domain::ExternalHandoffFactAvailability value) {
  switch (value) {
    case selection_domain::ExternalHandoffFactAvailability::obtained:
      return 1;
    case selection_domain::ExternalHandoffFactAvailability::not_obtained:
      return 2;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<selection_domain::ExternalHandoffFactAvailability>
decode_fact_availability(std::uint8_t value) {
  switch (value) {
    case 1:
      return selection_domain::ExternalHandoffFactAvailability::obtained;
    case 2:
      return selection_domain::ExternalHandoffFactAvailability::not_obtained;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::uint8_t> encode_not_obtained_reason(
    selection_domain::ExternalHandoffNotObtainedReason reason) {
  using Reason = selection_domain::ExternalHandoffNotObtainedReason;
  switch (reason) {
    case Reason::none:
      return 1;
    case Reason::resolution_failed:
      return 2;
    case Reason::no_persisted_resolved_source:
      return 3;
    case Reason::not_captured_for_this_fact:
      return 4;
    case Reason::legacy_record_has_no_historical_detail:
      return 5;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<selection_domain::ExternalHandoffNotObtainedReason>
decode_not_obtained_reason(std::uint8_t value) {
  using Reason = selection_domain::ExternalHandoffNotObtainedReason;
  switch (value) {
    case 1:
      return Reason::none;
    case 2:
      return Reason::resolution_failed;
    case 3:
      return Reason::no_persisted_resolved_source;
    case 4:
      return Reason::not_captured_for_this_fact;
    case 5:
      return Reason::legacy_record_has_no_historical_detail;
    default:
      return std::nullopt;
  }
}

void write_snapshot(ByteWriter& writer,
                    selection_domain::ResolvedSourceSnapshot const& snapshot) {
  writer.text(snapshot.software_id);
  writer.u8(*encode_source_purpose(snapshot.declared_purpose));
  writer.text(snapshot.declared_address);
  writer.text(snapshot.version);
  writer.text(snapshot.actual_address);
  writer.text(snapshot.hosting_mechanism);
  writer.text(snapshot.branch);
  writer.u8(snapshot.network_required ? 1 : 0);
  writer.u64(static_cast<std::uint64_t>(snapshot.resolved_at_milliseconds));
  writer.text(snapshot.capability_version);
  writer.u32(static_cast<std::uint32_t>(snapshot.packages.size()));
  for (auto const& package : snapshot.packages) {
    writer.text(package.candidate.software_id);
    writer.u8(*encode_architecture(package.candidate.architecture));
    writer.text(package.candidate.version);
    writer.text(package.candidate.identity);
    writer.u8(*encode_package_type(package.package_type));
    writer.u8(package.complete_package ? 1 : 0);
    writer.u8(package.network_required ? 1 : 0);
    writer.u8(package.expected_bytes.has_value() ? 1 : 0);
    if (package.expected_bytes.has_value()) {
      writer.u64(*package.expected_bytes);
    }
    writer.u8(package.expected_sha256.has_value() ? 1 : 0);
    if (package.expected_sha256.has_value()) {
      writer.text(*package.expected_sha256);
    }
    if (package.package_type == selection_domain::PackageType::archive_package) {
      writer.u32(static_cast<std::uint32_t>(package.archive_members.size()));
      for (auto const& member : package.archive_members) {
        writer.text(member);
      }
    }
  }
}

[[nodiscard]] bool read_snapshot(
    ByteReader& reader, selection_domain::ResolvedSourceSnapshot& snapshot,
    bool with_integrity) {
  std::uint8_t encoded_purpose{};
  std::uint8_t network_required{};
  std::uint64_t resolved_at{};
  std::uint32_t package_count{};
  if (!reader.text(snapshot.software_id) || !reader.u8(encoded_purpose) ||
      !reader.text(snapshot.declared_address) || !reader.text(snapshot.version) ||
      !reader.text(snapshot.actual_address) ||
      !reader.text(snapshot.hosting_mechanism) || !reader.text(snapshot.branch) ||
      !reader.u8(network_required) || network_required > 1 ||
      !reader.u64(resolved_at) || !reader.text(snapshot.capability_version) ||
      !reader.u32(package_count) || package_count > 1024) {
    return false;
  }
  auto const purpose = decode_source_purpose(encoded_purpose);
  if (!purpose.has_value() ||
      resolved_at > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  snapshot.declared_purpose = *purpose;
  snapshot.network_required = network_required != 0;
  snapshot.resolved_at_milliseconds = static_cast<std::int64_t>(resolved_at);
  snapshot.packages.clear();
  snapshot.packages.reserve(package_count);
  for (std::uint32_t index = 0; index < package_count; ++index) {
    selection_domain::ResolvedPackage package;
    std::uint8_t encoded_architecture{};
    std::uint8_t encoded_type{};
    std::uint8_t complete_package{};
    std::uint8_t package_network_required{};
    std::uint8_t expected_bytes_present{};
    std::uint8_t expected_sha256_present{};
    if (!reader.text(package.candidate.software_id) ||
        !reader.u8(encoded_architecture) ||
        !reader.text(package.candidate.version) ||
        !reader.text(package.candidate.identity) ||
        !reader.u8(encoded_type) || !reader.u8(complete_package) ||
        !reader.u8(package_network_required) || complete_package > 1 ||
        package_network_required > 1) {
      return false;
    }
    auto const architecture = decode_architecture(encoded_architecture);
    auto const type = decode_package_type(encoded_type);
    if (!architecture.has_value() || !type.has_value()) {
      return false;
    }
    package.candidate.architecture = *architecture;
    package.package_type = *type;
    package.complete_package = complete_package != 0;
    package.network_required = package_network_required != 0;
    if (with_integrity) {
      if (!reader.u8(expected_bytes_present) || expected_bytes_present > 1) {
        return false;
      }
      if (expected_bytes_present != 0) {
        std::uint64_t expected_bytes{};
        if (!reader.u64(expected_bytes)) {
          return false;
        }
        package.expected_bytes = expected_bytes;
      }
      if (!reader.u8(expected_sha256_present) || expected_sha256_present > 1) {
        return false;
      }
      if (expected_sha256_present != 0) {
        std::string expected_sha256;
        if (!reader.text(expected_sha256) || expected_sha256.size() != 64U) {
          return false;
        }
        package.expected_sha256 = std::move(expected_sha256);
      }
    }
    if (package.package_type == selection_domain::PackageType::archive_package) {
      std::uint32_t member_count{};
      if (!reader.u32(member_count) || member_count == 0 || member_count > 64) {
        return false;
      }
      package.archive_members.reserve(member_count);
      for (std::uint32_t member_index = 0; member_index < member_count;
           ++member_index) {
        std::string member;
        if (!reader.text(member) || member.empty() || member.size() > 512) {
          return false;
        }
        package.archive_members.push_back(std::move(member));
      }
    }
    snapshot.packages.push_back(std::move(package));
  }
  return snapshot.valid();
}

[[nodiscard]] domain::StateBytes encode_subject(
    selection_domain::SelectionState const& selection,
    std::vector<selection_domain::ResolvedSourceSnapshot> const& sources) {
  ByteWriter writer;
  writer.raw(k_subject_magic);
  writer.u32(k_subject_payload_version);
  writer.u8(selection.initialized ? 1 : 0);
  writer.u32(static_cast<std::uint32_t>(selection.selected_software_ids.size()));
  for (auto const& id : selection.selected_software_ids) {
    writer.text(id);
  }
  writer.u32(static_cast<std::uint32_t>(sources.size()));
  for (auto const& source : sources) {
    write_snapshot(writer, source);
  }
  return std::move(writer).finish();
}

[[nodiscard]] bool decode_subject(
    std::span<std::byte const> bytes, selection_domain::SelectionState& selection,
    std::vector<selection_domain::ResolvedSourceSnapshot>& sources) {
  ByteReader reader{bytes};
  std::array<std::byte, k_subject_magic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t initialized{};
  std::uint32_t selected_count{};
  std::uint32_t source_count{};
  if (!reader.raw(magic) || magic != k_subject_magic || !reader.u32(version) ||
      (version != k_subject_payload_version &&
       version != k_legacy_subject_payload_version) || !reader.u8(initialized) ||
      initialized > 1 ||
      !reader.u32(selected_count) || selected_count > 4096) {
    return false;
  }
  selection = {.initialized = initialized != 0};
  selection.selected_software_ids.reserve(selected_count);
  for (std::uint32_t index = 0; index < selected_count; ++index) {
    std::string id;
    if (!reader.text(id) || id.empty()) {
      return false;
    }
    selection.selected_software_ids.push_back(std::move(id));
  }
  std::ranges::sort(selection.selected_software_ids);
  if (std::ranges::adjacent_find(selection.selected_software_ids) !=
      selection.selected_software_ids.end()) {
    return false;
  }
  if (!reader.u32(source_count) || source_count > 4096) {
    return false;
  }
  sources.clear();
  sources.reserve(source_count);
  for (std::uint32_t index = 0; index < source_count; ++index) {
    selection_domain::ResolvedSourceSnapshot source;
    if (!read_snapshot(reader, source, version == k_subject_payload_version)) {
      return false;
    }
    sources.push_back(std::move(source));
  }
  return reader.complete();
}

void write_source_fact(
    ByteWriter& writer,
    selection_domain::ExternalHandoffResolvedSourceFact const& source) {
  writer.u8(*encode_fact_availability(source.availability));
  writer.u8(*encode_not_obtained_reason(source.not_obtained_reason));
  writer.text(source.resolved_address);
  writer.text(source.resolved_version);
  writer.text(source.resolver_capability_version);
  writer.u64(static_cast<std::uint64_t>(source.resolved_at_milliseconds));
}

[[nodiscard]] bool read_source_fact(
    ByteReader& reader,
    selection_domain::ExternalHandoffResolvedSourceFact& source) {
  std::uint8_t availability{};
  std::uint8_t not_obtained_reason{};
  std::uint64_t resolved_at{};
  if (!reader.u8(availability) || !reader.u8(not_obtained_reason) ||
      !reader.text(source.resolved_address) ||
      !reader.text(source.resolved_version) ||
      !reader.text(source.resolver_capability_version) || !reader.u64(resolved_at) ||
      resolved_at >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  auto const decoded_availability = decode_fact_availability(availability);
  auto const decoded_reason = decode_not_obtained_reason(not_obtained_reason);
  if (!decoded_availability.has_value() || !decoded_reason.has_value()) {
    return false;
  }
  source.availability = *decoded_availability;
  source.not_obtained_reason = *decoded_reason;
  source.resolved_at_milliseconds = static_cast<std::int64_t>(resolved_at);
  return true;
}

void write_handoff_fact(ByteWriter& writer,
                        selection_domain::ExternalHandoffFact const& fact) {
  writer.u8(*encode_handoff_fact_kind(fact.kind));
  writer.u8(*encode_handoff_status(fact.status));
  writer.u8(*encode_fact_availability(fact.timestamp_availability));
  writer.u8(*encode_not_obtained_reason(fact.timestamp_not_obtained_reason));
  writer.u64(static_cast<std::uint64_t>(fact.occurred_at_milliseconds));
  writer.text(fact.correlation_id);
  writer.text(fact.declared_address);
  write_source_fact(writer, fact.resolved_source);
  writer.text(fact.detail);
}

[[nodiscard]] bool read_handoff_fact(ByteReader& reader,
                                     selection_domain::ExternalHandoffFact& fact) {
  std::uint8_t kind{};
  std::uint8_t status{};
  std::uint8_t timestamp_availability{};
  std::uint8_t timestamp_not_obtained_reason{};
  std::uint64_t occurred_at{};
  if (!reader.u8(kind) || !reader.u8(status) || !reader.u8(timestamp_availability) ||
      !reader.u8(timestamp_not_obtained_reason) || !reader.u64(occurred_at) ||
      occurred_at >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      !reader.text(fact.correlation_id) || !reader.text(fact.declared_address) ||
      !read_source_fact(reader, fact.resolved_source) || !reader.text(fact.detail)) {
    return false;
  }
  auto const decoded_kind = decode_handoff_fact_kind(kind);
  auto const decoded_status = decode_handoff_status(status);
  auto const decoded_timestamp_availability =
      decode_fact_availability(timestamp_availability);
  auto const decoded_timestamp_reason =
      decode_not_obtained_reason(timestamp_not_obtained_reason);
  if (!decoded_kind.has_value() || !decoded_status.has_value() ||
      !decoded_timestamp_availability.has_value() ||
      !decoded_timestamp_reason.has_value()) {
    return false;
  }
  fact.kind = *decoded_kind;
  fact.status = *decoded_status;
  fact.timestamp_availability = *decoded_timestamp_availability;
  fact.timestamp_not_obtained_reason = *decoded_timestamp_reason;
  fact.occurred_at_milliseconds = static_cast<std::int64_t>(occurred_at);
  return fact.valid();
}

[[nodiscard]] domain::StateBytes encode_machine(
    std::vector<selection_domain::ExternalHandoffRecord> const& handoffs) {
  ByteWriter writer;
  writer.raw(k_machine_magic);
  writer.u32(k_machine_payload_version);
  writer.u32(static_cast<std::uint32_t>(handoffs.size()));
  for (auto const& handoff : handoffs) {
    writer.text(handoff.software_id);
    writer.text(handoff.declared_address);
    writer.u8(*encode_handoff_status(handoff.status));
    writer.text(handoff.detail);
    writer.u32(static_cast<std::uint32_t>(handoff.timeline.facts.size()));
    for (auto const& fact : handoff.timeline.facts) {
      write_handoff_fact(writer, fact);
    }
  }
  return std::move(writer).finish();
}

[[nodiscard]] bool decode_legacy_machine(
    ByteReader& reader,
    std::vector<selection_domain::ExternalHandoffRecord>& handoffs) {
  std::uint32_t count{};
  if (!reader.u32(count) || count > 4096) {
    return false;
  }
  handoffs.clear();
  handoffs.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    selection_domain::ExternalHandoffRecord handoff;
    std::uint8_t encoded_status{};
    if (!reader.text(handoff.software_id) ||
        !reader.text(handoff.declared_address) || !reader.u8(encoded_status) ||
        !reader.text(handoff.detail) || handoff.software_id.empty() ||
        handoff.declared_address.empty()) {
      return false;
    }
    auto const status = decode_handoff_status(encoded_status);
    if (!status.has_value()) {
      return false;
    }
    handoff.status = *status;
    handoff.timeline.facts.push_back({
        .kind = selection_domain::ExternalHandoffFactKind::legacy_record_imported,
        .status = handoff.status,
        .timestamp_availability =
            selection_domain::ExternalHandoffFactAvailability::not_obtained,
        .timestamp_not_obtained_reason =
            selection_domain::ExternalHandoffNotObtainedReason::
                legacy_record_has_no_historical_detail,
        .correlation_id = "legacy-software-source-handoff",
        .declared_address = handoff.declared_address,
        .resolved_source = {
            .availability =
                selection_domain::ExternalHandoffFactAvailability::not_obtained,
            .not_obtained_reason =
                selection_domain::ExternalHandoffNotObtainedReason::
                    legacy_record_has_no_historical_detail,
        },
        .detail = handoff.detail,
    });
    if (!handoff.valid()) {
      return false;
    }
    handoffs.push_back(std::move(handoff));
  }
  return reader.complete();
}

[[nodiscard]] bool decode_current_machine(
    ByteReader& reader,
    std::vector<selection_domain::ExternalHandoffRecord>& handoffs) {
  std::uint32_t count{};
  if (!reader.u32(count) || count > 4096) {
    return false;
  }
  handoffs.clear();
  handoffs.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    selection_domain::ExternalHandoffRecord handoff;
    std::uint8_t encoded_status{};
    std::uint32_t fact_count{};
    if (!reader.text(handoff.software_id) ||
        !reader.text(handoff.declared_address) || !reader.u8(encoded_status) ||
        !reader.text(handoff.detail) || !reader.u32(fact_count) ||
        handoff.software_id.empty() || handoff.declared_address.empty() ||
        fact_count == 0 || fact_count > k_max_handoff_facts) {
      return false;
    }
    auto const status = decode_handoff_status(encoded_status);
    if (!status.has_value()) {
      return false;
    }
    handoff.status = *status;
    handoff.timeline.facts.reserve(fact_count);
    for (std::uint32_t fact_index = 0; fact_index < fact_count; ++fact_index) {
      selection_domain::ExternalHandoffFact fact;
      if (!read_handoff_fact(reader, fact)) {
        return false;
      }
      handoff.timeline.facts.push_back(std::move(fact));
    }
    if (!handoff.valid()) {
      return false;
    }
    handoffs.push_back(std::move(handoff));
  }
  return reader.complete();
}

[[nodiscard]] bool decode_machine(
    std::span<std::byte const> bytes,
    std::vector<selection_domain::ExternalHandoffRecord>& handoffs) {
  ByteReader reader{bytes};
  std::array<std::byte, k_machine_magic.size()> magic{};
  std::uint32_t version{};
  if (!reader.raw(magic) || magic != k_machine_magic || !reader.u32(version)) {
    return false;
  }
  if (version == k_legacy_machine_payload_version) {
    return decode_legacy_machine(reader, handoffs);
  }
  return version == k_machine_payload_version &&
         decode_current_machine(reader, handoffs);
}

[[nodiscard]] bool writable(StateReadMode mode) noexcept {
  return mode == StateReadMode::uninitialized || mode == StateReadMode::writable ||
         mode == StateReadMode::recovered_previous;
}

[[nodiscard]] bool readable(StateReadMode mode) noexcept {
  return mode == StateReadMode::writable ||
         mode == StateReadMode::recovered_previous;
}

[[nodiscard]] domain::StateKey subject_key(domain::StateSubject subject) {
  return domain::StateKey::for_subject(
      std::move(subject), domain::AggregateId{"software-selection"});
}

[[nodiscard]] domain::StateKey machine_key() {
  return domain::StateKey::machine(domain::AggregateId{"software-source-handoff"});
}

[[nodiscard]] bool is_successful_persistence(SelectionActionResult const& result) {
  return result.code == SelectionActionCode::succeeded;
}

}  // namespace

SoftwareSelectionLifecycle::SoftwareSelectionLifecycle(
    DeviceStateStore& states, Clock const& clock, ExecutionLog& log,
    architecture_selection::ArchitectureSelectionLifecycle& architectures,
    ControlledSourceResolver& resolver, NetworkObserver const& network,
    SoftwarePresenceDetector& detector, ExternalAddressLauncher& launcher,
    domain::StateSubject state_subject)
    : states_(states),
      clock_(clock),
      log_(log),
      architectures_(architectures),
      resolver_(resolver),
      network_(network),
      detector_(detector),
      launcher_(launcher),
      state_subject_(std::move(state_subject)) {}

SelectionActionResult SoftwareSelectionLifecycle::restore() {
  if (mode_ != SelectionLifecycleMode::not_restored) {
    return {.code = mode_ == SelectionLifecycleMode::ready
                        ? SelectionActionCode::succeeded
                        : SelectionActionCode::read_only,
            .message = error_};
  }

  auto const subject = states_.inspect(subject_key(state_subject_));
  auto const machine = states_.inspect(machine_key());
  subject_writable_ = writable(subject.mode);
  machine_writable_ = writable(machine.mode);

  if (readable(subject.mode) && subject.snapshot.has_value() &&
      !decode_subject(subject.snapshot->state.value.payload, selection_, sources_)) {
    mode_ = SelectionLifecycleMode::failed;
    error_ = "software selection state could not be decoded";
    log_event("restore", ExecutionResult::failed, {}, error_);
    return {.code = SelectionActionCode::rejected, .message = error_};
  }
  if (readable(machine.mode) && machine.snapshot.has_value() &&
      !decode_machine(machine.snapshot->state.value.payload, handoffs_)) {
    mode_ = SelectionLifecycleMode::failed;
    error_ = "software handoff state could not be decoded";
    log_event("restore", ExecutionResult::failed, {}, error_);
    return {.code = SelectionActionCode::rejected, .message = error_};
  }

  if (subject.snapshot.has_value()) {
    subject_revision_ = subject.snapshot->revision;
  }
  if (machine.snapshot.has_value()) {
    machine_revision_ = machine.snapshot->revision;
  }
  if (!subject_writable_ && !machine_writable_) {
    mode_ = SelectionLifecycleMode::read_only;
    error_ = "software selection state is read-only";
    log_event("restore", ExecutionResult::unknown, {}, error_);
    return {.code = SelectionActionCode::read_only, .message = error_};
  }

  mode_ = SelectionLifecycleMode::ready;
  log_event("restore", ExecutionResult::succeeded);
  return {.code = SelectionActionCode::succeeded};
}

SoftwareSelectionSnapshot SoftwareSelectionLifecycle::snapshot() const {
  std::vector<std::string> removed;
  std::vector<std::string> changed;
  std::vector<std::string> disabled;
  if (catalog_.has_value()) {
    for (auto const& item : impact_.items) {
      switch (item.reason) {
        case software_catalog::CatalogSelectionImpactReason::removed:
          removed.push_back(item.id);
          break;
        case software_catalog::CatalogSelectionImpactReason::
            execution_semantics_changed:
          changed.push_back(item.id);
          break;
        case software_catalog::CatalogSelectionImpactReason::runtime_unavailable:
          disabled.push_back(item.id);
          break;
        case software_catalog::CatalogSelectionImpactReason::added:
        case software_catalog::CatalogSelectionImpactReason::runtime_available:
          break;
      }
    }
    if (removed.empty()) {
      removed = impact_.removed;
    }
    if (changed.empty()) {
      changed = impact_.changed;
    }
    if (disabled.empty()) {
      disabled = impact_.disabled;
    }
  }
  return {
      .mode = mode_,
      .has_current_catalog = catalog_.has_value(),
      .subject_writable = subject_writable_,
      .machine_writable = machine_writable_,
      .selection = selection_,
      .items = catalog_.has_value()
                   ? selection_domain::project_selection(*catalog_, selection_,
                                                          removed, changed, disabled)
                   : std::vector<selection_domain::SelectionItem>{},
      .sources = sources_,
      .handoffs = handoffs_,
      .active_catalog = active_catalog_,
      .error = error_,
  };
}

SelectionActionResult SoftwareSelectionLifecycle::on_catalog_replaced(
    CatalogSelectionProjection projection) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  if (!projection_is_complete(projection)) {
    log_event("catalog-projection", ExecutionResult::failed, {},
              "catalog projection is incomplete or inconsistent");
    return {.code = SelectionActionCode::invalid_catalog_projection,
            .message = "catalog projection is incomplete or inconsistent"};
  }
  if (projection_is_stale(projection)) {
    log_event("catalog-projection", ExecutionResult::cancelled, {},
              "catalog projection is older than the active selection catalog");
    return {.code = SelectionActionCode::stale_catalog_projection,
            .message = "catalog projection is stale"};
  }
  catalog_ = std::move(projection.runtime);
  active_catalog_ = std::move(projection.active);
  impact_ = std::move(projection.impact);
  if (selection_.initialized) {
    return {.code = SelectionActionCode::succeeded};
  }
  auto const old_selection = selection_;
  selection_ = selection_domain::default_selection(*catalog_);
  auto persisted = persist_subject();
  if (!is_successful_persistence(persisted)) {
    selection_ = old_selection;
    return persisted;
  }
  log_event("default-selection", ExecutionResult::succeeded);
  return {.code = SelectionActionCode::succeeded, .state_changed = true};
}

SelectionActionResult SoftwareSelectionLifecycle::select(
    std::string_view software_id, bool selected) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  if (!catalog_.has_value()) {
    return {.code = SelectionActionCode::no_current_catalog,
            .message = "no current effective software catalog is loaded"};
  }
  auto const old_selection = selection_;
  auto change =
      selection_domain::change_selection(*catalog_, selection_, software_id, selected);
  if (!change.applied) {
    log_event("selection-rejected", ExecutionResult::cancelled, software_id,
              change.reason);
    return {.code = SelectionActionCode::rejected, .message = change.reason};
  }
  selection_ = std::move(change.state);
  auto persisted = persist_subject();
  if (!is_successful_persistence(persisted)) {
    selection_ = old_selection;
    return persisted;
  }
  log_event("selection-changed", ExecutionResult::succeeded, software_id,
            selected ? "selected" : "cleared");
  return {.code = SelectionActionCode::succeeded, .state_changed = true};
}

SelectionActionResult SoftwareSelectionLifecycle::resolve_declared_source(
    std::string_view software_id,
    catalog_domain::CatalogSource const& declared_source) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  if (!catalog_.has_value()) {
    return {.code = SelectionActionCode::no_current_catalog,
            .message = "no current effective software catalog is loaded"};
  }
  if (!declared_source.purpose.has_value() ||
      !selection_domain::is_declared_source(*catalog_, software_id,
                                            declared_source)) {
    return {.code = SelectionActionCode::source_not_declared,
            .message = "source is not declared by the current catalog"};
  }
  if (!network_.available()) {
    return {.code = SelectionActionCode::network_unavailable,
            .message = "source resolution requires an available network"};
  }
  auto resolved = resolver_.resolve(software_id, declared_source);
  if (!resolved.resolved || !resolved.snapshot.has_value()) {
    auto correlation = log_.begin_correlation();
    auto const detail = resolved.error.empty() ? "controlled resolver failed"
                                               : std::move(resolved.error);
    auto const existing = std::ranges::find(
        handoffs_, software_id,
        &selection_domain::ExternalHandoffRecord::software_id);
    auto recorded = append_handoff_fact(
        software_id,
        {.kind = selection_domain::ExternalHandoffFactKind::source_resolution_failed,
         .status = existing == handoffs_.end()
                       ? selection_domain::ExternalHandoffStatus::none
                       : existing->status,
         .occurred_at_milliseconds = now_milliseconds(),
         .correlation_id = correlation.value,
         .declared_address = declared_source.address,
         .resolved_source = {
             .availability =
                 selection_domain::ExternalHandoffFactAvailability::not_obtained,
             .not_obtained_reason =
                 selection_domain::ExternalHandoffNotObtainedReason::resolution_failed,
         },
         .detail = detail});
    if (!is_successful_persistence(recorded)) {
      return recorded;
    }
    log_event(correlation, "source-resolution", ExecutionResult::failed,
              software_id, detail);
    return {.code = SelectionActionCode::resolver_failed,
            .state_changed = true,
            .message = detail,
            .handoff = std::move(recorded.handoff)};
  }
  if (!resolved.snapshot->valid() ||
      !source_matches_catalog(*resolved.snapshot, declared_source)) {
    auto correlation = log_.begin_correlation();
    auto const detail = std::string{
        "resolver returned a snapshot outside the declared source"};
    auto const existing = std::ranges::find(
        handoffs_, software_id,
        &selection_domain::ExternalHandoffRecord::software_id);
    auto recorded = append_handoff_fact(
        software_id,
        {.kind = selection_domain::ExternalHandoffFactKind::source_resolution_failed,
         .status = existing == handoffs_.end()
                       ? selection_domain::ExternalHandoffStatus::none
                       : existing->status,
         .occurred_at_milliseconds = now_milliseconds(),
         .correlation_id = correlation.value,
         .declared_address = declared_source.address,
         .resolved_source = {
             .availability =
                 selection_domain::ExternalHandoffFactAvailability::not_obtained,
             .not_obtained_reason =
                 selection_domain::ExternalHandoffNotObtainedReason::resolution_failed,
         },
         .detail = detail});
    if (!is_successful_persistence(recorded)) {
      return recorded;
    }
    log_event(correlation, "source-resolution", ExecutionResult::failed,
              software_id, detail);
    return {.code = SelectionActionCode::invalid_resolution,
            .state_changed = true,
            .message = "resolver returned an invalid declared-source snapshot",
            .handoff = std::move(recorded.handoff)};
  }
  if (resolved.snapshot->resolved_at_milliseconds >
      std::chrono::duration_cast<std::chrono::milliseconds>(
          clock_.now().time_since_epoch()).count()) {
    return {.code = SelectionActionCode::invalid_resolution,
            .message = "resolver timestamp is in the future"};
  }
  auto const old_sources = sources_;
  auto const existing = std::ranges::find_if(
      sources_, [&](selection_domain::ResolvedSourceSnapshot const& source) {
        return source.software_id == resolved.snapshot->software_id &&
               source.declared_purpose == resolved.snapshot->declared_purpose &&
               source.declared_address == resolved.snapshot->declared_address;
      });
  if (existing == sources_.end()) {
    sources_.push_back(*resolved.snapshot);
  } else {
    *existing = *resolved.snapshot;
  }
  auto persisted = persist_subject();
  if (!is_successful_persistence(persisted)) {
    sources_ = old_sources;
    return persisted;
  }
  log_event("source-resolution", ExecutionResult::succeeded, software_id,
            "controlled snapshot persisted");
  return {.code = SelectionActionCode::succeeded,
          .state_changed = true,
          .resolved_source = std::move(resolved.snapshot)};
}

SelectionActionResult SoftwareSelectionLifecycle::evaluate_architecture(
    std::string_view software_id,
    selection_domain::ResolvedSourceSnapshot const& source) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  auto const persisted = std::ranges::find(sources_, source);
  if (!catalog_.has_value() || persisted == sources_.end() ||
      source.software_id != software_id) {
    return {.code = SelectionActionCode::invalid_resolution,
            .message = "architecture can use only a persisted source snapshot"};
  }
  auto const software = std::ranges::find(
      catalog_->software, software_id,
      [](catalog_domain::RuntimeSoftware const& item) {
        return item.definition.id;
      });
  if (software == catalog_->software.end()) {
    return {.code = SelectionActionCode::invalid_resolution,
            .message = "software is no longer in the current catalog"};
  }
  auto const declared = std::ranges::find_if(
      software->definition.sources,
      [&](catalog_domain::CatalogSource const& candidate) {
        return candidate.purpose.has_value() &&
               *candidate.purpose == source.declared_purpose &&
               candidate.address == source.declared_address;
      });
  if (declared == software->definition.sources.end() ||
      !source_matches_catalog(source, *declared)) {
    return {.code = SelectionActionCode::invalid_resolution,
            .message = "source snapshot is not declared by the current catalog"};
  }
  std::vector<architecture_selection::selection_domain::PackageCandidate>
      complete_packages;
  std::vector<architecture_selection::selection_domain::PackageCandidate>
      online_packages;
  for (auto const& package : source.packages) {
    if ((package.package_type == selection_domain::PackageType::full_package ||
         package.package_type == selection_domain::PackageType::archive_package) &&
        package.complete_package) {
      complete_packages.push_back(package.candidate);
    } else if (package.package_type ==
                   selection_domain::PackageType::online_installer &&
               network_.available()) {
      online_packages.push_back(package.candidate);
    }
  }
  auto candidates = !complete_packages.empty() ? std::move(complete_packages)
                                                : std::move(online_packages);
  if (candidates.empty()) {
    return {.code = SelectionActionCode::network_unavailable,
            .message = "no complete package is available and online packages require network"};
  }
  auto result = architectures_.evaluate(
      {.software_id = std::string{software_id}, .candidates = std::move(candidates)});
  return {.code = SelectionActionCode::succeeded, .architecture = std::move(result)};
}

SelectionActionResult SoftwareSelectionLifecycle::begin_external_handoff(
    std::string_view software_id,
    catalog_domain::CatalogSource const& declared_source) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  if (!catalog_.has_value()) {
    return {.code = SelectionActionCode::no_current_catalog,
            .message = "no current effective software catalog is loaded"};
  }
  if (!declared_source.purpose.has_value() ||
      !selection_domain::is_declared_source(*catalog_, software_id,
                                            declared_source)) {
    return {.code = SelectionActionCode::source_not_declared,
            .message = "source is not declared by the current catalog"};
  }
  std::string launcher_error;
  if (!launcher_.open_declared_address(software_id, declared_source,
                                       launcher_error)) {
    log_event("external-handoff", ExecutionResult::failed, software_id,
              launcher_error);
    return {.code = SelectionActionCode::launcher_failed,
            .message = launcher_error.empty() ? "could not open declared address"
                                              : std::move(launcher_error)};
  }
  auto correlation = log_.begin_correlation();
  auto recorded = append_handoff_fact(
      software_id,
      {.kind = selection_domain::ExternalHandoffFactKind::declared_address_opened,
       .status = selection_domain::ExternalHandoffStatus::
           waiting_for_external_install,
       .occurred_at_milliseconds = now_milliseconds(),
       .correlation_id = correlation.value,
       .declared_address = declared_source.address,
       .resolved_source = source_fact_for(software_id, declared_source),
       .detail = "declared address opened; installation remains external"});
  if (!is_successful_persistence(recorded)) {
    return recorded;
  }
  log_event(correlation, "external-handoff", ExecutionResult::started,
            software_id, "declared address opened; installation remains external");
  return recorded;
}

SelectionActionResult SoftwareSelectionLifecycle::detect_external_install(
    std::string_view software_id) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  auto existing = std::ranges::find(
      handoffs_, software_id, &selection_domain::ExternalHandoffRecord::software_id);
  if (existing == handoffs_.end() ||
      (existing->status != selection_domain::ExternalHandoffStatus::
                              waiting_for_external_install &&
       existing->status != selection_domain::ExternalHandoffStatus::skipped &&
       existing->status !=
           selection_domain::ExternalHandoffStatus::externally_recognized)) {
    return {.code = SelectionActionCode::rejected,
            .message = "no matching external handoff can be rechecked"};
  }
  auto const current_status = existing->status;
  auto const declared_address = existing->declared_address;
  auto detection = detector_.detect(software_id);
  auto correlation = log_.begin_correlation();
  if (!detection.completed) {
    auto const detail = detection.detail.empty() ? "presence detection failed"
                                                  : std::move(detection.detail);
    auto recorded = append_handoff_fact(
        software_id,
        {.kind = selection_domain::ExternalHandoffFactKind::returned_for_recheck,
         .status = current_status,
         .occurred_at_milliseconds = now_milliseconds(),
         .correlation_id = correlation.value,
         .declared_address = declared_address,
         .resolved_source = {
             .availability =
                 selection_domain::ExternalHandoffFactAvailability::not_obtained,
             .not_obtained_reason =
                 selection_domain::ExternalHandoffNotObtainedReason::
                     not_captured_for_this_fact,
         },
         .detail = detail});
    if (!is_successful_persistence(recorded)) {
      return recorded;
    }
    log_event(correlation, "external-detection", ExecutionResult::failed,
              software_id, detail);
    return {.code = SelectionActionCode::detector_failed,
            .state_changed = true,
            .message = detail,
            .handoff = std::move(recorded.handoff)};
  }
  auto const next_status =
      detection.present
          ? selection_domain::ExternalHandoffStatus::externally_recognized
          : (current_status == selection_domain::ExternalHandoffStatus::skipped
                 ? selection_domain::ExternalHandoffStatus::skipped
                 : selection_domain::ExternalHandoffStatus::
                       waiting_for_external_install);
  auto const detail = detection.detail.empty()
                          ? (detection.present
                                 ? "external installation was recognized"
                                 : "software was not detected")
                          : std::move(detection.detail);
  auto recorded = append_handoff_fact(
      software_id,
      {.kind = selection_domain::ExternalHandoffFactKind::returned_for_recheck,
       .status = next_status,
       .occurred_at_milliseconds = now_milliseconds(),
       .correlation_id = correlation.value,
       .declared_address = declared_address,
       .resolved_source = {
           .availability =
               selection_domain::ExternalHandoffFactAvailability::not_obtained,
           .not_obtained_reason =
               selection_domain::ExternalHandoffNotObtainedReason::
                   not_captured_for_this_fact,
       },
       .detail = detail});
  if (!is_successful_persistence(recorded)) {
    return recorded;
  }
  log_event(correlation, "external-detection",
            detection.present ? ExecutionResult::succeeded : ExecutionResult::unknown,
            software_id, detail);
  return {.code = SelectionActionCode::succeeded,
          .state_changed = true,
          .message = detection.present
                         ? "software was externally recognized"
                         : "software was not detected; external handoff remains pending",
          .handoff = std::move(recorded.handoff)};
}

SelectionActionResult SoftwareSelectionLifecycle::skip_external_handoff(
    std::string_view software_id) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  auto existing = std::ranges::find(
      handoffs_, software_id, &selection_domain::ExternalHandoffRecord::software_id);
  if (existing == handoffs_.end()) {
    return {.code = SelectionActionCode::rejected,
            .message = "no matching external handoff exists"};
  }
  auto correlation = log_.begin_correlation();
  auto recorded = append_handoff_fact(
      software_id,
      {.kind = selection_domain::ExternalHandoffFactKind::skipped,
       .status = selection_domain::ExternalHandoffStatus::skipped,
       .occurred_at_milliseconds = now_milliseconds(),
       .correlation_id = correlation.value,
       .declared_address = existing->declared_address,
       .resolved_source = {
           .availability =
               selection_domain::ExternalHandoffFactAvailability::not_obtained,
           .not_obtained_reason =
               selection_domain::ExternalHandoffNotObtainedReason::
                   not_captured_for_this_fact,
       },
       .detail = "skipped by the user"});
  if (!is_successful_persistence(recorded)) {
    return recorded;
  }
  log_event(correlation, "external-handoff-skipped", ExecutionResult::cancelled,
            software_id, "skipped by the user");
  return recorded;
}

SelectionActionResult SoftwareSelectionLifecycle::continue_external_handoff(
    std::string_view software_id) {
  if (mode_ != SelectionLifecycleMode::ready) {
    return {.code = mode_ == SelectionLifecycleMode::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::not_restored,
            .message = error_};
  }
  auto const existing = std::ranges::find(
      handoffs_, software_id, &selection_domain::ExternalHandoffRecord::software_id);
  if (existing == handoffs_.end() ||
      existing->status !=
          selection_domain::ExternalHandoffStatus::externally_recognized) {
    return {.code = SelectionActionCode::rejected,
            .message = "only an externally recognized handoff can continue"};
  }
  auto correlation = log_.begin_correlation();
  auto recorded = append_handoff_fact(
      software_id,
      {.kind = selection_domain::ExternalHandoffFactKind::continued,
       .status = selection_domain::ExternalHandoffStatus::externally_recognized,
       .occurred_at_milliseconds = now_milliseconds(),
       .correlation_id = correlation.value,
       .declared_address = existing->declared_address,
       .resolved_source = {
           .availability =
               selection_domain::ExternalHandoffFactAvailability::not_obtained,
           .not_obtained_reason =
               selection_domain::ExternalHandoffNotObtainedReason::
                   not_captured_for_this_fact,
       },
       .detail = "user continued after external recognition"});
  if (!is_successful_persistence(recorded)) {
    return recorded;
  }
  log_event(correlation, "external-handoff-continued", ExecutionResult::succeeded,
            software_id, "user continued after external recognition");
  return recorded;
}

SelectionActionResult SoftwareSelectionLifecycle::persist_subject() {
  if (!subject_writable_) {
    return {.code = SelectionActionCode::read_only,
            .message = "software selection state is read-only"};
  }
  auto const state = domain::DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode_subject(selection_, sources_)},
  };
  StateCommitResult result;
  if (subject_revision_.has_value()) {
    result = states_.commit({
        .key = subject_key(state_subject_),
        .expected_revision = *subject_revision_,
        .state = state,
    });
  } else {
    result = states_.initialize(subject_key(state_subject_), state);
  }
  if (result.status != StateCommitStatus::committed || !result.snapshot.has_value()) {
    return {.code = result.status == StateCommitStatus::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::persistence_failed,
            .message = result.error.empty() ? "could not persist software selection"
                                            : std::move(result.error)};
  }
  subject_revision_ = result.snapshot->revision;
  return {.code = SelectionActionCode::succeeded};
}

SelectionActionResult SoftwareSelectionLifecycle::persist_machine() {
  if (!machine_writable_) {
    return {.code = SelectionActionCode::read_only,
            .message = "software handoff state is read-only"};
  }
  auto const state = domain::DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode_machine(handoffs_)},
  };
  StateCommitResult result;
  if (machine_revision_.has_value()) {
    result = states_.commit({
        .key = machine_key(),
        .expected_revision = *machine_revision_,
        .state = state,
    });
  } else {
    result = states_.initialize(machine_key(), state);
  }
  if (result.status != StateCommitStatus::committed || !result.snapshot.has_value()) {
    return {.code = result.status == StateCommitStatus::read_only
                        ? SelectionActionCode::read_only
                        : SelectionActionCode::persistence_failed,
            .message = result.error.empty() ? "could not persist software handoff"
                                            : std::move(result.error)};
  }
  machine_revision_ = result.snapshot->revision;
  return {.code = SelectionActionCode::succeeded};
}

SelectionActionResult SoftwareSelectionLifecycle::append_handoff_fact(
    std::string_view software_id, selection_domain::ExternalHandoffFact fact) {
  if (!machine_writable_) {
    return {.code = SelectionActionCode::read_only,
            .message = "software handoff state is read-only"};
  }
  if (software_id.empty() || !fact.valid()) {
    return {.code = SelectionActionCode::rejected,
            .message = "external handoff fact is incomplete or invalid"};
  }

  auto const old_handoffs = handoffs_;
  auto existing = std::ranges::find(
      handoffs_, software_id, &selection_domain::ExternalHandoffRecord::software_id);
  if (existing == handoffs_.end()) {
    handoffs_.push_back({
        .software_id = std::string{software_id},
        .declared_address = fact.declared_address,
        .status = fact.status,
        .detail = fact.detail,
        .timeline = {.facts = {std::move(fact)}},
    });
    existing = std::prev(handoffs_.end());
  } else {
    // These fields are only a current projection.  The append below is the
    // history write; previously persisted facts remain bit-for-bit intact.
    existing->timeline.facts.push_back(std::move(fact));
    auto const& latest = existing->timeline.facts.back();
    existing->declared_address = latest.declared_address;
    existing->status = latest.status;
    existing->detail = latest.detail;
  }
  if (!existing->valid()) {
    handoffs_ = old_handoffs;
    return {.code = SelectionActionCode::rejected,
            .message = "external handoff fact would violate its timeline"};
  }
  auto persisted = persist_machine();
  if (!is_successful_persistence(persisted)) {
    handoffs_ = old_handoffs;
    return persisted;
  }
  return {.code = SelectionActionCode::succeeded,
          .state_changed = true,
          .handoff = *existing};
}

selection_domain::ExternalHandoffResolvedSourceFact
SoftwareSelectionLifecycle::source_fact_for(
    std::string_view software_id,
    catalog_domain::CatalogSource const& declared_source) const {
  auto const source = std::ranges::find_if(
      sources_, [&](selection_domain::ResolvedSourceSnapshot const& candidate) {
        return declared_source.purpose.has_value() &&
               candidate.software_id == software_id &&
               candidate.declared_purpose == *declared_source.purpose &&
               candidate.declared_address == declared_source.address;
      });
  if (source == sources_.end()) {
    return {
        .availability =
            selection_domain::ExternalHandoffFactAvailability::not_obtained,
        .not_obtained_reason =
            selection_domain::ExternalHandoffNotObtainedReason::
                no_persisted_resolved_source,
    };
  }
  return {
      .availability = selection_domain::ExternalHandoffFactAvailability::obtained,
      .not_obtained_reason =
          selection_domain::ExternalHandoffNotObtainedReason::none,
      .resolved_address = source->actual_address,
      .resolved_version = source->version,
      .resolver_capability_version = source->capability_version,
      .resolved_at_milliseconds = source->resolved_at_milliseconds,
  };
}

std::int64_t SoftwareSelectionLifecycle::now_milliseconds() const noexcept {
  auto const milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                clock_.now().time_since_epoch())
                                .count();
  return milliseconds > 0 ? milliseconds : 1;
}

bool SoftwareSelectionLifecycle::source_matches_catalog(
    selection_domain::ResolvedSourceSnapshot const& source,
    catalog_domain::CatalogSource const& declared_source) const noexcept {
  if (!catalog_.has_value() || !declared_source.purpose.has_value() ||
      source.software_id.empty() ||
      source.declared_purpose != *declared_source.purpose ||
      source.declared_address != declared_source.address ||
      !selection_domain::is_declared_source(*catalog_, source.software_id,
                                             declared_source)) {
    return false;
  }
  auto const software = std::ranges::find(
      catalog_->software, source.software_id,
      [](catalog_domain::RuntimeSoftware const& item) {
        return item.definition.id;
      });
  return software != catalog_->software.end() &&
         source.branch == software->definition.branch;
}

bool SoftwareSelectionLifecycle::projection_is_complete(
    CatalogSelectionProjection const& projection) const noexcept {
  auto const runtime_item_count =
      projection.runtime.software.size() + projection.runtime.drivers.size();
  return projection.runtime.schema_version != 0 &&
         projection.runtime.revision != 0 &&
         !projection.runtime.default_locale.empty() &&
         projection.active.revision == projection.runtime.revision &&
         projection.active.item_count == runtime_item_count &&
         !projection.active.content_identity.empty() &&
         !projection.active.application_id.empty();
}

bool SoftwareSelectionLifecycle::projection_is_stale(
    CatalogSelectionProjection const& projection) const noexcept {
  if (!active_catalog_.has_value()) {
    return false;
  }
  return projection.active.revision < active_catalog_->revision ||
         (projection.active.revision == active_catalog_->revision &&
          projection.active.content_identity != active_catalog_->content_identity);
}

std::vector<std::string> SoftwareSelectionLifecycle::impact_ids(
    software_catalog::CatalogSelectionImpactReason reason) const {
  std::vector<std::string> ids;
  for (auto const& item : impact_.items) {
    if (item.reason == reason) {
      ids.push_back(item.id);
    }
  }
  return ids;
}

void SoftwareSelectionLifecycle::log_event(CorrelationId const& correlation,
                                           std::string_view stage,
                                           ExecutionResult result,
                                           std::string_view software_id,
                                           std::string_view detail) {
  auto fields = std::vector<DiagnosticField>{};
  if (!software_id.empty()) {
    fields.push_back({.key = "software_id",
                      .value = std::string{software_id},
                      .disposition = DiagnosticValueDisposition::retain});
  }
  if (!detail.empty()) {
    fields.push_back({.key = "detail",
                      .value = std::string{detail},
                      .disposition = DiagnosticValueDisposition::sensitive});
  }
  static_cast<void>(log_.append(
      correlation,
      {.kind = ExecutionEventKind::state_transition,
       .component = "software-selection",
       .stage = std::string{stage},
       .result = result,
       .fields = std::move(fields)}));
}

void SoftwareSelectionLifecycle::log_event(std::string_view stage,
                                           ExecutionResult result,
                                           std::string_view software_id,
                                           std::string_view detail) {
  log_event(log_.begin_correlation(), stage, result, software_id, detail);
}

char const* to_string(SelectionLifecycleMode mode) noexcept {
  switch (mode) {
    case SelectionLifecycleMode::not_restored:
      return "not-restored";
    case SelectionLifecycleMode::ready:
      return "ready";
    case SelectionLifecycleMode::read_only:
      return "read-only";
    case SelectionLifecycleMode::failed:
      return "failed";
  }
  return "unknown";
}

char const* to_string(SelectionActionCode code) noexcept {
  switch (code) {
    case SelectionActionCode::succeeded:
      return "succeeded";
    case SelectionActionCode::no_current_catalog:
      return "no-current-catalog";
    case SelectionActionCode::not_restored:
      return "not-restored";
    case SelectionActionCode::rejected:
      return "rejected";
    case SelectionActionCode::invalid_catalog_projection:
      return "invalid-catalog-projection";
    case SelectionActionCode::stale_catalog_projection:
      return "stale-catalog-projection";
    case SelectionActionCode::resolver_failed:
      return "resolver-failed";
    case SelectionActionCode::network_unavailable:
      return "network-unavailable";
    case SelectionActionCode::source_not_declared:
      return "source-not-declared";
    case SelectionActionCode::invalid_resolution:
      return "invalid-resolution";
    case SelectionActionCode::persistence_failed:
      return "persistence-failed";
    case SelectionActionCode::read_only:
      return "read-only";
    case SelectionActionCode::detector_failed:
      return "detector-failed";
    case SelectionActionCode::launcher_failed:
      return "launcher-failed";
  }
  return "unknown";
}

}  // namespace azzs::application::software_selection
