#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::domain {

using StateBytes = std::vector<std::byte>;

namespace detail {

[[nodiscard]] constexpr bool ascii_digit(char value) noexcept {
  return value >= '0' && value <= '9';
}

[[nodiscard]] constexpr bool ascii_lower(char value) noexcept {
  return value >= 'a' && value <= 'z';
}

[[nodiscard]] constexpr bool ascii_letter(char value) noexcept {
  return ascii_lower(value) || (value >= 'A' && value <= 'Z');
}

[[nodiscard]] constexpr char ascii_lowercase(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + 32) : value;
}

[[nodiscard]] inline bool reserved_windows_component(
    std::string_view value) noexcept {
  auto const dot = value.find('.');
  auto const stem = value.substr(0, dot);
  std::string normalized;
  normalized.reserve(stem.size());
  for (auto const character : stem) {
    normalized.push_back(ascii_lowercase(character));
  }
  if (normalized == "con" || normalized == "prn" || normalized == "aux" ||
      normalized == "nul") {
    return true;
  }
  if (normalized.size() == 4 &&
      (normalized.starts_with("com") || normalized.starts_with("lpt")) &&
      normalized[3] >= '1' && normalized[3] <= '9') {
    return true;
  }
  return false;
}

}  // namespace detail

struct AggregateId final {
  std::string value;

  [[nodiscard]] bool valid() const noexcept {
    if (value.empty() || value.size() > 64 || value == "." || value == ".." ||
        value.back() == '.' || detail::reserved_windows_component(value) ||
        !(detail::ascii_lower(value.front()) ||
          detail::ascii_digit(value.front()))) {
      return false;
    }
    for (auto const character : value) {
      if (!(detail::ascii_lower(character) || detail::ascii_digit(character) ||
            character == '_' || character == '-' || character == '.')) {
        return false;
      }
    }
    return true;
  }

  auto operator<=>(AggregateId const&) const = default;
};

struct StateSubject final {
  std::string value;

  [[nodiscard]] bool valid() const noexcept {
    if (value.empty() || value.size() > 184 || value == "." || value == ".." ||
        value.back() == '.' || detail::reserved_windows_component(value)) {
      return false;
    }
    for (auto const character : value) {
      if (!(detail::ascii_letter(character) || detail::ascii_digit(character) ||
            character == '_' || character == '-' || character == '.')) {
        return false;
      }
    }
    return true;
  }

  auto operator<=>(StateSubject const&) const = default;
};

enum class StatePartition : std::uint8_t {
  device = 1,
  subject = 2,
};

struct StateKey final {
  StatePartition partition{StatePartition::device};
  AggregateId aggregate;
  std::optional<StateSubject> subject;

  [[nodiscard]] static StateKey machine(AggregateId aggregate_id) {
    return StateKey{.partition = StatePartition::device,
                    .aggregate = std::move(aggregate_id)};
  }

  [[nodiscard]] static StateKey for_subject(StateSubject state_subject,
                                            AggregateId aggregate_id) {
    return StateKey{.partition = StatePartition::subject,
                    .aggregate = std::move(aggregate_id),
                    .subject = std::move(state_subject)};
  }

  [[nodiscard]] bool valid() const noexcept {
    return aggregate.valid() &&
           ((partition == StatePartition::device && !subject.has_value()) ||
            (partition == StatePartition::subject && subject.has_value() &&
             subject->valid()));
  }

  auto operator<=>(StateKey const&) const = default;
};

struct RevisionToken final {
  std::array<std::byte, 16> epoch{};
  std::uint64_t generation{};
  std::uint64_t content_digest{};

  auto operator<=>(RevisionToken const&) const = default;
};

// Schema 2 is the writer format. Schema 1 remains readable for the N-1
// compatibility window. A larger schema is preserved byte-for-byte and makes
// the aggregate read-only.
struct VersionedStateValue final {
  std::uint32_t schema{2};
  std::uint32_t minimum_reader{1};
  std::uint32_t minimum_writer{2};
  StateBytes payload;

  auto operator<=>(VersionedStateValue const&) const = default;
};

enum class DataImpactCategory : std::uint8_t {
  recovery_records = 1,
  workflow_records = 2,
  batch_progress = 3,
  catalog_state = 4,
  saved_drafts = 5,
  checkpoints = 6,
  preferences = 7,
  history = 8,
  emergency_withdrawals = 9,
};

struct ReinitializationAudit final {
  std::vector<DataImpactCategory> affected_categories;
  std::string confirmation_reference;
  std::int64_t confirmed_at_milliseconds{};

  auto operator<=>(ReinitializationAudit const&) const = default;
};

struct DeviceState final {
  VersionedStateValue value;
  std::vector<ReinitializationAudit> reinitializations;

  auto operator<=>(DeviceState const&) const = default;
};

struct DeviceStateSnapshot final {
  StateKey key;
  RevisionToken revision;
  DeviceState state;

  auto operator<=>(DeviceStateSnapshot const&) const = default;
};

}  // namespace azzs::domain
