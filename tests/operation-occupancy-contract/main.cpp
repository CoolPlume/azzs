#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>

#include "azzs/adapters/infrastructure/state_operation_occupancy_storage.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/application/operation_occupancy.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_operation_occupancy_storage.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

using azzs::application::OccupancyResultCode;
using azzs::application::OperationIdentity;
using azzs::application::SharedOperationOccupancy;
using azzs::adapters::infrastructure::StateOperationOccupancyStorage;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryOperationOccupancyStorage;
using azzs::testing::InMemoryStateFileSystem;
using azzs::testing::SequenceLeaseTokenSource;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "operation occupancy contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] OperationIdentity operation(std::string id) {
  return OperationIdentity{
      .kind = "consumer-operation",
      .operation_id = std::move(id),
      .correlation_id = "correlation-1",
  };
}

[[nodiscard]] bool verify_acquire_observe_and_release() {
  InMemoryOperationOccupancyStorage storage;
  SequenceLeaseTokenSource tokens{"lease-a-"};
  SharedOperationOccupancy occupancy{storage, tokens};

  auto const acquired = occupancy.try_acquire(operation("operation-1"));
  bool passed = expect(acquired.code == OccupancyResultCode::acquired &&
                           acquired.lease.has_value(),
                       "the first instance must acquire the device occupancy");
  auto const observed = occupancy.inspect();
  passed &= expect(observed.code == OccupancyResultCode::observed &&
                       observed.current.has_value() &&
                       observed.current->identity.operation_id == "operation-1",
                   "the persistent occupancy must be observable");

  SequenceLeaseTokenSource other_tokens{"lease-b-"};
  SharedOperationOccupancy other{storage, other_tokens};
  auto const occupied = other.try_acquire(operation("operation-2"));
  passed &= expect(occupied.code == OccupancyResultCode::occupied &&
                       occupied.current.has_value() &&
                       occupied.current->identity.operation_id == "operation-1",
                   "a second instance must receive the current occupant");

  auto const released = occupancy.release(*acquired.lease);
  passed &= expect(released.code == OccupancyResultCode::released,
                   "the matching lease must release the occupancy");
  auto const second = other.try_acquire(operation("operation-2"));
  passed &= expect(second.code == OccupancyResultCode::acquired,
                   "another instance may acquire only after release");
  return passed;
}

[[nodiscard]] bool verify_stale_release_is_rejected() {
  InMemoryOperationOccupancyStorage storage;
  SequenceLeaseTokenSource first_tokens{"first-"};
  SequenceLeaseTokenSource second_tokens{"second-"};
  SharedOperationOccupancy first{storage, first_tokens};
  SharedOperationOccupancy second{storage, second_tokens};

  auto const old_lease = first.try_acquire(operation("old"));
  bool passed = expect(old_lease.lease.has_value(),
                       "the old lease fixture must be acquired");
  passed &= expect(first.release(*old_lease.lease).code ==
                       OccupancyResultCode::released,
                   "the old lease fixture must be released");
  auto const new_lease = second.try_acquire(operation("new"));
  passed &= expect(new_lease.lease.has_value(),
                   "a new lease must replace the released lease");

  auto const stale = first.release(*old_lease.lease);
  passed &= expect(stale.code == OccupancyResultCode::stale_lease,
                   "an old token must not release a newer lease");
  auto const observed = first.inspect();
  passed &= expect(observed.current.has_value() &&
                       observed.current->identity.operation_id == "new",
                   "a stale release must leave the new occupant intact");
  return passed;
}

[[nodiscard]] bool verify_concurrent_claim_has_one_winner() {
  InMemoryOperationOccupancyStorage storage;
  SequenceLeaseTokenSource left_tokens{"left-"};
  SequenceLeaseTokenSource right_tokens{"right-"};
  SharedOperationOccupancy left{storage, left_tokens};
  SharedOperationOccupancy right{storage, right_tokens};
  std::barrier start{3};
  std::optional<azzs::application::OccupancyResult> left_result;
  std::optional<azzs::application::OccupancyResult> right_result;

  std::jthread left_thread{[&] {
    start.arrive_and_wait();
    left_result = left.try_acquire(operation("left"));
  }};
  std::jthread right_thread{[&] {
    start.arrive_and_wait();
    right_result = right.try_acquire(operation("right"));
  }};
  start.arrive_and_wait();
  left_thread.join();
  right_thread.join();

  auto const winners =
      (left_result->code == OccupancyResultCode::acquired ? 1 : 0) +
      (right_result->code == OccupancyResultCode::acquired ? 1 : 0);
  auto const occupied =
      (left_result->code == OccupancyResultCode::occupied ? 1 : 0) +
      (right_result->code == OccupancyResultCode::occupied ? 1 : 0);
  return expect(winners == 1 && occupied == 1,
                "two instances must produce one winner and one occupied result");
}

[[nodiscard]] bool verify_storage_error_is_typed() {
  InMemoryOperationOccupancyStorage storage;
  SequenceLeaseTokenSource tokens{"lease-"};
  SharedOperationOccupancy occupancy{storage, tokens};
  storage.fail_writes(true);
  auto const result = occupancy.try_acquire(operation("operation"));
  return expect(result.code == OccupancyResultCode::storage_error &&
                    result.raw_error == 112,
                "storage failures must retain the original error");
}

[[nodiscard]] bool verify_device_state_backed_storage_contract() {
  using namespace std::chrono_literals;
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{9'001ms}};
  azzs::application::DeviceStateStore first_states{files, clock};
  azzs::application::DeviceStateStore second_states{files, clock};
  StateOperationOccupancyStorage first_storage{first_states};
  StateOperationOccupancyStorage second_storage{second_states};
  SequenceLeaseTokenSource first_tokens{"state-first-"};
  SequenceLeaseTokenSource second_tokens{"state-second-"};
  SharedOperationOccupancy first{first_storage, first_tokens};
  SharedOperationOccupancy second{second_storage, second_tokens};

  auto acquired = first.try_acquire(operation("persistent"));
  bool passed = expect(acquired.code == OccupancyResultCode::acquired &&
                           acquired.lease.has_value(),
                       "the production occupancy adapter must initialize its "
                       "device aggregate");
  auto occupied = second.try_acquire(operation("other-instance"));
  passed &= expect(occupied.code == OccupancyResultCode::occupied &&
                       occupied.current.has_value() &&
                       occupied.current->identity.operation_id == "persistent",
                   "a second state-store instance must observe the durable "
                   "occupant");
  auto released = first.release(*acquired.lease);
  passed &= expect(released.code == OccupancyResultCode::released,
                   "the state-backed matching lease must release durably");
  auto observed = second.inspect();
  passed &= expect(observed.code == OccupancyResultCode::observed &&
                       !observed.current.has_value() && observed.revision == 2,
                   "the released state-backed record must retain a monotonic "
                   "revision");

  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"operation-occupancy"});
  files.corrupt(key, azzs::application::StateFileSlot::current);
  files.corrupt(key, azzs::application::StateFileSlot::previous);
  auto read_only = first.inspect();
  passed &= expect(read_only.code == OccupancyResultCode::read_only,
                   "dual state corruption must make occupancy read-only");
  return passed;
}

[[nodiscard]] bool verify_recovered_previous_cannot_grant_a_lease() {
  using namespace std::chrono_literals;
  InMemoryStateFileSystem files;
  FixedClock clock{azzs::application::WallClockTime{9'002ms}};
  azzs::application::DeviceStateStore states{files, clock};
  StateOperationOccupancyStorage storage{states};
  SequenceLeaseTokenSource tokens{"fallback-"};
  SharedOperationOccupancy occupancy{storage, tokens};

  auto first = occupancy.try_acquire(operation("first"));
  bool passed = expect(first.lease.has_value(),
                       "the fallback fixture must acquire its first lease");
  if (!first.lease.has_value()) {
    return false;
  }
  passed &= expect(occupancy.release(*first.lease).code ==
                       OccupancyResultCode::released,
                   "the fallback fixture must persist an empty N-1 generation");
  auto current = occupancy.try_acquire(operation("current"));
  passed &= expect(current.code == OccupancyResultCode::acquired,
                   "the fallback fixture must persist a newer active lease");

  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"operation-occupancy"});
  files.corrupt(key, azzs::application::StateFileSlot::current);
  auto observed = occupancy.inspect();
  auto attempted = occupancy.try_acquire(operation("must-not-acquire"));
  passed &= expect(
      observed.code == OccupancyResultCode::read_only &&
          attempted.code == OccupancyResultCode::read_only,
      "an occupancy recovered from N-1 may be observed only as read-only and must never grant another lease");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= verify_acquire_observe_and_release();
  passed &= verify_stale_release_is_rejected();
  passed &= verify_concurrent_claim_has_one_winner();
  passed &= verify_storage_error_is_typed();
  passed &= verify_device_state_backed_storage_contract();
  passed &= verify_recovered_previous_cannot_grant_a_lease();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "operation occupancy contract passed\n";
  return EXIT_SUCCESS;
}
