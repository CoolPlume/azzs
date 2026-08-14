#include "azzs/application/driver_acquisition.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/execution_log.hpp"
#include "azzs/application/restart_resume.hpp"

namespace azzs::application::driver_acquisition {
namespace {

constexpr std::array<std::byte, 8> k_magic{
    std::byte{'A'}, std::byte{'Z'}, std::byte{'Z'}, std::byte{'S'},
    std::byte{'D'}, std::byte{'R'}, std::byte{'V'}, std::byte{'1'},
};
constexpr std::uint32_t k_format_version = 1;
constexpr std::uint8_t k_no_entrypoint = 0xff;

class Encoder final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(std::byte{value}); }

  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      u8(static_cast<std::uint8_t>(value >> shift));
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
    if (position_ == bytes_.size()) {
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

  [[nodiscard]] bool raw(std::span<std::byte> value) {
    if (value.size() > bytes_.size() - position_) {
      return false;
    }
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(position_),
                value.size(), value.begin());
    position_ += value.size();
    return true;
  }

  [[nodiscard]] bool complete() const noexcept { return position_ == bytes_.size(); }

 private:
  std::span<std::byte const> bytes_;
  std::size_t position_{};
};

[[nodiscard]] bool valid_entrypoint(std::uint8_t value) noexcept {
  return value <= static_cast<std::uint8_t>(DriverEntrypoint::asus_support);
}

[[nodiscard]] bool supported_entrypoint(DriverEntrypoint value) noexcept {
  switch (value) {
    case DriverEntrypoint::amd_software:
    case DriverEntrypoint::intel_driver_assistant:
    case DriverEntrypoint::nvidia_drivers:
    case DriverEntrypoint::dell_support:
    case DriverEntrypoint::hp_support:
    case DriverEntrypoint::lenovo_support:
    case DriverEntrypoint::asus_support: return true;
  }
  return false;
}

[[nodiscard]] bool supported_decision(DriverHandoffDecision value) noexcept {
  switch (value) {
    case DriverHandoffDecision::completed_externally:
    case DriverHandoffDecision::restart_required:
    case DriverHandoffDecision::skip_for_now: return true;
  }
  return false;
}

[[nodiscard]] bool valid_persisted_state(std::uint8_t value) noexcept {
  return value >= static_cast<std::uint8_t>(DriverAcquisitionState::ready) &&
         value <= static_cast<std::uint8_t>(
                      DriverAcquisitionState::waiting_for_restart);
}

[[nodiscard]] char const* hardware_state_text(HardwareOverviewState value) noexcept {
  switch (value) {
    case HardwareOverviewState::not_observed: return "not-observed";
    case HardwareOverviewState::observing: return "observing";
    case HardwareOverviewState::ready: return "ready";
    case HardwareOverviewState::unrecognized: return "unrecognized";
  }
  return "unknown";
}

[[nodiscard]] bool writable(StateReadMode mode) noexcept {
  return mode == StateReadMode::uninitialized || mode == StateReadMode::writable ||
         mode == StateReadMode::recovered_previous;
}

[[nodiscard]] bool readable(StateReadMode mode) noexcept {
  return mode == StateReadMode::writable || mode == StateReadMode::recovered_previous;
}

[[nodiscard]] std::string lowercase(std::string_view value) {
  std::string result{value};
  std::ranges::transform(result, result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

[[nodiscard]] bool contains(std::string_view haystack,
                            std::string_view needle) {
  return lowercase(haystack).find(needle) != std::string::npos;
}

[[nodiscard]] std::vector<DriverEntrypoint> recommendations_for(
    HardwareOverviewSnapshot const& hardware) {
  std::vector<DriverEntrypoint> result;
  if (hardware.state != HardwareOverviewState::ready ||
      !hardware.observation.has_value()) {
    return result;
  }
  auto const& facts = *hardware.observation;
  auto const append_if_missing = [&result](DriverEntrypoint entrypoint) {
    if (std::ranges::find(result, entrypoint) == result.end()) {
      result.push_back(entrypoint);
    }
  };
  if (contains(facts.cpu, "amd") || contains(facts.gpu, "amd")) {
    append_if_missing(DriverEntrypoint::amd_software);
  }
  if (contains(facts.cpu, "intel") || contains(facts.network_adapter, "intel")) {
    append_if_missing(DriverEntrypoint::intel_driver_assistant);
  }
  if (contains(facts.gpu, "nvidia")) {
    append_if_missing(DriverEntrypoint::nvidia_drivers);
  }
  if (contains(facts.oem_model, "dell")) {
    append_if_missing(DriverEntrypoint::dell_support);
  }
  if (contains(facts.oem_model, "hp")) {
    append_if_missing(DriverEntrypoint::hp_support);
  }
  if (contains(facts.oem_model, "lenovo")) {
    append_if_missing(DriverEntrypoint::lenovo_support);
  }
  if (contains(facts.oem_model, "asus")) {
    append_if_missing(DriverEntrypoint::asus_support);
  }
  return result;
}

[[nodiscard]] bool is_driver_restart_checkpoint(
    restart_resume::RestartResumeSnapshot const& snapshot) {
  if (!snapshot.checkpoint.has_value()) {
    return false;
  }
  return std::ranges::any_of(
      snapshot.checkpoint->participants, [](auto const& participant) {
        return participant.operation ==
               restart_resume::RestartResumeOperation::driver_acquisition;
      });
}

}  // namespace

class DriverAcquisitionService::Impl final {
 public:
  Impl(DeviceStateStore& states, HardwareOverviewService& hardware,
       DriverHandoffPlatform& platform, DriverNetworkObserver const& network,
       ExecutionLog& log, restart_resume::RestartResumeService& restart_resume)
      : states_(states),
        hardware_(hardware),
        platform_(platform),
        network_(network),
        log_(log),
        restart_resume_(restart_resume) {}

  [[nodiscard]] DriverActionResult restore() {
    std::scoped_lock lock{mutex_};
    if (state_ != DriverAcquisitionState::not_restored) {
      return result(state_ == DriverAcquisitionState::read_only
                        ? DriverActionCode::read_only
                        : DriverActionCode::succeeded);
    }

    auto read = states_.inspect(key_);
    if (read.mode == StateReadMode::uninitialized) {
      state_ = DriverAcquisitionState::ready;
      writable_ = true;
      revision_.reset();
      log_event("restore", ExecutionResult::succeeded);
      return result(DriverActionCode::succeeded);
    }
    if (!readable(read.mode) || !read.snapshot.has_value()) {
      state_ = DriverAcquisitionState::read_only;
      writable_ = false;
      detail_ = read.error.empty()
                    ? "driver handoff state is unavailable for safe recovery"
                    : std::move(read.error);
      log_event("restore", ExecutionResult::unknown, {}, detail_);
      return result(DriverActionCode::read_only, detail_);
    }
    if (!decode(read.snapshot->state.value.payload)) {
      state_ = DriverAcquisitionState::read_only;
      writable_ = false;
      detail_ = "driver handoff state has an unsupported or invalid format";
      log_event("restore", ExecutionResult::unknown, {}, detail_);
      return result(DriverActionCode::read_only, detail_);
    }

    writable_ = writable(read.mode);
    revision_ = read.snapshot->revision;
    auto const restart = restart_resume_.snapshot();
    if (state_ == DriverAcquisitionState::waiting_for_restart && restart.writable &&
        !is_driver_restart_checkpoint(restart)) {
      // A failed barrier rollback can leave the driver record at its previous
      // checkpoint if the companion write was unavailable. Once storage is
      // writable again, recover only to an explicit decision boundary.
      state_ = DriverAcquisitionState::awaiting_user_decision;
      detail_ = "the restart barrier is unavailable; an explicit user decision is required";
      if (!persist()) {
        state_ = DriverAcquisitionState::read_only;
        writable_ = false;
        log_event("restore-missing-restart-barrier", ExecutionResult::unknown,
                  active_entrypoint_, detail_);
        return result(DriverActionCode::persistence_failed, detail_);
      }
      log_event("restore-missing-restart-barrier", ExecutionResult::unknown,
                active_entrypoint_, detail_);
    }
    if (state_ == DriverAcquisitionState::handoff_in_progress) {
      // A browser or assistant has no reliable completion signal. A later
      // process therefore requires a new explicit user decision.
      state_ = DriverAcquisitionState::awaiting_user_decision;
      if (!persist()) {
        state_ = DriverAcquisitionState::read_only;
        writable_ = false;
        log_event("restore-return-boundary", ExecutionResult::unknown, {}, detail_);
        return result(DriverActionCode::persistence_failed, detail_);
      }
      log_event("restore-return-boundary", ExecutionResult::unknown,
                active_entrypoint_);
    }
    log_event("restore", ExecutionResult::succeeded);
    return result(DriverActionCode::succeeded);
  }

  [[nodiscard]] DriverAcquisitionSnapshot snapshot() const {
    std::scoped_lock lock{mutex_};
    return snapshot_unlocked();
  }

  [[nodiscard]] DriverActionResult begin_external_handoff(
      DriverEntrypoint entrypoint) {
    std::scoped_lock lock{mutex_};
    if (!ready()) {
      return result(state_ == DriverAcquisitionState::read_only
                        ? DriverActionCode::read_only
                        : DriverActionCode::not_restored);
    }
    if (state_ != DriverAcquisitionState::ready) {
      return result(DriverActionCode::rejected,
                    "a previous driver handoff still needs an explicit decision");
    }
    if (!supported_entrypoint(entrypoint)) {
      return result(DriverActionCode::rejected,
                    "the requested driver entrypoint is not product-controlled");
    }

    active_entrypoint_ = entrypoint;
    state_ = DriverAcquisitionState::handoff_in_progress;
    detail_.clear();
    if (!persist()) {
      state_ = DriverAcquisitionState::ready;
      active_entrypoint_.reset();
      return result(DriverActionCode::persistence_failed, detail_);
    }
    log_event("handoff-prepared", ExecutionResult::started, active_entrypoint_);

    auto const action = entrypoint == DriverEntrypoint::amd_software
                            ? (platform_.assistant_installed()
                                   ? DriverAssistantAction::launch
                                   : DriverAssistantAction::install)
                            : DriverAssistantAction::open_page;
    std::string error;
    if (!platform_.open(entrypoint, action, error)) {
      auto const failure = error.empty()
                               ? "the fixed external driver entrypoint could not open"
                               : std::move(error);
      // A failed shell handoff has no external flow to return from. Close the
      // durable pre-launch state before projecting the failure; if that close
      // cannot persist, fail closed as read-only instead of exposing a result
      // button for an operation that never started.
      state_ = DriverAcquisitionState::ready;
      active_entrypoint_.reset();
      if (!persist()) {
        state_ = DriverAcquisitionState::read_only;
        writable_ = false;
        return result(DriverActionCode::persistence_failed, detail_);
      }
      detail_ = failure;
      log_event("external-entrypoint-opened", ExecutionResult::failed, {}, detail_);
      return result(DriverActionCode::launcher_failed, detail_);
    }
    log_event("external-entrypoint-opened", ExecutionResult::started,
              active_entrypoint_);
    return result(DriverActionCode::succeeded);
  }

  [[nodiscard]] DriverActionResult external_flow_returned() {
    std::scoped_lock lock{mutex_};
    if (!ready()) {
      return result(state_ == DriverAcquisitionState::read_only
                        ? DriverActionCode::read_only
                        : DriverActionCode::not_restored);
    }
    if (state_ != DriverAcquisitionState::handoff_in_progress) {
      return result(DriverActionCode::rejected,
                    "only an active external handoff can return for a decision");
    }
    state_ = DriverAcquisitionState::awaiting_user_decision;
    detail_.clear();
    if (!persist()) {
      state_ = DriverAcquisitionState::handoff_in_progress;
      return result(DriverActionCode::persistence_failed, detail_);
    }
    log_event("external-returned", ExecutionResult::unknown, active_entrypoint_);
    return result(DriverActionCode::succeeded);
  }

  [[nodiscard]] DriverActionResult decide(DriverHandoffDecision decision) {
    std::scoped_lock lock{mutex_};
    if (!ready()) {
      return result(state_ == DriverAcquisitionState::read_only
                        ? DriverActionCode::read_only
                        : DriverActionCode::not_restored);
    }
    if (state_ != DriverAcquisitionState::awaiting_user_decision) {
      return result(DriverActionCode::rejected,
                    "driver result requires an explicit external-return decision");
    }
    if (!supported_decision(decision)) {
      return result(DriverActionCode::rejected,
                    "the requested driver handoff decision is not supported");
    }

    auto const restart = restart_resume_.snapshot();
    auto const owns_restart_barrier = is_driver_restart_checkpoint(restart);
    if (decision == DriverHandoffDecision::restart_required) {
      if (owns_restart_barrier) {
        return result(DriverActionCode::rejected,
                      "the current driver handoff already owns a restart barrier");
      }
      return arm_restart();
    }

    if (owns_restart_barrier) {
      if (restart.state != restart_resume::RestartResumeState::awaiting_user_decision) {
        return result(DriverActionCode::restart_barrier_failed,
                      "restart recovery has not reached its explicit user decision");
      }
      auto cleared = decision == DriverHandoffDecision::completed_externally
                         ? restart_resume_.confirm_continue()
                         : restart_resume_.cancel();
      if (!cleared.succeeded()) {
        return result(DriverActionCode::restart_barrier_failed,
                      cleared.message.empty()
                          ? "the restart barrier could not be cleared"
                          : std::move(cleared.message));
      }
    }

    if (decision == DriverHandoffDecision::skip_for_now) {
      state_ = DriverAcquisitionState::ready;
      active_entrypoint_.reset();
      detail_.clear();
      if (!persist()) {
        state_ = DriverAcquisitionState::awaiting_user_decision;
        return result(DriverActionCode::persistence_failed, detail_);
      }
      log_event("user-result-confirmation", ExecutionResult::cancelled, {},
                "user selected skip");
      return result(DriverActionCode::succeeded);
    }

    auto const hardware = hardware_.refresh();
    last_observation_ = DriverPostHandoffObservation{
        .hardware_state = hardware.state,
        .network_available = network_.available(),
    };
    state_ = DriverAcquisitionState::ready;
    active_entrypoint_.reset();
    detail_.clear();
    if (!persist()) {
      state_ = DriverAcquisitionState::awaiting_user_decision;
      return result(DriverActionCode::persistence_failed, detail_);
    }
    log_event("user-result-confirmation", ExecutionResult::unknown, {},
              "user reported external completion; driver success remains unverified",
              *last_observation_);
    auto action = result(DriverActionCode::succeeded);
    action.refreshed_hardware = hardware;
    return action;
  }

  [[nodiscard]] DriverActionResult recover_after_restart() {
    std::scoped_lock lock{mutex_};
    if (!ready()) {
      return result(state_ == DriverAcquisitionState::read_only
                        ? DriverActionCode::read_only
                        : DriverActionCode::not_restored);
    }
    if (state_ != DriverAcquisitionState::waiting_for_restart ||
        !is_driver_restart_checkpoint(restart_resume_.snapshot())) {
      return result(DriverActionCode::rejected,
                    "no persisted driver restart handoff is awaiting recovery");
    }
    state_ = DriverAcquisitionState::awaiting_user_decision;
    detail_.clear();
    if (!persist()) {
      state_ = DriverAcquisitionState::waiting_for_restart;
      return result(DriverActionCode::persistence_failed, detail_);
    }
    log_event("restart-read-only-recovery", ExecutionResult::unknown,
              active_entrypoint_);
    return result(DriverActionCode::succeeded);
  }

 private:
  [[nodiscard]] bool ready() const noexcept {
    return writable_ && state_ != DriverAcquisitionState::not_restored &&
           state_ != DriverAcquisitionState::read_only;
  }

  [[nodiscard]] bool persist() {
    auto bytes = encode();
    if (!bytes.has_value()) {
      detail_ = "driver handoff state exceeds its closed persistence format";
      return false;
    }
    domain::DeviceState state{
        .value = {.schema = 2,
                  .minimum_reader = 1,
                  .minimum_writer = 2,
                  .payload = std::move(*bytes)},
    };
    auto committed = revision_.has_value()
                         ? states_.commit({.key = key_,
                                           .expected_revision = *revision_,
                                           .state = std::move(state)})
                         : states_.initialize(key_, std::move(state));
    if (committed.status == StateCommitStatus::committed &&
        committed.snapshot.has_value()) {
      revision_ = committed.snapshot->revision;
      return true;
    }
    detail_ = committed.error.empty() ? "driver handoff state could not be persisted"
                                      : std::move(committed.error);
    return false;
  }

  [[nodiscard]] std::optional<domain::StateBytes> encode() const {
    if (state_ == DriverAcquisitionState::not_restored ||
        state_ == DriverAcquisitionState::read_only ||
        !valid_persisted_state(static_cast<std::uint8_t>(state_))) {
      return std::nullopt;
    }
    if ((state_ == DriverAcquisitionState::handoff_in_progress ||
         state_ == DriverAcquisitionState::awaiting_user_decision ||
         state_ == DriverAcquisitionState::waiting_for_restart) &&
        !active_entrypoint_.has_value()) {
      return std::nullopt;
    }
    Encoder encoder;
    encoder.raw(k_magic);
    encoder.u32(k_format_version);
    encoder.u8(static_cast<std::uint8_t>(state_));
    encoder.u8(active_entrypoint_.has_value()
                   ? static_cast<std::uint8_t>(*active_entrypoint_)
                   : k_no_entrypoint);
    encoder.u8(last_observation_.has_value() ? 1 : 0);
    if (last_observation_.has_value()) {
      encoder.u8(static_cast<std::uint8_t>(last_observation_->hardware_state));
      encoder.u8(last_observation_->network_available ? 1 : 0);
    }
    return encoder.finish();
  }

  [[nodiscard]] bool decode(domain::StateBytes const& bytes) {
    Decoder decoder{bytes};
    std::array<std::byte, k_magic.size()> magic{};
    std::uint32_t version{};
    std::uint8_t state{};
    std::uint8_t entrypoint{};
    std::uint8_t has_observation{};
    if (!decoder.raw(magic) || magic != k_magic || !decoder.u32(version) ||
        version != k_format_version || !decoder.u8(state) ||
        !valid_persisted_state(state) || !decoder.u8(entrypoint) ||
        (entrypoint != k_no_entrypoint && !valid_entrypoint(entrypoint)) ||
        !decoder.u8(has_observation) || has_observation > 1) {
      return false;
    }
    std::optional<DriverPostHandoffObservation> observation;
    if (has_observation == 1) {
      std::uint8_t hardware_state{};
      std::uint8_t network_available{};
      if (!decoder.u8(hardware_state) ||
          hardware_state > static_cast<std::uint8_t>(HardwareOverviewState::unrecognized) ||
          !decoder.u8(network_available) || network_available > 1) {
        return false;
      }
      observation = DriverPostHandoffObservation{
          .hardware_state = static_cast<HardwareOverviewState>(hardware_state),
          .network_available = network_available == 1,
      };
    }
    auto const decoded_state = static_cast<DriverAcquisitionState>(state);
    auto const active = entrypoint == k_no_entrypoint
                            ? std::optional<DriverEntrypoint>{}
                            : std::optional<DriverEntrypoint>{
                                  static_cast<DriverEntrypoint>(entrypoint)};
    if ((decoded_state == DriverAcquisitionState::handoff_in_progress ||
         decoded_state == DriverAcquisitionState::awaiting_user_decision ||
         decoded_state == DriverAcquisitionState::waiting_for_restart) !=
            active.has_value() ||
        !decoder.complete()) {
      return false;
    }
    state_ = decoded_state;
    active_entrypoint_ = active;
    last_observation_ = observation;
    detail_.clear();
    return true;
  }

  [[nodiscard]] DriverActionResult arm_restart() {
    state_ = DriverAcquisitionState::waiting_for_restart;
    detail_.clear();
    if (!persist()) {
      state_ = DriverAcquisitionState::awaiting_user_decision;
      return result(DriverActionCode::persistence_failed, detail_);
    }
    auto correlation = log_.begin_correlation();
    auto armed = restart_resume_.arm({
        .correlation_id = correlation.value,
        .participants = {{
            .operation = restart_resume::RestartResumeOperation::driver_acquisition,
            .operation_id = "driver-acquisition",
        }},
    });
    if (!armed.succeeded()) {
      auto const message = armed.message.empty()
                               ? "the restart barrier could not be armed"
                               : armed.message;
      if (is_driver_restart_checkpoint(armed.snapshot)) {
        // The shared barrier could not be atomically cleared. Keep this
        // service in the matching durable waiting state rather than presenting
        // a second external-return decision against a still-pending barrier.
        detail_ = message;
        log_event("restart-barrier", ExecutionResult::failed, active_entrypoint_,
                  message);
        return result(DriverActionCode::restart_barrier_failed, message);
      }
      state_ = DriverAcquisitionState::awaiting_user_decision;
      if (!persist()) {
        auto const persistence_error = detail_;
        state_ = DriverAcquisitionState::read_only;
        writable_ = false;
        detail_ = message + "; driver handoff rollback could not be persisted";
        if (!persistence_error.empty()) {
          detail_ += ": " + persistence_error;
        }
        log_event("restart-barrier", ExecutionResult::failed, active_entrypoint_,
                  detail_);
        return result(DriverActionCode::persistence_failed, detail_);
      }
      log_event("restart-barrier", ExecutionResult::failed, active_entrypoint_,
                message);
      return result(DriverActionCode::restart_barrier_failed, message);
    }
    log_event("restart-barrier", ExecutionResult::started, active_entrypoint_);
    return result(DriverActionCode::succeeded);
  }

  [[nodiscard]] DriverAcquisitionSnapshot snapshot_unlocked() const {
    return {
        .state = state_,
        .writable = writable_,
        .assistant_installed = platform_.assistant_installed(),
        .active_entrypoint = active_entrypoint_,
        .recommended_entrypoints = recommendations_for(hardware_.snapshot()),
        .last_observation = last_observation_,
        .detail = detail_,
    };
  }

  [[nodiscard]] DriverActionResult result(DriverActionCode code,
                                           std::string message = {}) const {
    return {.code = code,
            .snapshot = snapshot_unlocked(),
            .message = std::move(message)};
  }

  void log_event(std::string_view stage, ExecutionResult result,
                 std::optional<DriverEntrypoint> entrypoint = {},
                 std::string_view detail = {},
                 std::optional<DriverPostHandoffObservation> observation = {}) {
    std::vector<DiagnosticField> fields;
    if (entrypoint.has_value()) {
      fields.push_back({.key = "entrypoint",
                        .value = to_string(*entrypoint),
                        .disposition = DiagnosticValueDisposition::retain});
    }
    if (!detail.empty()) {
      fields.push_back({.key = "detail",
                        .value = std::string{detail},
                        .disposition = DiagnosticValueDisposition::retain});
    }
    if (observation.has_value()) {
      fields.push_back({.key = "hardware_observation",
                        .value = hardware_state_text(observation->hardware_state),
                        .disposition = DiagnosticValueDisposition::retain});
      fields.push_back({.key = "network_observation",
                        .value = observation->network_available ? "available" : "unavailable",
                        .disposition = DiagnosticValueDisposition::retain});
      fields.push_back({.key = "driver_success",
                        .value = "unverified",
                        .disposition = DiagnosticValueDisposition::retain});
    }
    auto correlation = log_.begin_correlation();
    static_cast<void>(log_.append(
        correlation, {.kind = ExecutionEventKind::state_transition,
                      .component = "driver-acquisition",
                      .stage = std::string{stage},
                      .result = result,
                      .fields = std::move(fields)}));
  }

  DeviceStateStore& states_;
  HardwareOverviewService& hardware_;
  DriverHandoffPlatform& platform_;
  DriverNetworkObserver const& network_;
  ExecutionLog& log_;
  restart_resume::RestartResumeService& restart_resume_;
  domain::StateKey key_ =
      domain::StateKey::machine(domain::AggregateId{"driver-acquisition"});
  std::optional<domain::RevisionToken> revision_;
  DriverAcquisitionState state_{DriverAcquisitionState::not_restored};
  bool writable_{false};
  std::optional<DriverEntrypoint> active_entrypoint_;
  std::optional<DriverPostHandoffObservation> last_observation_;
  std::string detail_;
  mutable std::mutex mutex_;
};

DriverAcquisitionService::DriverAcquisitionService(
    DeviceStateStore& states, HardwareOverviewService& hardware,
    DriverHandoffPlatform& platform, DriverNetworkObserver const& network,
    ExecutionLog& log, restart_resume::RestartResumeService& restart_resume)
    : impl_(std::make_unique<Impl>(states, hardware, platform, network, log,
                                   restart_resume)) {}

DriverAcquisitionService::~DriverAcquisitionService() = default;

DriverActionResult DriverAcquisitionService::restore() { return impl_->restore(); }

DriverAcquisitionSnapshot DriverAcquisitionService::snapshot() const {
  return impl_->snapshot();
}

DriverActionResult DriverAcquisitionService::begin_external_handoff(
    DriverEntrypoint entrypoint) {
  return impl_->begin_external_handoff(entrypoint);
}

DriverActionResult DriverAcquisitionService::external_flow_returned() {
  return impl_->external_flow_returned();
}

DriverActionResult DriverAcquisitionService::decide(DriverHandoffDecision decision) {
  return impl_->decide(decision);
}

DriverActionResult DriverAcquisitionService::recover_after_restart() {
  return impl_->recover_after_restart();
}

char const* to_string(DriverEntrypoint value) noexcept {
  switch (value) {
    case DriverEntrypoint::amd_software: return "amd-software";
    case DriverEntrypoint::intel_driver_assistant: return "intel-driver-assistant";
    case DriverEntrypoint::nvidia_drivers: return "nvidia-drivers";
    case DriverEntrypoint::dell_support: return "dell-support";
    case DriverEntrypoint::hp_support: return "hp-support";
    case DriverEntrypoint::lenovo_support: return "lenovo-support";
    case DriverEntrypoint::asus_support: return "asus-support";
  }
  return "unknown";
}

char const* to_string(DriverAcquisitionState value) noexcept {
  switch (value) {
    case DriverAcquisitionState::not_restored: return "not-restored";
    case DriverAcquisitionState::ready: return "ready";
    case DriverAcquisitionState::handoff_in_progress: return "handoff-in-progress";
    case DriverAcquisitionState::awaiting_user_decision: return "awaiting-user-decision";
    case DriverAcquisitionState::waiting_for_restart: return "waiting-for-restart";
    case DriverAcquisitionState::read_only: return "read-only";
  }
  return "unknown";
}

char const* to_string(DriverActionCode value) noexcept {
  switch (value) {
    case DriverActionCode::succeeded: return "succeeded";
    case DriverActionCode::not_restored: return "not-restored";
    case DriverActionCode::read_only: return "read-only";
    case DriverActionCode::rejected: return "rejected";
    case DriverActionCode::persistence_failed: return "persistence-failed";
    case DriverActionCode::launcher_failed: return "launcher-failed";
    case DriverActionCode::restart_barrier_failed: return "restart-barrier-failed";
  }
  return "unknown";
}

}  // namespace azzs::application::driver_acquisition
