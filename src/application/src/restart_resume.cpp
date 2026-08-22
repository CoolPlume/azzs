#include "azzs/application/restart_resume.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "azzs/application/device_state_store.hpp"

namespace azzs::application::restart_resume {
namespace {

constexpr std::array<std::byte, 8> k_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'R'}, std::byte{'S'}, std::byte{'0'}, std::byte{'1'},
};
constexpr std::uint32_t k_format_version = 1;
constexpr std::size_t k_max_text = 256;
constexpr std::size_t k_max_participants = 3;

class Encoder final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void text(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (auto character : value) {
      u8(static_cast<std::uint8_t>(character));
    }
  }

  void raw(std::span<std::byte const> value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  [[nodiscard]] domain::StateBytes finish() { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class Decoder final {
 public:
  explicit Decoder(std::span<std::byte const> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t& value) {
    if (position_ == bytes_.size()) return false;
    value = std::to_integer<std::uint8_t>(bytes_[position_++]);
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8) {
      std::uint8_t part{};
      if (!u8(part)) return false;
      value |= static_cast<std::uint32_t>(part) << shift;
    }
    return true;
  }

  [[nodiscard]] bool text(std::string& value) {
    std::uint32_t size{};
    if (!u32(size) || size > k_max_text || size > bytes_.size() - position_) {
      return false;
    }
    value.clear();
    value.reserve(size);
    for (std::uint32_t index = 0; index < size; ++index) {
      std::uint8_t character{};
      if (!u8(character)) return false;
      value.push_back(static_cast<char>(character));
    }
    return true;
  }

  [[nodiscard]] bool raw(std::span<std::byte> value) {
    if (value.size() > bytes_.size() - position_) return false;
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                value.size(), value.begin());
    position_ += value.size();
    return true;
  }

  [[nodiscard]] bool complete() const noexcept {
    return position_ == bytes_.size();
  }

 private:
  std::span<std::byte const> bytes_;
  std::size_t position_{};
};

[[nodiscard]] bool valid_text(std::string const& value) noexcept {
  return !value.empty() && value.size() <= k_max_text &&
         std::ranges::all_of(value, [](unsigned char character) {
           return character >= 0x20 && character < 0x7f;
         });
}

[[nodiscard]] bool valid_operation(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(
                      RestartResumeOperation::driver_acquisition);
}

[[nodiscard]] std::optional<domain::StateBytes> encode(
    RestartResumeSnapshot const& snapshot) {
  if (snapshot.state != RestartResumeState::idle &&
      !snapshot.checkpoint.has_value()) {
    return std::nullopt;
  }
  if (snapshot.checkpoint.has_value() && !snapshot.checkpoint->valid()) {
    return std::nullopt;
  }

  Encoder encoder;
  encoder.raw(k_magic);
  encoder.u32(k_format_version);
  encoder.u8(static_cast<std::uint8_t>(snapshot.state));
  encoder.u8(snapshot.login_resume_registered ? 1 : 0);
  encoder.u8(snapshot.checkpoint.has_value() ? 1 : 0);
  if (snapshot.checkpoint.has_value()) {
    encoder.text(snapshot.checkpoint->correlation_id);
    encoder.u8(static_cast<std::uint8_t>(snapshot.checkpoint->participants.size()));
    for (auto const& participant : snapshot.checkpoint->participants) {
      encoder.u8(static_cast<std::uint8_t>(participant.operation));
      encoder.text(participant.operation_id);
    }
  }
  return encoder.finish();
}

[[nodiscard]] std::optional<RestartResumeSnapshot> decode(
    domain::StateBytes const& bytes) {
  Decoder decoder{bytes};
  std::array<std::byte, k_magic.size()> magic{};
  std::uint32_t version{};
  std::uint8_t state{};
  std::uint8_t registered{};
  std::uint8_t has_checkpoint{};
  if (!decoder.raw(magic) || magic != k_magic || !decoder.u32(version) ||
      version != k_format_version || !decoder.u8(state) ||
      state > static_cast<std::uint8_t>(RestartResumeState::read_only) ||
      !decoder.u8(registered) || registered > 1 ||
      !decoder.u8(has_checkpoint) || has_checkpoint > 1) {
    return std::nullopt;
  }

  RestartResumeSnapshot snapshot{
      .state = static_cast<RestartResumeState>(state),
      .writable = true,
      .login_resume_registered = registered == 1,
  };
  if (has_checkpoint == 1) {
    RestartResumeRequest request;
    std::uint8_t participant_count{};
    if (!decoder.text(request.correlation_id) || !decoder.u8(participant_count) ||
        participant_count == 0 || participant_count > k_max_participants) {
      return std::nullopt;
    }
    request.participants.reserve(participant_count);
    for (std::uint8_t index = 0; index < participant_count; ++index) {
      std::uint8_t operation{};
      RestartResumeParticipant participant;
      if (!decoder.u8(operation) || !valid_operation(operation) ||
          !decoder.text(participant.operation_id)) {
        return std::nullopt;
      }
      participant.operation = static_cast<RestartResumeOperation>(operation);
      request.participants.push_back(std::move(participant));
    }
    snapshot.checkpoint = std::move(request);
  }
  if (!decoder.complete() ||
      (snapshot.state == RestartResumeState::idle) !=
          !snapshot.checkpoint.has_value() ||
      (snapshot.checkpoint.has_value() && !snapshot.checkpoint->valid())) {
    return std::nullopt;
  }
  return snapshot;
}

[[nodiscard]] bool registration_succeeded(LoginResumeRegistrationCode code) {
  return code == LoginResumeRegistrationCode::registered ||
         code == LoginResumeRegistrationCode::cleared;
}

}  // namespace

bool RestartResumeParticipant::valid() const noexcept {
  return valid_text(operation_id);
}

bool RestartResumeRequest::valid() const noexcept {
  if (!valid_text(correlation_id) || participants.empty() ||
      participants.size() > k_max_participants) {
    return false;
  }
  for (std::size_t index = 0; index < participants.size(); ++index) {
    if (!participants[index].valid()) return false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (participants[previous].operation == participants[index].operation) {
        return false;
      }
    }
  }
  return true;
}

class RestartResumeService::Impl final {
 public:
  Impl(DeviceStateStore& states, LoginResumeRegistration& registration)
      : states_(states), registration_(registration) {}

  [[nodiscard]] RestartResumeActionResult restore() {
    std::scoped_lock lock{mutex_};
    auto read = states_.inspect(key_);
    if (read.mode == StateReadMode::uninitialized) {
      snapshot_ = {.state = RestartResumeState::idle, .writable = true};
      revision_.reset();
      return result(RestartResumeActionCode::succeeded);
    }
    if ((read.mode != StateReadMode::writable &&
         read.mode != StateReadMode::recovered_previous) ||
        !read.snapshot.has_value()) {
      snapshot_ = {.state = RestartResumeState::read_only,
                   .writable = false,
                   .detail = read.error.empty()
                                 ? "restart-resume state is unavailable for safe recovery"
                                 : std::move(read.error)};
      revision_.reset();
      return result(RestartResumeActionCode::read_only, snapshot_.detail);
    }
    auto decoded = decode(read.snapshot->state.value.payload);
    if (!decoded.has_value()) {
      snapshot_ = {.state = RestartResumeState::read_only,
                   .writable = false,
                   .detail = "restart-resume state has an unsupported or invalid format"};
      revision_.reset();
      return result(RestartResumeActionCode::read_only, snapshot_.detail);
    }
    snapshot_ = std::move(*decoded);
    snapshot_.writable = true;
    revision_ = read.snapshot->revision;
    return result(RestartResumeActionCode::succeeded);
  }

  [[nodiscard]] RestartResumeSnapshot snapshot() const {
    std::scoped_lock lock{mutex_};
    return snapshot_;
  }

  [[nodiscard]] RestartResumeActionResult arm(RestartResumeRequest request) {
    std::scoped_lock lock{mutex_};
    if (!ready()) return result(RestartResumeActionCode::not_restored);
    if (!request.valid() || snapshot_.checkpoint.has_value()) {
      return result(RestartResumeActionCode::rejected,
                    "a valid restart checkpoint must be the only pending checkpoint");
    }
    auto const previous = snapshot_;
    snapshot_.state = RestartResumeState::waiting_for_windows_restart;
    snapshot_.checkpoint = std::move(request);
    snapshot_.login_resume_registered = false;
    if (!persist()) return result(RestartResumeActionCode::persistence_failed);

    auto registered = registration_.register_once();
    if (!registration_succeeded(registered.code)) {
      auto const message = registered.detail.empty()
                               ? "the next-login resume registration was not established"
                               : std::move(registered.detail);
      if (rollback_armed_checkpoint(previous, message)) {
        return result(RestartResumeActionCode::registration_failed, message);
      }
      return result(RestartResumeActionCode::registration_failed, snapshot_.detail);
    }
    snapshot_.login_resume_registered = true;
    if (!persist()) {
      auto const message = snapshot_.detail.empty()
                               ? "the restart registration state could not be persisted"
                               : snapshot_.detail;
      if (rollback_armed_checkpoint(previous, message)) {
        return result(RestartResumeActionCode::persistence_failed, message);
      }
      return result(RestartResumeActionCode::persistence_failed, snapshot_.detail);
    }
    snapshot_.detail.clear();
    return result(RestartResumeActionCode::succeeded);
  }

  [[nodiscard]] RestartResumeActionResult resume_after_login() {
    std::scoped_lock lock{mutex_};
    if (!ready()) return result(RestartResumeActionCode::not_restored);
    if (snapshot_.state != RestartResumeState::waiting_for_windows_restart ||
        !snapshot_.checkpoint.has_value()) {
      return result(RestartResumeActionCode::rejected,
                    "no restart checkpoint is waiting for a login recovery");
    }
    auto cleared = registration_.clear_once();
    if (!registration_succeeded(cleared.code)) {
      snapshot_.detail = cleared.detail.empty()
                             ? "the one-shot login resume registration could not be cleared"
                             : std::move(cleared.detail);
      return result(RestartResumeActionCode::registration_failed, snapshot_.detail);
    }
    snapshot_.state = RestartResumeState::awaiting_read_only_verification;
    snapshot_.login_resume_registered = false;
    snapshot_.detail.clear();
    return persist() ? result(RestartResumeActionCode::succeeded)
                     : result(RestartResumeActionCode::persistence_failed);
  }

  [[nodiscard]] RestartResumeActionResult complete_read_only_verification() {
    std::scoped_lock lock{mutex_};
    if (!ready()) return result(RestartResumeActionCode::not_restored);
    if (snapshot_.state != RestartResumeState::awaiting_read_only_verification) {
      return result(RestartResumeActionCode::rejected,
                    "read-only recovery must be completed before a user decision");
    }
    snapshot_.state = RestartResumeState::awaiting_user_decision;
    snapshot_.detail.clear();
    return persist() ? result(RestartResumeActionCode::succeeded)
                     : result(RestartResumeActionCode::persistence_failed);
  }

  [[nodiscard]] RestartResumeActionResult confirm_continue() {
    return clear_after_explicit_decision("continue");
  }

  [[nodiscard]] RestartResumeActionResult cancel() {
    return clear_after_explicit_decision("cancel");
  }

 private:
  [[nodiscard]] bool ready() const noexcept {
    return snapshot_.writable && snapshot_.state != RestartResumeState::read_only;
  }

  [[nodiscard]] bool persist() {
    auto bytes = encode(snapshot_);
    if (!bytes.has_value()) {
      snapshot_.detail = "restart-resume checkpoint exceeds its closed persistence format";
      return false;
    }
    domain::DeviceState state{
        .value = {.schema = 2, .minimum_reader = 1, .minimum_writer = 2,
                  .payload = std::move(*bytes)},
    };
    StateCommitResult committed = revision_.has_value()
        ? states_.commit({.key = key_, .expected_revision = *revision_, .state = std::move(state)})
        : states_.initialize(key_, std::move(state));
    if (committed.status == StateCommitStatus::committed &&
        committed.snapshot.has_value()) {
      revision_ = committed.snapshot->revision;
      return true;
    }
    snapshot_.detail = committed.error.empty()
                           ? "restart-resume checkpoint could not be persisted"
                           : std::move(committed.error);
    return false;
  }

  // `arm` writes a checkpoint before it asks the platform to register a
  // one-shot launch. A registration failure must not leave a bare checkpoint
  // behind. If cleanup or the rollback write itself fails, retain the pending
  // snapshot so the caller can stay fail-closed against the same barrier.
  [[nodiscard]] bool rollback_armed_checkpoint(
      RestartResumeSnapshot const& previous, std::string const& reason) {
    auto cleared = registration_.clear_once();
    if (!registration_succeeded(cleared.code)) {
      snapshot_.detail = reason + "; restart registration cleanup failed";
      return false;
    }
    auto const pending = snapshot_;
    snapshot_ = previous;
    snapshot_.detail = reason;
    if (persist()) {
      return true;
    }
    auto const rollback_error = snapshot_.detail;
    snapshot_ = pending;
    snapshot_.detail = rollback_error.empty()
                           ? reason + "; restart checkpoint rollback failed"
                           : std::move(rollback_error);
    return false;
  }

  [[nodiscard]] RestartResumeActionResult clear_after_explicit_decision(
      std::string_view decision) {
    std::scoped_lock lock{mutex_};
    if (!ready()) return result(RestartResumeActionCode::not_restored);
    if (snapshot_.state != RestartResumeState::awaiting_user_decision ||
        !snapshot_.checkpoint.has_value()) {
      return result(RestartResumeActionCode::rejected,
                    "an explicit decision is only available after read-only recovery");
    }
    snapshot_.state = RestartResumeState::idle;
    snapshot_.checkpoint.reset();
    snapshot_.login_resume_registered = false;
    snapshot_.detail.clear();
    if (!persist()) return result(RestartResumeActionCode::persistence_failed);
    return result(RestartResumeActionCode::succeeded, std::string{decision});
  }

  [[nodiscard]] RestartResumeActionResult result(
      RestartResumeActionCode code, std::string message = {}) const {
    return {.code = code, .snapshot = snapshot_, .message = std::move(message)};
  }

  DeviceStateStore& states_;
  LoginResumeRegistration& registration_;
  domain::StateKey key_ = domain::StateKey::machine(
      domain::AggregateId{"restart-resume"});
  std::optional<domain::RevisionToken> revision_;
  RestartResumeSnapshot snapshot_;
  mutable std::mutex mutex_;
};

RestartResumeService::RestartResumeService(
    DeviceStateStore& states, LoginResumeRegistration& registration)
    : impl_(std::make_unique<Impl>(states, registration)) {}

RestartResumeService::~RestartResumeService() = default;

RestartResumeActionResult RestartResumeService::restore() {
  return impl_->restore();
}

RestartResumeSnapshot RestartResumeService::snapshot() const {
  return impl_->snapshot();
}

RestartResumeActionResult RestartResumeService::arm(RestartResumeRequest request) {
  return impl_->arm(std::move(request));
}

RestartResumeActionResult RestartResumeService::resume_after_login() {
  return impl_->resume_after_login();
}

RestartResumeActionResult RestartResumeService::complete_read_only_verification() {
  return impl_->complete_read_only_verification();
}

RestartResumeActionResult RestartResumeService::confirm_continue() {
  return impl_->confirm_continue();
}

RestartResumeActionResult RestartResumeService::cancel() {
  return impl_->cancel();
}

char const* to_string(RestartResumeState value) noexcept {
  switch (value) {
    case RestartResumeState::idle: return "idle";
    case RestartResumeState::waiting_for_windows_restart: return "waiting-for-windows-restart";
    case RestartResumeState::awaiting_read_only_verification: return "awaiting-read-only-verification";
    case RestartResumeState::awaiting_user_decision: return "awaiting-user-decision";
    case RestartResumeState::read_only: return "read-only";
  }
  return "unknown";
}

char const* to_string(RestartResumeActionCode value) noexcept {
  switch (value) {
    case RestartResumeActionCode::succeeded: return "succeeded";
    case RestartResumeActionCode::not_restored: return "not-restored";
    case RestartResumeActionCode::rejected: return "rejected";
    case RestartResumeActionCode::persistence_failed: return "persistence-failed";
    case RestartResumeActionCode::registration_failed: return "registration-failed";
    case RestartResumeActionCode::read_only: return "read-only";
  }
  return "unknown";
}

}  // namespace azzs::application::restart_resume
