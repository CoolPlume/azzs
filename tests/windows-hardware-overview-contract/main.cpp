#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/adapters/windows/windows_hardware_observer.hpp"

namespace {

using azzs::adapters::windows::WindowsHardwareObserver;
using azzs::adapters::windows::WindowsHardwareQueryCode;
using azzs::adapters::windows::WindowsHardwareQueryExecutor;
using azzs::adapters::windows::WindowsHardwareQueryResult;
using azzs::application::HardwareObservationCode;
using azzs::application::HardwareDeviceKind;
using azzs::application::HardwareDeviceStatus;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows hardware overview contract failed: " << message
              << '\n';
  }
  return condition;
}

struct ExpectedQuery final {
  std::string class_name;
  std::vector<std::string> properties;
  WindowsHardwareQueryResult result;
};

class FakeQueryExecutor final : public WindowsHardwareQueryExecutor {
 public:
  std::vector<ExpectedQuery> expected;
  std::size_t calls{0};
  bool mismatch{false};

  [[nodiscard]] WindowsHardwareQueryResult query(
      std::string_view class_name,
      std::span<std::string_view const> properties,
      std::stop_token cancellation) override {
    if (cancellation.stop_requested()) {
      return {.code = WindowsHardwareQueryCode::cancelled,
              .error = "cancelled by test"};
    }
    if (calls >= expected.size()) {
      mismatch = true;
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "unexpected query"};
    }
    auto const& query = expected[calls++];
    if (class_name != query.class_name || properties.size() != query.properties.size()) {
      mismatch = true;
      return {.code = WindowsHardwareQueryCode::failed,
              .error = "query shape mismatch"};
    }
    for (std::size_t index = 0; index < properties.size(); ++index) {
      if (properties[index] != query.properties[index]) {
        mismatch = true;
        return {.code = WindowsHardwareQueryCode::failed,
                .error = "query property mismatch"};
      }
    }
    return query.result;
  }
};

[[nodiscard]] std::vector<ExpectedQuery> full_queries() {
  return {
      {"Win32_Processor",
       {"Name", "Manufacturer", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"AMD Ryzen 7", "AuthenticAMD",
                  "ACPI\\AuthenticAMD_0000", "OK", "0"}}}},
      {"Win32_VideoController",
       {"Name", "AdapterCompatibility", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode", "VideoProcessor"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"NVIDIA GeForce RTX", "NVIDIA",
                  "PCI\\VEN_10DE&DEV_0001", "OK", "0", "RTX"},
                 {"AMD Radeon", "AMD", "PCI\\VEN_1002&DEV_0002", "OK",
                  "0", "Radeon"}}}},
      {"Win32_BaseBoard",
       {"Manufacturer", "Product", "HostingBoard", "Status",
        "ConfigManagerErrorCode"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"ASUS", "PRIME B650", "TRUE", "OK", "0"}}}},
      {"Win32_NetworkAdapter",
       {"Name", "Manufacturer", "AdapterType", "PhysicalAdapter",
        "PNPDeviceID", "Status", "ConfigManagerErrorCode", "ServiceName",
        "NetConnectionStatus"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"Intel Ethernet", "Intel", "Ethernet 802.3", "TRUE",
                  "PCI\\VEN_8086&DEV_0003", "OK", "0", "e1iexpress", "2"}}}},
      {"Win32_ComputerSystem", {"Manufacturer", "Model"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"ASUS", "ROG"}}}},
  };
}

[[nodiscard]] bool complete_read_only_model_facts_are_mapped() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  WindowsHardwareObserver observer{std::move(executor)};

  auto const result = observer.observe({});

  return expect(result.code == HardwareObservationCode::succeeded &&
                    result.observation.has_value(),
                "complete WMI facts must produce a successful observation") &&
         expect(!raw_executor->mismatch && raw_executor->calls == 5,
                "the adapter must use exactly the five approved read-only "
                "model queries") &&
                expect(result.observation->cpu == "AMD Ryzen 7" &&
                    result.observation->gpu ==
                        "NVIDIA GeForce RTX; AMD Radeon" &&
                    result.observation->motherboard == "ASUS PRIME B650" &&
                    result.observation->network_adapter == "Intel Ethernet" &&
                    result.observation->oem_model == "ASUS ROG" &&
                    result.observation->devices.size() == 5 &&
                    result.observation->has_confirmed_physical_hardware(),
                "CPU, GPU, board, network, and OEM model facts must map "
                "without serial, MAC, IP, or computer-name fields and only "
                "confirmed physical records");
}

[[nodiscard]] bool disabled_driverless_and_error_devices_are_retained() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[1].result.rows = {
      {"Microsoft Basic Display Adapter", "Microsoft",
       "PCI\\VEN_1234&DEV_0001", "Error", "28", ""}};
  raw_executor->expected[2].result.rows = {
      {"ASUS", "PRIME B650", "TRUE", "Disabled", "22"}};
  raw_executor->expected[3].result.rows = {
      {"Intel Ethernet", "Intel", "Ethernet 802.3", "TRUE",
       "PCI\\VEN_8086&DEV_0003", "Error", "10", "e1iexpress", "2"}};
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  if (!expect(result.code == HardwareObservationCode::succeeded &&
                  result.observation.has_value() &&
                  result.observation->devices.size() == 4,
              "physical devices with degraded states must remain visible")) {
    return false;
  }
  auto const& devices = result.observation->devices;
  return expect(std::ranges::any_of(
                    devices, [](auto const& device) {
                      return device.kind == HardwareDeviceKind::gpu &&
                             device.status == HardwareDeviceStatus::no_driver;
                    }),
                "a physical display without a driver must remain visible") &&
         expect(std::ranges::any_of(
                    devices, [](auto const& device) {
                      return device.kind == HardwareDeviceKind::motherboard &&
                             device.status == HardwareDeviceStatus::disabled;
                    }),
                "a disabled physical board must remain visible") &&
         expect(std::ranges::any_of(
                    devices, [](auto const& device) {
                      return device.kind == HardwareDeviceKind::network_adapter &&
                             device.status == HardwareDeviceStatus::error;
                    }),
                "an error-state physical network adapter must remain visible");
}

[[nodiscard]] bool virtual_software_vpn_loopback_and_unknown_rows_are_filtered() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[0].result.rows = {
      {"Virtual CPU", "Microsoft", "ROOT\\VIRTUALCPU", "OK", "0"}};
  raw_executor->expected[1].result.rows = {
      {"Microsoft Remote Display", "Microsoft", "ROOT\\RDP_MF", "OK",
       "0", ""},
      {"VMware SVGA", "VMware", "PCI\\VEN_15AD&DEV_0405", "OK", "0",
       ""}};
  raw_executor->expected[2].result.rows = {
      {"Microsoft", "Virtual Machine", "TRUE", "OK", "0"}};
  raw_executor->expected[3].result.rows = {
      {"TAP-Windows Adapter V9", "OpenVPN", "VPN", "FALSE",
       "ROOT\\TAP0901", "OK", "0", "tap0901", "2"},
      {"Loopback Pseudo-Interface", "Microsoft", "Loopback", "FALSE",
       "SWD\\LOOPBACK", "OK", "0", "", "2"},
      {"Hyper-V Virtual Ethernet", "Microsoft", "Ethernet", "TRUE",
       "VMBUS\\NETVSC", "OK", "0", "netvsc", "2"},
      {"Unknown adapter", "Unknown", "Ethernet", "TRUE", "", "OK", "0",
       "unknown", "2"}};
  raw_executor->expected[4].result.rows =
      {{"Microsoft Corporation", "Virtual Machine"}};
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.code == HardwareObservationCode::failed,
                "an observation with no confirmed physical hardware must fail closed") &&
         expect(result.observation.has_value() &&
                    !result.observation->has_confirmed_physical_hardware() &&
                    result.observation->cpu.empty() && result.observation->gpu.empty() &&
                    result.observation->motherboard.empty() &&
                    result.observation->network_adapter.empty() &&
                    result.observation->devices.empty(),
                "virtual, software, VPN, loopback, and unknown rows must not enter summaries");
}

[[nodiscard]] bool partial_failure_preserves_usable_model_facts() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[1].result = {
      .code = WindowsHardwareQueryCode::failed,
      .error = "video query unavailable",
  };
  WindowsHardwareObserver observer{std::move(executor)};

  auto const result = observer.observe({});

  return expect(result.code == HardwareObservationCode::partial &&
                    result.observation.has_value() &&
                    result.observation->gpu.empty() &&
                    result.observation->cpu == "AMD Ryzen 7",
                "a non-terminal probe failure must return usable partial facts") &&
         expect(!raw_executor->mismatch && raw_executor->calls == 5,
                "a partial failure must still collect independent model facts");
}

[[nodiscard]] bool permission_denial_and_cancellation_are_terminal() {
  auto denied_executor = std::make_unique<FakeQueryExecutor>();
  auto* denied_raw = denied_executor.get();
  denied_raw->expected = full_queries();
  denied_raw->expected[0].result = {
      .code = WindowsHardwareQueryCode::permission_denied,
      .error = "access denied",
  };
  WindowsHardwareObserver denied_observer{std::move(denied_executor)};
  auto const denied = denied_observer.observe({});

  auto cancelled_executor = std::make_unique<FakeQueryExecutor>();
  auto* cancelled_raw = cancelled_executor.get();
  cancelled_raw->expected = full_queries();
  WindowsHardwareObserver cancelled_observer{std::move(cancelled_executor)};
  std::stop_source cancellation;
  cancellation.request_stop();
  auto const cancelled = cancelled_observer.observe(cancellation.get_token());

  return expect(denied.code == HardwareObservationCode::permission_denied &&
                    denied_raw->calls == 1,
                "permission denial must stop observation for an unrecognized "
                "result upstream") &&
         expect(cancelled.code == HardwareObservationCode::cancelled &&
                    cancelled_raw->calls == 0,
                "pre-cancelled observation must not execute WMI queries");
}

[[nodiscard]] bool model_change_probe_requires_complete_facts() {
  auto complete_executor = std::make_unique<FakeQueryExecutor>();
  auto* complete_raw = complete_executor.get();
  complete_raw->expected = full_queries();
  WindowsHardwareObserver complete_observer{std::move(complete_executor)};
  auto const complete = complete_observer.current_model_observation({});

  auto partial_executor = std::make_unique<FakeQueryExecutor>();
  auto* partial_raw = partial_executor.get();
  partial_raw->expected = full_queries();
  partial_raw->expected[3].result = {
      .code = WindowsHardwareQueryCode::failed,
      .error = "network query unavailable",
  };
  WindowsHardwareObserver partial_observer{std::move(partial_executor)};
  auto const partial = partial_observer.current_model_observation({});

  return expect(complete.has_value() && complete_raw->calls == 5,
                "a complete read-only model probe may provide a cache key") &&
         expect(!partial.has_value() && partial_raw->calls == 5,
                "partial probes must not falsely declare a hardware change");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= complete_read_only_model_facts_are_mapped();
  passed &= disabled_driverless_and_error_devices_are_retained();
  passed &= virtual_software_vpn_loopback_and_unknown_rows_are_filtered();
  passed &= partial_failure_preserves_usable_model_facts();
  passed &= permission_denial_and_cancellation_are_terminal();
  passed &= model_change_probe_requires_complete_facts();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
