#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include "azzs/application/device_state_store.hpp"
#include "azzs/application/guided_initialization.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/in_memory_state_file_system.hpp"

namespace {

namespace guided = azzs::application::guided_initialization;
using azzs::application::DeviceStateStore;
using azzs::application::WallClockTime;
using azzs::testing::FixedClock;
using azzs::testing::InMemoryStateFileSystem;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "guided initialization contract failed: " << message << '\n';
  }
  return condition;
}

class FixtureEvidence final : public guided::EvidenceSource {
 public:
  [[nodiscard]] guided::Evidence observe() override {
    ++observe_calls;
    return evidence;
  }

  [[nodiscard]] bool continue_external_handoff(std::string_view software_id,
                                                std::string& error) override {
    ++external_continue_calls;
    if (!allow_external_continue) {
      error = "external continuation rejected by fixture";
      return false;
    }
    auto found = std::ranges::find_if(
        evidence.external_handoffs,
        [software_id](guided::ExternalHandoffEvidence const& handoff) {
          return handoff.software_id == software_id;
        });
    if (found == evidence.external_handoffs.end()) {
      error = "unknown external software";
      return false;
    }
    found->state = guided::ExternalHandoffState::skipped;
    evidence.software_installation = {
        .state = guided::StageState::partial,
        .detail = "external installation remains an explicitly recognized fact",
    };
    return true;
  }

  [[nodiscard]] bool continue_after_restart(std::string& error) override {
    ++restart_continue_calls;
    if (!allow_restart_continue) {
      error = "restart continuation rejected by fixture";
      return false;
    }
    evidence.restart_gate = guided::RestartGateState::none;
    evidence.system_optimization = {
        .state = guided::StageState::completed,
        .detail = "system optimization was verified after restart",
    };
    return true;
  }

  guided::Evidence evidence{
      .drivers = {.state = guided::StageState::pending},
      .system_optimization = {.state = guided::StageState::pending},
      .software_installation = {.state = guided::StageState::pending},
      .software_optimization = {.state = guided::StageState::no_applicable_items},
  };
  bool allow_external_continue{true};
  bool allow_restart_continue{true};
  int observe_calls{0};
  int external_continue_calls{0};
  int restart_continue_calls{0};
};

struct Fixture final {
  InMemoryStateFileSystem files;
  FixedClock clock{WallClockTime{}};
  DeviceStateStore states{files, clock};
  FixtureEvidence evidence;
  guided::GuidedInitializationService service{states, clock, evidence};

  [[nodiscard]] bool restore() { return service.restore().succeeded(); }
};

[[nodiscard]] bool skip_and_auto_skip_unavailable_stages() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "a new fixture must restore");
  passed &= expect(fixture.service.start().succeeded(),
                   "a new guided flow must start independently");
  passed &= expect(fixture.service.snapshot().active.has_value() &&
                       fixture.service.snapshot().active->current_stage ==
                           guided::Stage::drivers,
                   "a new flow must begin at drivers");

  auto skipped = fixture.service.skip_current_stage();
  passed &= expect(skipped.succeeded() && skipped.snapshot.active.has_value() &&
                       skipped.snapshot.active->current_stage ==
                           guided::Stage::system_optimization,
                   "driver skip must move only the guided record to system optimization");

  fixture.evidence.evidence.system_optimization = {
      .state = guided::StageState::completed,
      .detail = "settings already match the recommended baseline",
  };
  fixture.evidence.evidence.software_installation = {
      .state = guided::StageState::no_applicable_items,
      .detail = "no installable software remains",
  };
  auto refreshed = fixture.service.refresh();
  passed &= expect(refreshed.succeeded() && !refreshed.snapshot.active.has_value() &&
                       refreshed.snapshot.history.size() == 1 &&
                       refreshed.snapshot.history.front().state ==
                           guided::FlowState::completed,
                   "completed and unavailable stages must finish without creating a batch");
  passed &= expect(refreshed.snapshot.summary.skipped == 1 &&
                       refreshed.snapshot.summary.no_applicable_items == 2,
                   "the completion summary must retain skipped and no-applicable stages");
  return passed;
}

[[nodiscard]] bool external_handoff_waits_for_explicit_continue() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the handoff fixture must restore");
  passed &= expect(fixture.service.start().succeeded() &&
                       fixture.service.skip_current_stage().succeeded(),
                   "handoff setup must reach system optimization");
  fixture.evidence.evidence.system_optimization.state = guided::StageState::completed;
  fixture.evidence.evidence.software_installation = {
      .state = guided::StageState::external_handoff,
      .detail = "source resolution failed; external installation is available",
  };
  fixture.evidence.evidence.external_handoffs = {{
      .software_id = "example-software",
      .state = guided::ExternalHandoffState::waiting_for_external_install,
  }};
  auto waiting = fixture.service.refresh();
  passed &= expect(waiting.succeeded() && waiting.snapshot.active.has_value() &&
                       waiting.snapshot.active->current_stage ==
                           guided::Stage::software_installation &&
                       waiting.snapshot.active->stages[2].state ==
                           guided::StageState::external_handoff &&
                       waiting.snapshot.active->external_handoff_software_ids.size() == 1,
                   "an unresolved source must pause the software stage with only its stable id");

  fixture.evidence.evidence.external_handoffs.front().state =
      guided::ExternalHandoffState::externally_recognized;
  auto recognized = fixture.service.refresh();
  passed &= expect(recognized.succeeded() && recognized.snapshot.active.has_value() &&
                       recognized.snapshot.summary.externally_recognized == 1,
                   "recognition must remain a visible summary fact before continuation");
  passed &= expect(fixture.service.continue_external_handoff("example-software")
                       .succeeded() &&
                       fixture.evidence.external_continue_calls == 1 &&
                       !fixture.service.snapshot().active.has_value(),
                   "only explicit handoff continuation may replan the next stage");
  return passed;
}

[[nodiscard]] bool restart_barrier_requires_explicit_resume() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the restart fixture must restore");
  passed &= expect(fixture.service.start().succeeded() &&
                       fixture.service.skip_current_stage().succeeded(),
                   "restart setup must reach system optimization");
  fixture.evidence.evidence.restart_gate = guided::RestartGateState::waiting_for_windows_restart;
  fixture.evidence.evidence.system_optimization = {
      .state = guided::StageState::waiting_for_restart,
      .detail = "Windows restart is required before verification",
  };
  fixture.evidence.evidence.software_installation = {
      .state = guided::StageState::no_applicable_items,
      .detail = "no installable software remains",
  };
  auto waiting = fixture.service.refresh();
  passed &= expect(waiting.succeeded() && waiting.snapshot.active.has_value() &&
                       waiting.snapshot.active->state == guided::FlowState::waiting_for_restart,
                   "the shared restart gate must pause the flow");
  fixture.evidence.evidence.restart_gate = guided::RestartGateState::awaiting_user_continue;
  auto awaiting = fixture.service.refresh();
  passed &= expect(awaiting.succeeded() && awaiting.snapshot.active.has_value() &&
                       awaiting.snapshot.active->state ==
                           guided::FlowState::awaiting_restart_continue,
                   "read-only restart verification must still await explicit user continue");
  auto continued = fixture.service.continue_after_restart();
  passed &= expect(continued.succeeded() && fixture.evidence.restart_continue_calls == 1 &&
                       !continued.snapshot.active.has_value(),
                   "restart continuation must return to the same stage and then advance");
  return passed;
}

[[nodiscard]] bool failures_retry_without_restarting_work() {
  Fixture fixture;
  fixture.evidence.evidence.drivers = {
      .state = guided::StageState::failed,
      .detail = "driver handoff failed",
  };
  bool passed = expect(fixture.restore(), "the failed fixture must restore");
  auto started = fixture.service.start();
  passed &= expect(started.succeeded() && started.snapshot.active.has_value() &&
                       started.snapshot.summary.failed == 1 &&
                       started.snapshot.summary.retry_available,
                   "a current failed stage must expose only a retry intent");
  auto before_observations = fixture.evidence.observe_calls;
  auto retry = fixture.service.retry_current_stage();
  passed &= expect(retry.succeeded() && fixture.evidence.observe_calls == before_observations &&
                       retry.snapshot.active.has_value() &&
                       retry.snapshot.active->stages[0].state == guided::StageState::failed,
                   "retry must not observe a new source or fabricate a new operation");
  return passed;
}

[[nodiscard]] bool cancellation_and_restart_restore_history() {
  Fixture fixture;
  bool passed = expect(fixture.restore(), "the persistence fixture must restore");
  passed &= expect(fixture.service.start().succeeded() &&
                       fixture.service.cancel().succeeded(),
                   "cancellation must persist a read-only history record");
  auto const id = fixture.service.snapshot().history.front().id;
  guided::GuidedInitializationService reopened{fixture.states, fixture.clock,
                                               fixture.evidence};
  auto restored = reopened.restore();
  passed &= expect(restored.succeeded() && !restored.snapshot.active.has_value() &&
                       restored.snapshot.history.size() == 1 &&
                       restored.snapshot.history.front().id == id &&
                       restored.snapshot.history.front().state ==
                           guided::FlowState::cancelled,
                   "a later workbench must recover the immutable flow history");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= skip_and_auto_skip_unavailable_stages();
  passed &= external_handoff_waits_for_explicit_continue();
  passed &= restart_barrier_requires_explicit_resume();
  passed &= failures_retry_without_restarting_work();
  passed &= cancellation_and_restart_restore_history();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
