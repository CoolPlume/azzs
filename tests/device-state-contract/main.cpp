#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/application/device_state_store.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

using namespace std::chrono_literals;
using azzs::application::DeviceStateStore;
using azzs::application::CheckpointStatus;
using azzs::application::StateCheckpoint;
using azzs::application::StateCommitRequest;
using azzs::application::StateCommitStatus;
using azzs::application::StateFileSlot;
using azzs::application::StateReadMode;
using azzs::application::StateReinitializeRequest;
using azzs::domain::AggregateId;
using azzs::domain::DataImpactCategory;
using azzs::domain::DeviceState;
using azzs::domain::StateBytes;
using azzs::domain::StateKey;
using azzs::domain::StateSubject;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::StateFileOperation;

[[nodiscard]] StateBytes bytes(std::string_view text) {
  StateBytes result;
  result.reserve(text.size());
  for (auto const character : text) {
    result.push_back(static_cast<std::byte>(character));
  }
  return result;
}

[[nodiscard]] bool contains_text(StateBytes const& source,
                                 std::string_view text) {
  auto const needle = bytes(text);
  return std::search(source.begin(), source.end(), needle.begin(), needle.end()) !=
         source.end();
}

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "device state contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] DeviceState state(std::string_view payload,
                                std::uint32_t schema = 2) {
  return DeviceState{
      .value = {.schema = schema,
                .minimum_reader = 1,
                .minimum_writer = schema,
                .payload = bytes(payload)},
  };
}

[[nodiscard]] std::uint32_t read_u32(StateBytes const& source,
                                     std::size_t position) {
  std::uint32_t value{};
  for (unsigned shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(
                 std::to_integer<std::uint8_t>(source[position++]))
             << shift;
  }
  return value;
}

void write_u32(StateBytes& target, std::size_t position,
               std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    target[position++] =
        std::byte{static_cast<std::uint8_t>(value >> shift)};
  }
}

void write_u64(StateBytes& target, std::size_t position,
               std::uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8) {
    target[position++] =
        std::byte{static_cast<std::uint8_t>(value >> shift)};
  }
}

[[nodiscard]] std::uint64_t fixture_digest(
    std::span<std::byte const> source) {
  std::uint64_t value{};
  for (auto const byte : source) {
    value ^= static_cast<std::uint64_t>(
                 std::to_integer<std::uint8_t>(byte))
             << 56;
    for (unsigned bit = 0; bit < 8; ++bit) {
      value = (value & 0x8000000000000000ULL) != 0
                  ? (value << 1) ^ 0x42f0e1eba9ea3693ULL
                  : value << 1;
    }
  }
  return value;
}

[[nodiscard]] StateBytes as_future_schema(StateBytes encoded) {
  std::size_t position = 8 + 4 + 1;
  auto const aggregate_size = read_u32(encoded, position);
  position += 4 + aggregate_size;
  auto const subject_size = read_u32(encoded, position);
  position += 4 + subject_size;
  write_u32(encoded, position, 3);
  auto const content_size = encoded.size() - sizeof(std::uint64_t);
  write_u64(encoded, content_size,
            fixture_digest(std::span<std::byte const>{encoded}.first(
                content_size)));
  return encoded;
}

[[nodiscard]] StateBytes as_future_envelope(StateBytes encoded) {
  write_u32(encoded, 8, 2);
  encoded.back() ^= std::byte{0x5a};
  return encoded;
}

[[nodiscard]] StateBytes with_generation(StateBytes encoded,
                                         std::uint64_t generation) {
  std::size_t position = 8 + 4 + 1;
  auto const aggregate_size = read_u32(encoded, position);
  position += 4 + aggregate_size;
  auto const subject_size = read_u32(encoded, position);
  position += 4 + subject_size;
  position += 4 + 4 + 4 + 16;
  write_u64(encoded, position, generation);
  auto const content_size = encoded.size() - sizeof(std::uint64_t);
  write_u64(encoded, content_size,
            fixture_digest(std::span<std::byte const>{encoded}.first(
                content_size)));
  return encoded;
}

struct Fixture final {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{1234ms}};
  DeviceStateStore store{files, clock};
};

[[nodiscard]] bool first_and_continuous_commit_keep_n_minus_one() {
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{1234ms}};
  DeviceStateStore store{files, clock};
  auto const key = StateKey::machine(AggregateId{"recovery-records"});

  bool passed = true;
  passed &= expect(store.inspect(key).mode == StateReadMode::uninitialized,
                   "an empty aggregate must be explicitly uninitialized");

  auto first = state("first");
  auto const initialized = store.initialize(key, first);
  passed &= expect(initialized.status == StateCommitStatus::committed,
                   "the first authoritative generation must commit");
  passed &= expect(initialized.snapshot.has_value(),
                   "a successful first commit must return its snapshot");
  if (!initialized.snapshot.has_value()) {
    return false;
  }
  passed &= expect(initialized.snapshot->revision.generation == 1,
                   "the first generation must be one");

  auto second = state("second");
  auto const committed = store.commit(StateCommitRequest{
      .key = key,
      .expected_revision = initialized.snapshot->revision,
      .state = second,
  });
  passed &= expect(committed.status == StateCommitStatus::committed,
                   "a matching revision must commit continuously");
  passed &= expect(committed.snapshot.has_value(),
                   "a continuous commit must return its snapshot");
  if (committed.snapshot.has_value()) {
    passed &= expect(committed.snapshot->revision.generation == 2,
                     "a continuous commit must increment the generation");
    passed &= expect(committed.snapshot->state.value.payload == bytes("second"),
                     "the current generation must expose the new payload");
  }

  files.corrupt(key, azzs::application::StateFileSlot::current);
  auto const recovered = store.inspect(key);
  passed &= expect(recovered.mode == StateReadMode::recovered_previous,
                   "a damaged current generation must fall back explicitly");
  passed &= expect(recovered.snapshot.has_value(),
                   "a valid N-1 generation must remain readable");
  if (recovered.snapshot.has_value()) {
    passed &= expect(recovered.snapshot->state.value.payload == bytes("first"),
                     "N-1 must be the last authoritative payload");
  }

  return passed;
}

[[nodiscard]] bool schema_window_and_future_bytes_are_safe() {
  bool passed = true;
  Fixture compatible;
  auto const compatible_key =
      StateKey::machine(AggregateId{"schema-window"});
  auto const previous_format =
      compatible.store.initialize(compatible_key, state("v1", 1));
  passed &= expect(previous_format.status == StateCommitStatus::committed,
                   "schema N-1 must remain readable");
  auto const read_previous = compatible.store.inspect(compatible_key);
  passed &= expect(read_previous.mode == StateReadMode::writable &&
                       read_previous.snapshot.has_value() &&
                       read_previous.snapshot->state.value.schema == 1,
                   "schema 1 must load inside the N/N-1 window");
  if (read_previous.snapshot.has_value()) {
    auto const migrated = compatible.store.commit(StateCommitRequest{
        .key = compatible_key,
        .expected_revision = read_previous.snapshot->revision,
        .state = state("v2", 2),
    });
    passed &= expect(migrated.status == StateCommitStatus::committed &&
                         migrated.snapshot.has_value() &&
                         migrated.snapshot->state.value.schema == 2,
                     "a schema 1 aggregate must commit schema 2");
  }

  Fixture future;
  auto const future_key = StateKey::machine(AggregateId{"future-schema"});
  auto initialized = future.store.initialize(future_key, state("old"));
  if (!initialized.snapshot.has_value()) {
    return expect(false, "future-schema fixture must initialize");
  }
  auto advanced = future.store.commit(StateCommitRequest{
      .key = future_key,
      .expected_revision = initialized.snapshot->revision,
      .state = state("new"),
  });
  if (!advanced.snapshot.has_value()) {
    return expect(false, "future-schema fixture must create N-1");
  }
  auto raw = future.files.raw_file(future_key, StateFileSlot::current);
  if (!raw.has_value()) {
    return expect(false, "future-schema fixture must expose current bytes");
  }
  auto future_bytes = as_future_schema(*raw);
  future.files.seed(future_key, StateFileSlot::current, future_bytes);

  auto const future_read = future.store.inspect(future_key);
  passed &= expect(future_read.mode == StateReadMode::read_only_future,
                   "a valid future current must not fall back to writable N-1");
  auto const rejected_commit = future.store.commit(StateCommitRequest{
      .key = future_key,
      .expected_revision = advanced.snapshot->revision,
      .state = state("must-not-write"),
  });
  passed &= expect(rejected_commit.status == StateCommitStatus::read_only,
                   "future state must reject ordinary writes");
  auto const rejected_reset = future.store.reinitialize(
      StateReinitializeRequest{
          .key = future_key,
          .replacement = state("must-not-reset").value,
          .affected_categories = {DataImpactCategory::history},
          .confirmation_reference = "confirmation/future",
      });
  passed &= expect(rejected_reset.status == StateCommitStatus::read_only,
                   "future state must reject reinitialization by an old writer");
  passed &= expect(future.files.raw_file(future_key, StateFileSlot::current) ==
                       std::optional<StateBytes>{future_bytes},
                   "future bytes must remain exactly unchanged");

  Fixture future_previous;
  auto const previous_key =
      StateKey::machine(AggregateId{"future-previous-schema"});
  auto previous_first =
      future_previous.store.initialize(previous_key, state("first"));
  if (!previous_first.snapshot.has_value()) {
    return expect(false, "future previous fixture must initialize");
  }
  auto previous_current = future_previous.store.commit(StateCommitRequest{
      .key = previous_key,
      .expected_revision = previous_first.snapshot->revision,
      .state = state("second"),
  });
  auto raw_previous =
      future_previous.files.raw_file(previous_key, StateFileSlot::previous);
  if (!previous_current.snapshot.has_value() || !raw_previous.has_value()) {
    return expect(false, "future previous fixture must create both generations");
  }
  auto opaque_previous = as_future_schema(*raw_previous);
  future_previous.files.seed(previous_key, StateFileSlot::previous,
                             opaque_previous);
  auto const protected_current =
      future_previous.files.raw_file(previous_key, StateFileSlot::current);
  auto const previous_read = future_previous.store.inspect(previous_key);
  auto const previous_commit = future_previous.store.commit(StateCommitRequest{
      .key = previous_key,
      .expected_revision = previous_current.snapshot->revision,
      .state = state("must-not-overwrite-future-previous"),
  });
  passed &= expect(previous_read.mode == StateReadMode::read_only_future &&
                       previous_commit.status == StateCommitStatus::read_only,
                   "an opaque future N-1 must protect the whole aggregate from writes");
  passed &= expect(
      future_previous.files.raw_file(previous_key, StateFileSlot::previous) ==
              std::optional<StateBytes>{opaque_previous} &&
          future_previous.files.raw_file(previous_key, StateFileSlot::current) ==
              protected_current,
      "future N-1 and supported current bytes must both remain unchanged");

  Fixture future_envelope;
  auto const envelope_key =
      StateKey::machine(AggregateId{"future-envelope"});
  auto envelope_first =
      future_envelope.store.initialize(envelope_key, state("first"));
  auto envelope_current = future_envelope.store.commit(StateCommitRequest{
      .key = envelope_key,
      .expected_revision = envelope_first.snapshot->revision,
      .state = state("second"),
  });
  auto envelope_bytes = future_envelope.files.raw_file(
      envelope_key, StateFileSlot::current);
  if (!envelope_current.snapshot.has_value() || !envelope_bytes.has_value()) {
    return expect(false, "future envelope fixture must create both generations");
  }
  auto opaque_envelope = as_future_envelope(*envelope_bytes);
  future_envelope.files.seed(envelope_key, StateFileSlot::current,
                             opaque_envelope);
  auto envelope_read = future_envelope.store.inspect(envelope_key);
  auto envelope_commit = future_envelope.store.commit(StateCommitRequest{
      .key = envelope_key,
      .expected_revision = envelope_current.snapshot->revision,
      .state = state("must-not-overwrite-future-envelope"),
  });
  passed &= expect(
      envelope_read.mode == StateReadMode::read_only_future &&
          envelope_commit.status == StateCommitStatus::read_only &&
          future_envelope.files.raw_file(envelope_key, StateFileSlot::current) ==
              std::optional<StateBytes>{opaque_envelope},
      "a future envelope must remain opaque and read-only even when its checksum format changed");

  return passed;
}

[[nodiscard]] bool dual_corruption_requires_audited_reinitialization() {
  Fixture fixture;
  auto const key = StateKey::machine(AggregateId{"corrupt-state"});
  bool passed = true;
  auto first = fixture.store.initialize(
      key, state("first-corrupt-evidence-marker"));
  if (!first.snapshot.has_value()) {
    return expect(false, "corruption fixture must initialize");
  }
  auto second = fixture.store.commit(StateCommitRequest{
      .key = key,
      .expected_revision = first.snapshot->revision,
      .state = state("second-corrupt-evidence-marker"),
  });
  if (!second.snapshot.has_value()) {
    return expect(false, "corruption fixture must create N-1");
  }
  auto const old_epoch = second.snapshot->revision.epoch;
  fixture.files.corrupt(key, StateFileSlot::current);
  fixture.files.corrupt(key, StateFileSlot::previous);
  auto corrupt_current = fixture.files.raw_file(key, StateFileSlot::current);
  auto corrupt_previous = fixture.files.raw_file(key, StateFileSlot::previous);
  auto const corrupt_candidate = bytes("candidate-residual");
  auto const corrupt_intent = bytes("intent-residual");
  auto const corrupt_staging = bytes("previous-staging-residual");
  fixture.files.seed(key, StateFileSlot::candidate, corrupt_candidate);
  fixture.files.seed(key, StateFileSlot::intent, corrupt_intent);
  fixture.files.seed(key, StateFileSlot::previous_staging, corrupt_staging);

  auto const damaged = fixture.store.inspect(key);
  passed &= expect(damaged.mode == StateReadMode::read_only_corrupt,
                   "dual corruption must enter explicit read-only recovery");
  auto const blocked = fixture.store.commit(StateCommitRequest{
      .key = key,
      .expected_revision = second.snapshot->revision,
      .state = state("blocked"),
  });
  passed &= expect(blocked.status == StateCommitStatus::read_only,
                   "dual corruption must reject all ordinary mutations");

  fixture.files.fail_on({.operation = StateFileOperation::read,
                         .slot = StateFileSlot::corrupt_archive,
                         .occurrence = 2});
  auto unverified_archive = fixture.store.reinitialize(
      StateReinitializeRequest{
          .key = key,
          .replacement = state("must-not-publish-unverified-archive").value,
          .affected_categories = {DataImpactCategory::history},
          .confirmation_reference = "confirmation/archive-reread-failure",
      });
  passed &= expect(
      unverified_archive.status == StateCommitStatus::failed &&
          unverified_archive.failed_stage == "corrupt-archive-reread" &&
          fixture.store.inspect(key).mode == StateReadMode::read_only_corrupt,
      "reinitialization must remain read-only until the appended corruption archive rereads and validates");

  fixture.clock.advance(1ms);
  auto reset = fixture.store.reinitialize(StateReinitializeRequest{
      .key = key,
      .replacement = state("first-reinitialized-authority-marker").value,
      .affected_categories = {DataImpactCategory::recovery_records,
                              DataImpactCategory::batch_progress},
      .confirmation_reference = "confirmation/reinitialize-42",
  });
  passed &= expect(reset.status == StateCommitStatus::committed &&
                       reset.snapshot.has_value(),
                   "explicit impact and confirmation must allow reinitialization");
  if (reset.snapshot.has_value()) {
    passed &= expect(reset.snapshot->revision.epoch != old_epoch,
                     "reinitialization must start a new revision epoch");
    passed &= expect(reset.snapshot->state.value.payload ==
                         bytes("first-reinitialized-authority-marker"),
                     "reinitialization must create the requested clean state");
    passed &= expect(
        reset.snapshot->state.reinitializations.size() == 1 &&
            reset.snapshot->state.reinitializations[0]
                    .confirmation_reference ==
                "confirmation/reinitialize-42" &&
            reset.snapshot->state.reinitializations[0]
                    .affected_categories.size() == 2,
        "reinitialization must embed impact and confirmation audit facts");
  }
  passed &= expect(
      fixture.files.raw_file(key, StateFileSlot::corrupt_current) ==
          corrupt_current,
      "reinitialization must archive the corrupt authoritative bytes");
  passed &= expect(
      fixture.files.raw_file(key, StateFileSlot::corrupt_previous) ==
          corrupt_previous,
      "reinitialization must archive the corrupt last trusted bytes");
  passed &= expect(
      fixture.files.raw_file(key, StateFileSlot::corrupt_candidate) ==
              std::optional<StateBytes>{corrupt_candidate} &&
          fixture.files.raw_file(key, StateFileSlot::corrupt_intent) ==
              std::optional<StateBytes>{corrupt_intent} &&
          fixture.files.raw_file(key,
                                 StateFileSlot::corrupt_previous_staging) ==
              std::optional<StateBytes>{corrupt_staging},
      "reinitialization must archive every transaction residual before reuse");

  if (reset.snapshot.has_value()) {
    auto erased_audit = fixture.store.commit(StateCommitRequest{
        .key = key,
        .expected_revision = reset.snapshot->revision,
        .state = state("erase-audit"),
    });
    passed &= expect(
        erased_audit.status == StateCommitStatus::failed,
        "ordinary commits must not erase reinitialization audit facts");
    auto preserved = reset.snapshot->state;
    preserved.value.payload = bytes("second-lineage-current-marker");
    auto preserved_commit = fixture.store.commit(StateCommitRequest{
        .key = key,
        .expected_revision = reset.snapshot->revision,
        .state = std::move(preserved),
    });
    passed &= expect(
        preserved_commit.status == StateCommitStatus::committed &&
            preserved_commit.snapshot.has_value() &&
            preserved_commit.snapshot->state.reinitializations.size() == 1,
        "ordinary commits may continue only while preserving the audit trail");
    if (preserved_commit.snapshot.has_value()) {
      fixture.files.corrupt(key, StateFileSlot::current);
      fixture.files.corrupt(key, StateFileSlot::previous);
      fixture.clock.advance(1ms);
      auto second_reset = fixture.store.reinitialize(StateReinitializeRequest{
          .key = key,
          .replacement = state("second-reinitialized-authority").value,
          .affected_categories = {DataImpactCategory::history},
          .confirmation_reference = "confirmation/reinitialize-again",
      });
      auto archive =
          fixture.files.raw_file(key, StateFileSlot::corrupt_archive);
      passed &= expect(
          second_reset.status == StateCommitStatus::committed &&
              archive.has_value() &&
              contains_text(*archive, "first-corrupt-evidence-marker") &&
              contains_text(*archive, "second-corrupt-evidence-marker") &&
              contains_text(*archive,
                            "first-reinitialized-authority-marker") &&
              contains_text(*archive, "second-lineage-current-marker"),
          "each reinitialization must append raw corruption evidence without overwriting earlier generations");
    }
  }

  return passed;
}

[[nodiscard]] bool invalid_generation_order_preserves_both_generations() {
  Fixture fixture;
  auto const key = StateKey::machine(AggregateId{"invalid-generation-order"});
  auto first = fixture.store.initialize(
      key, state("ordering-previous-evidence-marker"));
  if (!first.snapshot.has_value()) {
    return expect(false, "generation ordering fixture must initialize");
  }
  auto second = fixture.store.commit(StateCommitRequest{
      .key = key,
      .expected_revision = first.snapshot->revision,
      .state = state("ordering-current-evidence-marker"),
  });
  auto previous = fixture.files.raw_file(key, StateFileSlot::previous);
  if (!second.snapshot.has_value() || !previous.has_value()) {
    return expect(false, "generation ordering fixture must create N-1");
  }
  fixture.files.seed(key, StateFileSlot::previous,
                     with_generation(*previous, 3));
  auto damaged = fixture.store.inspect(key);
  auto reset = fixture.store.reinitialize(StateReinitializeRequest{
      .key = key,
      .replacement = state("ordering-reset").value,
      .affected_categories = {DataImpactCategory::history},
      .confirmation_reference = "confirmation/generation-order",
  });
  auto archive = fixture.files.raw_file(key, StateFileSlot::corrupt_archive);
  bool passed = expect(
      damaged.mode == StateReadMode::read_only_corrupt &&
          reset.status == StateCommitStatus::committed && archive.has_value() &&
          contains_text(*archive, "ordering-previous-evidence-marker") &&
          contains_text(*archive, "ordering-current-evidence-marker"),
      "an invalid N/N-1 generation order must archive both raw generations before reinitialization");
  if (reset.snapshot.has_value()) {
    fixture.files.corrupt(key, StateFileSlot::current);
    auto after_new_corruption = fixture.store.inspect(key);
    passed &= expect(
        after_new_corruption.mode == StateReadMode::read_only_corrupt &&
            !fixture.files.raw_file(key, StateFileSlot::previous).has_value(),
        "a new epoch must not fall back to a parseable previous generation from before reinitialization");
  }
  return passed;
}

struct FaultScenario final {
  StateFileOperation operation;
  std::optional<StateFileSlot> slot;
  std::size_t occurrence;
  char const* stage;
  bool outcome_unknown;
};

[[nodiscard]] bool write_stage_failures_preserve_a_valid_authority() {
  constexpr FaultScenario scenarios[]{
      {StateFileOperation::write, StateFileSlot::candidate, 1,
       "candidate-write", false},
      {StateFileOperation::flush, StateFileSlot::candidate, 1,
       "candidate-flush", false},
      {StateFileOperation::read, StateFileSlot::candidate, 2,
       "candidate-reread", false},
      {StateFileOperation::write, StateFileSlot::intent, 1,
       "intent-write", false},
      {StateFileOperation::flush, StateFileSlot::intent, 1,
       "intent-flush", false},
      {StateFileOperation::write, StateFileSlot::previous_staging, 1,
       "previous-staging-write", false},
      {StateFileOperation::flush, StateFileSlot::previous_staging, 1,
       "previous-staging-flush", false},
      {StateFileOperation::replace, StateFileSlot::previous, 1,
       "previous-replace", false},
      {StateFileOperation::read, StateFileSlot::previous, 2,
       "previous-reread", false},
      {StateFileOperation::flush_volume, std::nullopt, 1,
       "previous-volume-flush", false},
      {StateFileOperation::replace, StateFileSlot::current, 1,
       "current-replace", false},
      {StateFileOperation::flush_volume, std::nullopt, 2, "volume-flush",
       true},
      {StateFileOperation::read, StateFileSlot::current, 2,
       "current-reread", true},
  };

  bool passed = true;
  for (auto const& scenario : scenarios) {
    Fixture fixture;
    auto const key = StateKey::machine(AggregateId{"fault-stages"});
    auto first = fixture.store.initialize(key, state("first"));
    if (!first.snapshot.has_value()) {
      return expect(false, "fault fixture must initialize");
    }
    fixture.files.fail_on({.operation = scenario.operation,
                           .slot = scenario.slot,
                           .occurrence = scenario.occurrence});
    auto result = fixture.store.commit(StateCommitRequest{
        .key = key,
        .expected_revision = first.snapshot->revision,
        .state = state("second"),
    });
    if (result.failed_stage != scenario.stage) {
      std::cerr << "expected fault stage " << scenario.stage << ", got "
                << result.failed_stage << '\n';
    }
    passed &= expect(result.failed_stage == scenario.stage,
                     "a write failure must identify its exact stage");
    passed &= expect(
        result.status == (scenario.outcome_unknown
                              ? StateCommitStatus::outcome_unknown
                              : StateCommitStatus::failed),
        "a write failure must distinguish failed from unknown outcome");

    auto recovered = fixture.store.inspect(key);
    passed &= expect(recovered.mode == StateReadMode::writable &&
                         recovered.snapshot.has_value(),
                     "every injected failure must leave a valid authority");
    if (recovered.snapshot.has_value()) {
      auto const expected = scenario.outcome_unknown ? "second" : "first";
      passed &= expect(recovered.snapshot->state.value.payload ==
                           bytes(expected),
                       "post-failure recovery must expose only an old or fully committed generation");
    }
  }
  return passed;
}

[[nodiscard]] bool transaction_residuals_recover_without_guessing() {
  bool passed = true;
  Fixture fixture;
  auto const key = StateKey::machine(AggregateId{"transaction-recovery"});
  auto first = fixture.store.initialize(key, state("first"));
  if (!first.snapshot.has_value()) {
    return expect(false, "transaction fixture must initialize");
  }
  fixture.files.fail_next(StateFileOperation::write,
                          StateFileSlot::previous_staging);
  fixture.files.fail_next(StateFileOperation::remove, StateFileSlot::intent);
  fixture.files.fail_next(StateFileOperation::remove,
                          StateFileSlot::candidate);
  auto interrupted = fixture.store.commit(StateCommitRequest{
      .key = key,
      .expected_revision = first.snapshot->revision,
      .state = state("second"),
  });
  passed &= expect(interrupted.status == StateCommitStatus::outcome_unknown,
                   "an uncleanable durable intent must report unknown outcome");
  passed &= expect(
      fixture.files.raw_file(key, StateFileSlot::intent).has_value() &&
          fixture.files.raw_file(key, StateFileSlot::candidate).has_value(),
      "the fixture must retain an intent and candidate residual");

  DeviceStateStore restarted{fixture.files, fixture.clock};
  auto recovered = restarted.inspect(key);
  passed &= expect(recovered.mode == StateReadMode::writable &&
                       recovered.snapshot.has_value() &&
                       recovered.snapshot->state.value.payload ==
                           bytes("second"),
                   "a valid intent may complete its exact candidate after restart");
  passed &= expect(
      !fixture.files.raw_file(key, StateFileSlot::intent).has_value() &&
          !fixture.files.raw_file(key, StateFileSlot::candidate).has_value(),
      "successful transaction recovery must clean its residuals");

  Fixture candidate_only_source;
  auto const orphan_key =
      StateKey::machine(AggregateId{"candidate-without-intent"});
  auto source = candidate_only_source.store.initialize(orphan_key,
                                                       state("candidate"));
  if (!source.snapshot.has_value()) {
    return expect(false, "candidate-only source must initialize");
  }
  auto orphan_bytes = candidate_only_source.files.raw_file(
      orphan_key, StateFileSlot::current);
  InMemoryStateFileSystem orphan_files;
  FixedClock orphan_clock{azzs::application::WallClockTime{1234ms}};
  DeviceStateStore orphan_store{orphan_files, orphan_clock};
  orphan_files.seed(orphan_key, StateFileSlot::candidate, *orphan_bytes);
  auto orphan = orphan_store.inspect(orphan_key);
  passed &= expect(orphan.mode == StateReadMode::read_only_corrupt,
                   "a candidate without a valid intent must never self-promote or look empty");

  Fixture previous_validation;
  auto const validation_key =
      StateKey::machine(AggregateId{"recovery-validates-previous"});
  auto validation_first =
      previous_validation.store.initialize(validation_key, state("first"));
  if (!validation_first.snapshot.has_value()) {
    return expect(false, "previous validation fixture must initialize");
  }
  previous_validation.files.fail_next(
      StateFileOperation::write, StateFileSlot::previous_staging);
  previous_validation.files.fail_next(StateFileOperation::remove,
                                      StateFileSlot::intent);
  previous_validation.files.fail_next(StateFileOperation::remove,
                                      StateFileSlot::candidate);
  auto validation_interrupted = previous_validation.store.commit(
      StateCommitRequest{.key = validation_key,
                         .expected_revision =
                             validation_first.snapshot->revision,
                         .state = state("second")});
  passed &= expect(validation_interrupted.status ==
                       StateCommitStatus::outcome_unknown,
                   "the previous validation fixture must retain its transaction");
  previous_validation.files.fail_on(
      {.operation = StateFileOperation::read,
       .slot = StateFileSlot::previous,
       .occurrence = 2});
  DeviceStateStore validation_restart{previous_validation.files,
                                      previous_validation.clock};
  auto validation_recovered = validation_restart.inspect(validation_key);
  passed &= expect(
      validation_recovered.mode == StateReadMode::writable &&
          validation_recovered.snapshot.has_value() &&
          validation_recovered.snapshot->state.value.payload == bytes("first"),
      "transaction recovery must not publish current until the newly written previous generation rereads and validates");

  Fixture null_intent_source;
  auto const null_intent_key =
      StateKey::machine(AggregateId{"null-intent-recovery"});
  null_intent_source.files.fail_next(StateFileOperation::replace,
                                    StateFileSlot::current);
  null_intent_source.files.fail_next(StateFileOperation::remove,
                                    StateFileSlot::intent);
  null_intent_source.files.fail_next(StateFileOperation::remove,
                                    StateFileSlot::candidate);
  auto null_interrupted = null_intent_source.store.initialize(
      null_intent_key, state("new-initialization"));
  auto null_candidate = null_intent_source.files.raw_file(
      null_intent_key, StateFileSlot::candidate);
  auto null_intent = null_intent_source.files.raw_file(
      null_intent_key, StateFileSlot::intent);
  Fixture occupied_target;
  auto occupied =
      occupied_target.store.initialize(null_intent_key, state("old-epoch"));
  auto old_bytes = occupied_target.files.raw_file(
      null_intent_key, StateFileSlot::current);
  if (null_interrupted.status != StateCommitStatus::outcome_unknown ||
      !null_candidate.has_value() || !null_intent.has_value() ||
      !occupied.snapshot.has_value() || !old_bytes.has_value()) {
    return expect(false, "null-intent conflict fixture must be complete");
  }
  occupied_target.files.seed(null_intent_key, StateFileSlot::previous,
                             *old_bytes);
  static_cast<void>(occupied_target.files.remove(null_intent_key,
                                                 StateFileSlot::current));
  occupied_target.files.seed(null_intent_key, StateFileSlot::candidate,
                             *null_candidate);
  occupied_target.files.seed(null_intent_key, StateFileSlot::intent,
                             *null_intent);
  auto null_conflict = occupied_target.store.inspect(null_intent_key);
  passed &= expect(
      null_conflict.mode == StateReadMode::read_only_corrupt &&
          !occupied_target.files
               .raw_file(null_intent_key, StateFileSlot::current)
               .has_value() &&
          occupied_target.files.raw_file(null_intent_key,
                                         StateFileSlot::previous) == old_bytes,
      "an expected-null transaction may complete only when current and previous are both absent");

  return passed;
}

[[nodiscard]] bool recoverable_initialization_cannot_be_reinitialized() {
  Fixture fixture;
  auto const key =
      StateKey::machine(AggregateId{"recoverable-initialization"});
  fixture.files.fail_next(StateFileOperation::replace,
                          StateFileSlot::current);
  fixture.files.fail_next(StateFileOperation::remove, StateFileSlot::intent);
  fixture.files.fail_next(StateFileOperation::remove,
                          StateFileSlot::candidate);
  auto interrupted = fixture.store.initialize(key, state("recover-me"));
  bool passed = expect(
      interrupted.status == StateCommitStatus::outcome_unknown &&
          fixture.files.raw_file(key, StateFileSlot::intent).has_value() &&
          fixture.files.raw_file(key, StateFileSlot::candidate).has_value(),
      "the fixture must retain a recoverable initialization transaction");

  auto reset = fixture.store.reinitialize(StateReinitializeRequest{
      .key = key,
      .replacement = state("must-not-reset").value,
      .affected_categories = {DataImpactCategory::history},
      .confirmation_reference = "confirmation/recoverable-initialization",
  });
  auto recovered = fixture.store.inspect(key);
  passed &= expect(
      reset.status == StateCommitStatus::conflict &&
          recovered.mode == StateReadMode::writable &&
          recovered.snapshot.has_value() &&
          recovered.snapshot->state.value.payload == bytes("recover-me"),
      "reinitialization must recover an exact durable transaction before considering dual corruption");
  return passed;
}

[[nodiscard]] bool checkpoints_conflict_and_consume_before_cleanup() {
  bool passed = true;
  Fixture fixture;
  auto const key = StateKey::for_subject(StateSubject{"interactive-user"},
                                         AggregateId{"catalog-draft"});
  auto first = fixture.store.initialize(key, state("saved-draft"));
  if (!first.snapshot.has_value()) {
    return expect(false, "checkpoint fixture must initialize");
  }
  auto written = fixture.store.write_checkpoint(
      key, StateCheckpoint{.base_revision = first.snapshot->revision,
                           .payload = bytes("unsaved")});
  passed &= expect(written.status == CheckpointStatus::available,
                   "a matching subject revision must accept a checkpoint");
  auto second = fixture.store.commit(StateCommitRequest{
      .key = key,
      .expected_revision = first.snapshot->revision,
      .state = state("new-saved-draft"),
  });
  if (!second.snapshot.has_value()) {
    return expect(false, "checkpoint fixture must advance its base");
  }
  auto conflict =
      fixture.store.read_checkpoint(key, second.snapshot->revision);
  passed &= expect(conflict.status == CheckpointStatus::conflict,
                   "a changed base revision must not auto-merge a checkpoint");

  auto current_checkpoint = fixture.store.write_checkpoint(
      key, StateCheckpoint{.base_revision = second.snapshot->revision,
                           .payload = bytes("current-unsaved")});
  passed &= expect(current_checkpoint.status == CheckpointStatus::available,
                   "a current base revision must accept a new checkpoint");
  fixture.files.fail_next(StateFileOperation::remove,
                          StateFileSlot::checkpoint);
  auto consumed =
      fixture.store.consume_checkpoint(key, second.snapshot->revision);
  passed &= expect(consumed.status == CheckpointStatus::consumed,
                   "checkpoint consumption must commit before payload cleanup");
  auto after_failed_cleanup =
      fixture.store.read_checkpoint(key, second.snapshot->revision);
  passed &= expect(after_failed_cleanup.status == CheckpointStatus::consumed,
                   "a retained payload must not revive after its consumed tombstone");

  fixture.files.fail_next(StateFileOperation::remove,
                          StateFileSlot::checkpoint_consumed);
  auto identical_rewrite = fixture.store.write_checkpoint(
      key, StateCheckpoint{.base_revision = second.snapshot->revision,
                           .payload = bytes("current-unsaved")});
  auto after_identical_rewrite =
      fixture.store.read_checkpoint(key, second.snapshot->revision);
  passed &= expect(
      identical_rewrite.status == CheckpointStatus::failed &&
          after_identical_rewrite.status == CheckpointStatus::available &&
          after_identical_rewrite.checkpoint.has_value() &&
          after_identical_rewrite.checkpoint->payload ==
              bytes("current-unsaved"),
      "an old tombstone must not consume a newly published checkpoint with identical content");

  auto device_key = StateKey::machine(AggregateId{"device-checkpoint"});
  auto device_state = fixture.store.initialize(device_key, state("device"));
  if (device_state.snapshot.has_value()) {
    auto rejected = fixture.store.write_checkpoint(
        device_key,
        StateCheckpoint{.base_revision = device_state.snapshot->revision,
                        .payload = bytes("wrong-partition")});
    passed &= expect(rejected.status == CheckpointStatus::failed,
                     "unsaved checkpoints must stay in the subject partition");
  }
  return passed;
}

[[nodiscard]] bool instances_and_partitions_share_only_their_authority() {
  bool passed = true;
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{4321ms}};
  DeviceStateStore first_store{files, clock};
  DeviceStateStore second_store{files, clock};
  auto const machine = StateKey::machine(AggregateId{"shared-occupancy"});
  auto initial = first_store.initialize(machine, state("free"));
  if (!initial.snapshot.has_value()) {
    return expect(false, "two-instance fixture must initialize");
  }
  auto observed = second_store.inspect(machine);
  if (!observed.snapshot.has_value()) {
    return expect(false, "the second instance must observe machine state");
  }
  auto first_commit = first_store.commit(StateCommitRequest{
      .key = machine,
      .expected_revision = initial.snapshot->revision,
      .state = state("occupied"),
  });
  auto stale_commit = second_store.commit(StateCommitRequest{
      .key = machine,
      .expected_revision = observed.snapshot->revision,
      .state = state("also-occupied"),
  });
  passed &= expect(first_commit.status == StateCommitStatus::committed &&
                       stale_commit.status == StateCommitStatus::conflict,
                   "two instances using one revision must have exactly one winner");

  auto held_lock = files.try_lock(machine);
  passed &= expect(held_lock.status ==
                           azzs::application::StateFileLockStatus::acquired &&
                       held_lock.lock != nullptr,
                   "the test must acquire the cross-instance commit lock");
  passed &= expect(second_store.inspect(machine).mode == StateReadMode::busy,
                   "another instance must observe a held commit lock as busy");
  held_lock.lock.reset();

  files.fail_next(StateFileOperation::lock);
  auto lock_failure = second_store.inspect(machine);
  passed &= expect(lock_failure.mode == StateReadMode::failed &&
                       !lock_failure.error.empty(),
                   "lock I/O failure must not be mislabeled as another instance");

  auto const alice = StateKey::for_subject(StateSubject{"alice"},
                                           AggregateId{"draft"});
  auto const bob = StateKey::for_subject(StateSubject{"bob"},
                                         AggregateId{"draft"});
  auto alice_state = first_store.initialize(alice, state("alice-only"));
  auto bob_state = first_store.initialize(bob, state("bob-only"));
  passed &= expect(alice_state.status == StateCommitStatus::committed &&
                       bob_state.status == StateCommitStatus::committed,
                   "different state subjects must have independent aggregates");
  auto alice_read = second_store.inspect(alice);
  auto bob_read = second_store.inspect(bob);
  passed &= expect(alice_read.snapshot.has_value() &&
                       bob_read.snapshot.has_value() &&
                       alice_read.snapshot->state.value.payload ==
                           bytes("alice-only") &&
                       bob_read.snapshot->state.value.payload ==
                           bytes("bob-only"),
                   "subject data must not cross its partition boundary");
  return passed;
}

[[nodiscard]] bool encoding_is_deterministic_for_fixed_inputs() {
  Fixture first;
  Fixture second;
  auto const key = StateKey::machine(AggregateId{"deterministic"});
  auto first_result = first.store.initialize(key, state("same"));
  auto second_result = second.store.initialize(key, state("same"));
  bool passed = true;
  passed &= expect(first_result.status == StateCommitStatus::committed &&
                       second_result.status == StateCommitStatus::committed,
                   "deterministic fixtures must initialize");
  passed &= expect(first.files.raw_file(key, StateFileSlot::current) ==
                       second.files.raw_file(key, StateFileSlot::current),
                   "fixed key, clock and state must produce identical binary bytes");
  return passed;
}

[[nodiscard]] bool state_keys_reject_unsafe_path_components() {
  bool passed = true;
  passed &= expect(AggregateId{"recovery-records.v2"}.valid(),
                   "a bounded lowercase aggregate identifier must be valid");
  passed &= expect(!AggregateId{"Recovery"}.valid(),
                   "aggregate identifiers must reject uppercase characters");
  passed &= expect(!AggregateId{"../escape"}.valid() &&
                       !AggregateId{"escape\\child"}.valid() &&
                       !AggregateId{"drive:c"}.valid(),
                   "aggregate identifiers must reject path separators and colons");
  passed &= expect(!AggregateId{"."}.valid() && !AggregateId{".."}.valid() &&
                       !AggregateId{"con"}.valid() &&
                       !AggregateId{"com1.log"}.valid() &&
                       !AggregateId{"name."}.valid(),
                   "aggregate identifiers must reject reserved Windows components");
  passed &= expect(!AggregateId{std::string(65, 'a')}.valid() &&
                       !AggregateId{std::string{"safe\0tail", 9}}.valid(),
                   "aggregate identifiers must reject overlong and NUL input");

  passed &= expect(StateSubject{"S-1-5-21-1000"}.valid() &&
                       StateSubject{"test.user_01"}.valid(),
                   "state subjects must accept SID-compatible ASCII identifiers");
  passed &= expect(!StateSubject{"../alice"}.valid() &&
                       !StateSubject{"alice/bob"}.valid() &&
                       !StateSubject{"alice:bob"}.valid() &&
                       !StateSubject{"NUL"}.valid() &&
                       !StateSubject{std::string(185, 'a')}.valid(),
                   "state subjects must reject unsafe or overlong components");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= first_and_continuous_commit_keep_n_minus_one();
  passed &= schema_window_and_future_bytes_are_safe();
  passed &= dual_corruption_requires_audited_reinitialization();
  passed &= invalid_generation_order_preserves_both_generations();
  passed &= write_stage_failures_preserve_a_valid_authority();
  passed &= transaction_residuals_recover_without_guessing();
  passed &= recoverable_initialization_cannot_be_reinitialized();
  passed &= checkpoints_conflict_and_consume_before_cleanup();
  passed &= instances_and_partitions_share_only_their_authority();
  passed &= encoding_is_deterministic_for_fixed_inputs();
  passed &= state_keys_reject_unsafe_path_components();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "device state contract passed\n";
  return EXIT_SUCCESS;
}
