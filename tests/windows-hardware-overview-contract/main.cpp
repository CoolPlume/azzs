#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
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
using azzs::adapters::windows::WindowsCpuTopology;
using azzs::adapters::windows::WindowsDisplayEdid;
using azzs::adapters::windows::WindowsDisplayConnection;
using azzs::adapters::windows::WindowsDisplayPhysicalSize;
using azzs::adapters::windows::WindowsGpuMemory;
using azzs::application::HardwareObservationCode;
using azzs::application::HardwareDisplayConnection;
using azzs::application::HardwareDeviceRecord;
using azzs::application::HardwareDeviceKind;
using azzs::application::HardwareDeviceStatus;
using azzs::application::HardwareStorageMedia;
using azzs::application::HardwareGpuComputeUnit;
using azzs::application::HardwareGpuType;

template <typename T>
concept exposes_stable_instance_key = requires(T record) {
  record.stable_instance_key;
};

static_assert(!exposes_stable_instance_key<HardwareDeviceRecord>);
static_assert(!exposes_stable_instance_key<WindowsDisplayEdid>);
static_assert(!exposes_stable_instance_key<WindowsDisplayConnection>);
static_assert(!exposes_stable_instance_key<WindowsDisplayPhysicalSize>);

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
  std::optional<WindowsCpuTopology> topology;
  std::vector<WindowsGpuMemory> dxgi_adapters;
  std::vector<WindowsDisplayEdid> edids;
  std::vector<WindowsDisplayConnection> connections;
  std::vector<WindowsDisplayPhysicalSize> physical_sizes;
  std::vector<std::string> requested_display_pnp_ids;
  bool throw_display_edids{false};
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

  [[nodiscard]] std::optional<WindowsCpuTopology> cpu_topology(
      std::stop_token) override {
    return topology;
  }

  [[nodiscard]] std::vector<WindowsGpuMemory> gpu_memory(
      std::stop_token) override {
    return dxgi_adapters;
  }

  [[nodiscard]] std::vector<WindowsDisplayEdid> display_edids(
      std::span<std::string const> pnp_device_ids,
      std::stop_token) override {
    if (throw_display_edids) {
      throw std::runtime_error{"display EDID unavailable"};
    }
    requested_display_pnp_ids.assign(pnp_device_ids.begin(),
                                     pnp_device_ids.end());
    return edids;
  }

  [[nodiscard]] std::vector<WindowsDisplayConnection> display_connections(
      std::span<std::string const>, std::stop_token) override {
    return connections;
  }

  [[nodiscard]] std::vector<WindowsDisplayPhysicalSize> display_physical_sizes(
      std::span<std::string const>, std::stop_token) override {
    return physical_sizes;
  }
};

[[nodiscard]] std::vector<std::uint8_t> range_limit_edid(
    std::uint8_t minimum_vertical_hz, std::uint8_t maximum_vertical_hz,
    std::uint8_t range_offset_flags = 0, std::uint8_t horizontal_cm = 0,
    std::uint8_t vertical_cm = 0) {
  std::vector<std::uint8_t> edid(128, 0);
  std::uint8_t const header[]{0x00, 0xff, 0xff, 0xff,
                             0xff, 0xff, 0xff, 0x00};
  std::ranges::copy(header, edid.begin());
  edid[18] = 1;
  edid[19] = 4;
  edid[21] = horizontal_cm;
  edid[22] = vertical_cm;
  constexpr std::size_t descriptor = 54;
  edid[descriptor + 3] = 0xfd;
  edid[descriptor + 4] = range_offset_flags;
  edid[descriptor + 5] = minimum_vertical_hz;
  edid[descriptor + 6] = maximum_vertical_hz;
  std::uint32_t checksum = 0;
  for (std::size_t index = 0; index < 127; ++index) {
    checksum += edid[index];
  }
  edid[127] = static_cast<std::uint8_t>((256 - checksum % 256) % 256);
  return edid;
}

[[nodiscard]] bool line_separated(std::string const& value) {
  return value.find('\n') != std::string::npos &&
         value.find("; ") == std::string::npos;
}

[[nodiscard]] std::vector<ExpectedQuery> full_queries() {
  return {
      {"Win32_Processor",
       {"Name", "Manufacturer", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode", "NumberOfCores",
        "NumberOfLogicalProcessors"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"AMD Ryzen 7", "AuthenticAMD",
                  "ACPI\\AuthenticAMD_0000", "OK", "0", "8", "16"}}}},
      {"Win32_VideoController",
       {"Name", "AdapterCompatibility", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode", "VideoProcessor", "AdapterRAM"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"NVIDIA GeForce RTX", "NVIDIA",
                  "PCI\\VEN_10DE&DEV_0001", "OK", "0", "RTX",
                  "17179869184"},
                 {"AMD Radeon", "AMD", "PCI\\VEN_1002&DEV_0002", "OK",
                  "0", "Radeon", "4294967296"}}}},
      {"Win32_BaseBoard",
       {"Manufacturer", "Product", "HostingBoard", "Status"},
       {.code = WindowsHardwareQueryCode::succeeded,
         .rows = {{"ASUS", "PRIME B650", "TRUE", "OK"}}}},
      {"Win32_NetworkAdapter",
       {"Name", "Manufacturer", "AdapterType", "PhysicalAdapter",
        "PNPDeviceID", "Status", "ConfigManagerErrorCode", "ServiceName",
        "NetConnectionStatus"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"Intel Ethernet", "Intel", "Ethernet 802.3", "TRUE",
                  "PCI\\VEN_8086&DEV_0003", "OK", "0", "e1iexpress", "2"},
                 {"Intel(R) Wi-Fi 7 BE200", "Intel", "IEEE 802.11", "TRUE",
                  "PCI\\VEN_8086&DEV_0004", "OK", "0", "Netwtw", "2"}}}},
      {"Win32_ComputerSystem", {"Manufacturer", "Model"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"ASUS", "ROG"}}}},
      {"Win32_PhysicalMemory",
       {"Manufacturer", "Capacity", "Speed", "PartNumber",
        "DeviceLocator", "Tag", "SMBIOSMemoryType", "ConfiguredClockSpeed"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"Micron", "25769803776", "5600", "MTC20C2085S1EC48BA1",
                  "DIMM0", "Physical Memory 0", "34", "5600"},
                 {"Micron", "25769803776", "5600", "MTC20C2085S1EC48BA1",
                  "DIMM1", "Physical Memory 1", "34", "5600"}}}},
      {"Win32_DesktopMonitor",
       {"Name", "PNPDeviceID", "Status", "ConfigManagerErrorCode",
        "ScreenWidth", "ScreenHeight"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"BOE Display", "DISPLAY\\BOE1234\\1", "OK", "0", "2560",
                  "1600"},
                 {"BOE Display", "DISPLAY\\BOE1234\\2", "OK", "0", "2560",
                  "1600"}}}},
      {"Win32_DiskDrive",
       {"Model", "Manufacturer", "Size", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode", "InterfaceType", "MediaType"},
       {.code = WindowsHardwareQueryCode::succeeded,
         .rows = {{"(标准磁盘驱动器) PC801 SK hynix", "SK hynix", "1099511627776",
                   "SCSI\\DISK&VEN_NVME&PROD_PC801", "OK", "0", "SCSI", "Fixed hard disk media"},
                  {"(Standard disk drive) Samsung SSD 990 PRO", "Samsung", "2199023255552",
                   "PCI\\VEN_144D&DEV_A80A", "OK", "0", "NVMe", "SSD"},
                  {"(标准磁盘驱动器) Seagate BarraCuda HDD", "Seagate", "1099511627776",
                   "SCSI\\DISK_SEAGATE", "OK", "0", "SCSI", "Fixed hard disk media"}}}},
      {"Win32_SoundDevice",
       {"Name", "Manufacturer", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"Realtek High Definition Audio", "Realtek",
                  "HDAUDIO\\FUNC_01&VEN_10EC", "OK", "0"},
                 {"NVIDIA High Definition Audio", "NVIDIA",
                  "HDAUDIO\\FUNC_01&VEN_10DE", "OK", "0"}}}},
      {"Win32_PnPEntity",
       {"Name", "Manufacturer", "PNPDeviceID", "Status",
        "ConfigManagerErrorCode", "PNPClass"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"Intel(R) AI Boost", "Intel",
                  "PCI\\VEN_8086&DEV_7D1D", "OK", "0", "ComputeAccelerator"},
                 {"Microsoft Remote Display Adapter", "Microsoft",
                  "ROOT\\RDP_MF", "OK", "0", "Display"}}}},
      {"Win32_OperatingSystem", {"Caption", "Version", "BuildNumber", "OSArchitecture"},
       {.code = WindowsHardwareQueryCode::succeeded,
        .rows = {{"Microsoft Windows 11 Pro", "10.0.26100", "26100", "64-bit"}}}},
  };
}

[[nodiscard]] bool complete_read_only_model_facts_are_mapped() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->dxgi_adapters = {
      {.model_name = "NVIDIA GeForce RTX",
       .dedicated_video_memory = 16ull * 1024ull * 1024ull * 1024ull},
      {.model_name = "AMD Radeon",
       .dedicated_video_memory = 4ull * 1024ull * 1024ull * 1024ull},
  };
  WindowsHardwareObserver observer{std::move(executor)};

  auto const result = observer.observe({});
  return expect(result.code == HardwareObservationCode::succeeded &&
                    result.observation.has_value(),
                "complete WMI facts must produce a successful observation") &&
          expect(!raw_executor->mismatch && raw_executor->calls == 11,
                 "the adapter must use exactly the approved read-only "
                 "model queries") &&
                 expect(result.observation->cpu.find("8核心16线程") !=
                             std::string::npos &&
                     result.observation->gpu.find("英伟达 NVIDIA") !=
                         std::string::npos &&
                     result.observation->gpu.find("专用显存 16 GB") !=
                         std::string::npos &&
                     result.observation->gpu.find("专用显存 4 GB") !=
                         std::string::npos &&
                     result.observation->motherboard.find("华硕 ASUS") !=
                         std::string::npos &&
                     result.observation->motherboard.find("PRIME B650") !=
                         std::string::npos &&
                     result.observation->wired_network_adapter.find("英特尔 Intel") !=
                         std::string::npos &&
                     result.observation->wireless_network_adapter.find("英特尔 Intel") !=
                         std::string::npos &&
                     result.observation->memory.find("美光 Micron") !=
                         std::string::npos &&
                     result.observation->memory.find("48GB 5600MHz (24GB + 24GB)") !=
                         std::string::npos &&
                      result.observation->display.find("京东方 BOE") !=
                          std::string::npos &&
                       result.observation->display.find(
                           "京东方 BOE Display（2560 × 1600；刷新率未识别；内建/外接未识别；英寸未识别）") !=
                           std::string::npos &&
                       result.observation->display.find("未知") ==
                           std::string::npos &&
                      result.observation->display.find(" EDID") ==
                          std::string::npos &&
                     result.observation->storage.find("PC801 SK 海力士") !=
                         std::string::npos &&
                     result.observation->storage.find("三星 Samsung SSD 990 PRO") !=
                         std::string::npos &&
                     result.observation->storage.find("接口：") ==
                         std::string::npos &&
                     result.observation->storage.find("PCIe") ==
                         std::string::npos &&
                     result.observation->storage.find("NAND") ==
                         std::string::npos &&
                     result.observation->storage.find("资料识别") ==
                         std::string::npos &&
                     result.observation->storage.find("标准磁盘驱动器") ==
                         std::string::npos &&
                     result.observation->storage.find("Standard disk drive") ==
                         std::string::npos &&
                     result.observation->solid_state_storage.find("Samsung SSD 990 PRO") !=
                         std::string::npos &&
                     result.observation->solid_state_storage.find("PC801 SK") !=
                         std::string::npos &&
                     result.observation->hard_disk_storage.find("PC801 SK") ==
                         std::string::npos &&
                     result.observation->hard_disk_storage.find("Seagate BarraCuda HDD") !=
                         std::string::npos &&
                     result.observation->audio.find("瑞昱 Realtek") !=
                         std::string::npos &&
                      result.observation->npu.find("英特尔 Intel") !=
                          std::string::npos &&
                      line_separated(result.observation->gpu) &&
                      line_separated(result.observation->solid_state_storage) &&
                      line_separated(result.observation->audio) &&
                      result.observation->motherboard.find('\n') ==
                          std::string::npos &&
                     result.observation->operating_system.find("24H2") !=
                         std::string::npos &&
                     result.observation->operating_system.find("版本 10.0.26100") !=
                         std::string::npos &&
                     result.observation->oem_model == "华硕 ASUS ROG" &&
                     std::ranges::any_of(
                         result.observation->devices, [](auto const& device) {
                           return device.kind == HardwareDeviceKind::cpu &&
                                  device.core_count == 8 &&
                                  device.thread_count == 16;
                         }) &&
                      result.observation->devices.size() == 15 &&
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
      {"ASUS", "PRIME B650", "TRUE", "Disabled"}};
  raw_executor->expected[3].result.rows = {
      {"Intel Ethernet", "Intel", "Ethernet 802.3", "TRUE",
       "PCI\\VEN_8086&DEV_0003", "Error", "10", "e1iexpress", "2"}};
  raw_executor->expected[5].result.rows = {};
  raw_executor->expected[6].result.rows = {};
  raw_executor->expected[7].result.rows = {};
  raw_executor->expected[8].result.rows = {};
  raw_executor->expected[9].result.rows = {};
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
      {"Microsoft", "Virtual Machine", "TRUE", "OK"}};
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
  raw_executor->expected[5].result.rows = {};
  raw_executor->expected[6].result.rows = {};
  raw_executor->expected[7].result.rows = {};
  raw_executor->expected[8].result.rows = {};
  raw_executor->expected[9].result.rows = {};
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
                    result.observation->cpu.find("8核心16线程") !=
                        std::string::npos,
                "a non-terminal probe failure must return usable partial facts") &&
          expect(!raw_executor->mismatch && raw_executor->calls == 11,
                "a partial failure must still collect independent model facts");
}

[[nodiscard]] bool oem_firmware_fallbacks_keep_concrete_models() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[0].result.rows = {
      {"Intel(R) Core(TM) Ultra 9", "GenuineIntel", "", "OK", "0", "24", "24"}};
  raw_executor->expected[2].result.rows = {
      {"HP", "8A43", "true", "OK"}};
  raw_executor->expected[6].result.rows = {
      {"Generic PnP Monitor", "DISPLAY\\BOE1234\\1", "OK", "0", "2560", "1600"}};
  raw_executor->expected[9].result.rows = {
      {"BOE NE160QDM-NY4", "BOE", "DISPLAY\\BOE1234\\1", "Disabled", "22", "Monitor"},
      {"Intel(R) AI Boost", "Intel", "PCI\\VEN_8086&DEV_7D1D", "OK", "0",
       "ComputeAccelerator"},
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->cpu.find("24核心24线程") !=
                        std::string::npos &&
                    result.observation->motherboard == "惠普 HP 8A43" &&
                    result.observation->display.find("京东方 BOE") !=
                        std::string::npos &&
                    result.observation->display.find("2560 × 1600") !=
                        std::string::npos &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::display &&
                                 device.status == HardwareDeviceStatus::disabled;
                        }) &&
                    !raw_executor->mismatch && raw_executor->calls == 11,
                "missing processor PNP ids, a concrete hosted board, and generic monitor rows must use concrete OEM WMI fallbacks");
}

[[nodiscard]] bool incomplete_board_rows_fail_closed() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[2].result.rows = {
      {"HP", "", "true", "OK"},
      {"", "8D41", "true", "OK"},
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->motherboard.empty() &&
                    !std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind ==
                                 HardwareDeviceKind::motherboard;
                        }) &&
                    !raw_executor->mismatch && raw_executor->calls == 11,
                "a baseboard without both manufacturer and product must not "
                "be promoted from an empty-PNP WMI row");
}

[[nodiscard]] bool real_machine_wmi_field_shapes_are_projected() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[0].result.rows = {
      {"Intel(R) Core(TM) Ultra 9 275HX", "GenuineIntel", "", "OK", "",
       "24", "24"}};
  raw_executor->expected[2].result.rows = {
      {"HP", "8D41", "-1", "OK"}};
  raw_executor->expected[3].result.rows = {
      {"Realtek Gaming 2.5GbE Family Controller", "Realtek",
       "以太网 802.3", "True",
       "PCI\\VEN_10EC&DEV_8125&SUBSYS_8D41103C&REV_05\\01000000684CE00000",
       "", "0", "rt25cx21", "2"},
      {"Intel(R) Wi-Fi 7 BE200 320MHz", "Intel Corporation", "以太网 802.3",
       "True", "PCI\\VEN_8086&DEV_272B&SUBSYS_00F48086&REV_1A\\4&2354814&0&0031",
       "", "0", "Netwaw18", "7"}};
  raw_executor->expected[5].result.rows = {
      {"Micron Technology", "25769803776", "5600", "CT24G56C46S5.M8B1   ",
       "Bottom-Slot 1(left)", "Physical Memory 0", "34", "5600"},
      {"Micron Technology", "25769803776", "5600", "CT24G56C46S5.M8B1   ",
       "Bottom-Slot 2(right)", "Physical Memory 1", "34", "5600"}};
  raw_executor->expected[6].result.rows = {
      {"默认监视器", "", "OK", "", "", ""},
      {"通用即插即用监视器", "DISPLAY\\LHC907D\\5&18F24B9&0&UID8449", "OK",
       "0", "3840", "2160"},
      {"通用即插即用监视器", "DISPLAY\\BOE0CD1\\4&1739F381&1&UID8388688", "OK",
       "0", "2560", "1600"}};
  raw_executor->expected[7].result.rows = {
      {"Samsung SSD 990 PRO 2TB 1863GB", "(标准磁盘驱动器)", "2000396321280",
       "SCSI\\DISK&VEN_NVME&PROD_SAMSUNG_SSD_990\\5&211D8CE7&0&000000", "OK",
       "0", "SCSI", "Fixed hard disk media"},
      {"SK hynix PCB01 HFS001TFM9X187N", "(标准磁盘驱动器)", "1024203640320",
       "SCSI\\DISK&VEN_NVME&PROD_SK_HYNIX_PCB01_H\\5&24C09446&0&000000", "OK",
       "0", "SCSI", "Fixed hard disk media"},
      {"SK hynix PCB01 HFS999TFM9X187N", "(标准磁盘驱动器)", "1024203640320",
       "SCSI\\DISK&VEN_NVME&PROD_SK_HYNIX_PCB01_X\\5&24C09446&0&000001", "OK",
       "0", "SCSI", "Fixed hard disk media"}};
  raw_executor->expected[9].result.rows = {
      {"Integrated Monitor", "(标准监视器类型)",
       "DISPLAY\\BOE0CD1\\4&1739F381&1&UID8388688", "OK", "0", "Monitor"},
      {"Generic Monitor (P275MV PLUS)", "(标准监视器类型)",
       "DISPLAY\\LHC907D\\5&18F24B9&0&UID8449", "OK", "0", "Monitor"}};
  raw_executor->expected[10].result.rows = {
      {"Microsoft Windows 11 专业版", "10.0.26200", "26200", "64-bit"}};
  raw_executor->topology = WindowsCpuTopology{
      .performance_cores = 8,
      .efficiency_cores = 16,
      .logical_processors = 24,
      .split_known = true,
  };
  raw_executor->edids = {
      {.model_key = "boe0cd1",
       .bytes = range_limit_edid(60, 240, 0x0c, 34, 22)},
      {.model_key = "lhc907d",
       .bytes = range_limit_edid(48, 160, 0, 60, 34)},
  };
  raw_executor->connections = {
      {.model_key = "boe0cd1",
       .connection = HardwareDisplayConnection::internal,
       .width = 2560,
       .height = 1600,
       .refresh_rate_hz = 240},
      {.model_key = "lhc907d",
       .connection = HardwareDisplayConnection::external,
       .width = 3840,
       .height = 2160,
       .refresh_rate_hz = 160},
  };

  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  auto const& observation = result.observation;
  auto refresh_fingerprint_changed = false;
  if (observation.has_value()) {
    auto changed = *observation;
    for (auto& device : changed.devices) {
      if (device.kind == HardwareDeviceKind::display) {
        ++device.physical_refresh_rate_limit_hz;
        refresh_fingerprint_changed =
            changed.model_fingerprint() != observation->model_fingerprint();
        break;
      }
    }
  }
  return expect(result.succeeded() && observation.has_value() &&
                    !raw_executor->mismatch && raw_executor->calls == 11,
                "real WMI field shapes must satisfy the approved query contract") &&
         expect(observation->cpu.find(
                    "24核心24线程；8性能核+16能效核+0低功耗能效核") !=
                         std::string::npos &&
                     observation->motherboard == "惠普 HP 8D41" &&
                     observation->wired_network_adapter.find("瑞昱 Realtek") !=
                         std::string::npos &&
                     observation->wireless_network_adapter.find("英特尔 Intel") !=
                         std::string::npos &&
                     observation->operating_system.find("25H2") !=
                         std::string::npos &&
                     observation->operating_system.find("版本 10.0.26200") !=
                         std::string::npos,
                "an empty CPU PNP id, concrete hosted board, and localized network types must retain physical models") &&
          expect(observation->memory.find(
                      "英睿达 Crucial DDR5 48GB 5600MHz (24GB + 24GB)") !=
                     std::string::npos,
                "matching physical DIMMs must be aggregated by part number") &&
         expect(observation->display.find(
                         "京东方 BOE0CD1（2560 × 1600；240 Hz；内建；15.9 英寸）") !=
                         std::string::npos &&
                     observation->display.find(
                         "泰坦军团 TITAN ARMY P275MV PLUS（3840 × 2160；160 Hz；外接；27.2 英寸）") !=
                         std::string::npos &&
                     observation->display.find("EDID") == std::string::npos &&
                     observation->display.find("刷新率") == std::string::npos &&
                     line_separated(observation->display) &&
                     observation->display.find("默认监视器") == std::string::npos,
                "active DisplayConfig facts must enrich physical monitor names without labels or desktop modes") &&
         expect(observation->solid_state_storage.find("Samsung SSD 990 PRO 2TB") !=
                         std::string::npos &&
                     observation->solid_state_storage.find("1863GB") ==
                         std::string::npos &&
                     observation->solid_state_storage.find(
                         "SK 海力士 PCB01 HFS001TFM9X187N") !=
                          std::string::npos &&
                     observation->solid_state_storage.find(
                         "SK 海力士 PCB01 HFS999TFM9X187N 1TB") !=
                         std::string::npos &&
                     observation->solid_state_storage.find(
                         "接口：") == std::string::npos &&
                     observation->solid_state_storage.find("PCIe") ==
                         std::string::npos &&
                     observation->solid_state_storage.find("NAND") ==
                         std::string::npos &&
                     observation->solid_state_storage.find("资料识别") ==
                         std::string::npos &&
                     observation->hard_disk_storage.empty() &&
                     observation->storage.find("标准磁盘驱动器") ==
                         std::string::npos &&
                     line_separated(observation->solid_state_storage) &&
                     raw_executor->requested_display_pnp_ids.size() == 2 &&
                     refresh_fingerprint_changed,
                    "only the exact PCB01 product and validated raw EDID model keys may receive model-specific facts");
}

[[nodiscard]] bool displayconfig_facts_are_authoritative_and_fail_closed() {
  auto render = [](std::vector<WindowsDisplayConnection> connections,
                   std::vector<WindowsDisplayEdid> edids,
                   bool wmi_dimensions_known) {
    auto executor = std::make_unique<FakeQueryExecutor>();
    executor->expected = full_queries();
    executor->expected[6].result.rows = {
        {"Generic PnP Monitor", "DISPLAY\\BOE0CD1\\1", "OK", "0",
         wmi_dimensions_known ? "2560" : "", wmi_dimensions_known ? "1600" : ""}};
    executor->expected[9].result.rows = {
        {"Integrated Monitor", "(标准监视器类型)", "DISPLAY\\BOE0CD1\\1", "OK",
         "0", "Monitor"}};
    executor->connections = std::move(connections);
    executor->edids = std::move(edids);
    WindowsHardwareObserver observer{std::move(executor)};
    auto const result = observer.observe({});
    return result.observation;
  };

  auto const no_mapping = render({}, {}, false);
  auto const active_overrides_edid = render(
      {{.model_key = "boe0cd1",
        .connection = HardwareDisplayConnection::internal,
        .width = 2560,
        .height = 1600,
        .refresh_rate_hz = 144}},
      {{.model_key = "boe0cd1",
        .bytes = range_limit_edid(60, 240, 0, 60, 34)}}, false);
  auto const conflicting_paths = render(
      {{.model_key = "boe0cd1",
        .connection = HardwareDisplayConnection::internal,
        .width = 2560,
        .height = 1600,
        .refresh_rate_hz = 144},
       {.model_key = "boe0cd1",
        .connection = HardwareDisplayConnection::external,
        .width = 2560,
        .height = 1600,
        .refresh_rate_hz = 144}},
      {}, false);
  auto const explicit_instance_mismatch = render(
      {{.model_key = "boe0cd1",
        .instance_ordinal = 2,
        .connection = HardwareDisplayConnection::internal,
        .width = 2560,
        .height = 1600,
        .refresh_rate_hz = 144}},
      {}, true);

  auto const find_display = [](std::optional<azzs::application::HardwareObservation> const& observation) {
    if (!observation.has_value()) {
      return static_cast<azzs::application::HardwareDeviceRecord const*>(nullptr);
    }
    for (auto const& device : observation->devices) {
      if (device.kind == HardwareDeviceKind::display) {
        return &device;
      }
    }
    return static_cast<azzs::application::HardwareDeviceRecord const*>(nullptr);
  };
  auto const* active_device = find_display(active_overrides_edid);
  auto const* conflicting_device = find_display(conflicting_paths);
  auto const* mismatched_device = find_display(explicit_instance_mismatch);

  return expect(no_mapping.has_value() &&
                    no_mapping->display.find(
                        "京东方 BOE0CD1（分辨率未识别；刷新率未识别；内建/外接未识别；英寸未识别）") !=
                        std::string::npos,
                "without an active DisplayConfig mapping, every display fact slot must stay explicitly unrecognised") &&
         expect(active_overrides_edid.has_value() && active_device != nullptr &&
                     active_overrides_edid->display.find(
                        "京东方 BOE0CD1（2560 × 1600；144 Hz；内建；27.2 英寸）") != std::string::npos &&
                    active_overrides_edid->display.find("240 Hz") == std::string::npos &&
                    active_device->display_width == 2560 &&
                    active_device->display_height == 1600 &&
                    active_device->display_refresh_rate_hz == 144 &&
                    active_device->physical_refresh_rate_limit_hz == 240,
                "active DisplayConfig refresh must override the EDID capability limit and fill missing WMI dimensions") &&
         expect(conflicting_paths.has_value() && conflicting_device != nullptr &&
                     conflicting_paths->display.find(
                         "京东方 BOE0CD1（2560 × 1600；144 Hz；内建/外接未识别；英寸未识别）") !=
                         std::string::npos &&
                    conflicting_device->display_connection ==
                        HardwareDisplayConnection::unknown,
                "conflicting DisplayConfig paths must mark only the unreliable display fields unrecognised") &&
         expect(explicit_instance_mismatch.has_value() && mismatched_device != nullptr &&
                    explicit_instance_mismatch->display.find(
                        "京东方 BOE0CD1（2560 × 1600；刷新率未识别；内建/外接未识别；英寸未识别）") !=
                        std::string::npos &&
                    explicit_instance_mismatch->display.find("144 Hz") ==
                        std::string::npos &&
                    mismatched_device->display_connection ==
                        HardwareDisplayConnection::unknown,
                "an explicit DisplayConfig instance mismatch must not fall back to a same-model WMI record");
}

[[nodiscard]] bool topology_and_dxgi_enrich_only_verified_models() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[0].result.rows = {
      {"Intel(R) Core(TM) Ultra 9 275HX", "GenuineIntel", "", "OK", "0",
       "24", "24"}};
  raw_executor->expected[1].result.rows = {
      {"NVIDIA GeForce RTX 5080 Laptop GPU", "NVIDIA",
       "PCI\\VEN_10DE&DEV_1F00&SUBSYS_12341043", "OK", "0", "",
       "4293918720"},
      {"Intel Graphics", "Intel", "PCI\\VEN_8086&DEV_7D55", "OK", "0",
       "", "4293918720"}};
  raw_executor->topology = WindowsCpuTopology{
      .performance_cores = 8,
      .efficiency_cores = 16,
      .logical_processors = 24,
      .split_known = true,
  };
  raw_executor->dxgi_adapters = {
      {.model_name = "NVIDIA GeForce RTX 5080 Laptop GPU",
       .dedicated_video_memory = 16003ull * 1024ull * 1024ull},
      {.model_name = "Intel Graphics",
       .dedicated_video_memory = 128ull * 1024ull * 1024ull,
       .shared_system_memory = 32175ull * 1024ull * 1024ull},
  };
  raw_executor->throw_display_edids = true;
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value(),
                "verified DXGI and topology facts must enrich a complete observation") &&
         expect(result.observation->cpu.find(
                    "8性能核+16能效核+0低功耗能效核") !=
                    std::string::npos &&
                    result.observation->gpu.find("专用显存 16 GB") !=
                        std::string::npos &&
                    result.observation->gpu.find(
                        "核显；专用显存 128 MB；共享内存 31.4 GB") !=
                        std::string::npos &&
                    result.observation->gpu.find("专用显存 4 GB") ==
                        std::string::npos &&
                    result.observation->gpu.find("华硕 ASUS") !=
                        std::string::npos &&
                    result.observation->cpu.find("(R)") ==
                        std::string::npos &&
                    result.observation->cpu.find("(TM)") ==
                        std::string::npos &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 device.core_count == 24 &&
                                 device.thread_count == 24 &&
                                 device.performance_core_count == 8 &&
                                 device.efficiency_core_count == 16 &&
                                 device.low_power_efficiency_core_count == 0;
                        }) &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::gpu &&
                                 device.gpu_type == HardwareGpuType::discrete &&
                                 device.gpu_shared_memory_bytes == 0 &&
                                 device.name.find("共享内存") == std::string::npos;
                        }) &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::gpu &&
                                 device.gpu_type == HardwareGpuType::integrated &&
                                 device.gpu_compute_unit == HardwareGpuComputeUnit::xe &&
                                 device.gpu_compute_unit_count == 8 &&
                                 device.gpu_shared_memory_bytes != 0;
                        }),
                "CPU P/E and GPU memory must use the platform facts, never the truncated WMI AdapterRAM value");
}

[[nodiscard]] bool gpu_model_families_are_expanded_without_guessing() {
  auto render = [](std::string name, std::string compatibility,
                   std::string video_processor, std::string pnp_id) {
    auto executor = std::make_unique<FakeQueryExecutor>();
    executor->expected = full_queries();
    executor->expected[1].result.rows = {{std::move(name),
                                          std::move(compatibility),
                                          std::move(pnp_id), "OK", "0",
                                          std::move(video_processor), "0"}};
    executor->dxgi_adapters.clear();
    WindowsHardwareObserver observer{std::move(executor)};
    auto const result = observer.observe({});
    return result.observation.has_value() ? result.observation->gpu
                                           : std::string{};
  };

  auto const intel = render("Intel(R) Graphics", "Intel Corporation",
                            "Intel(R) Graphics Family",
                            "PCI\\VEN_8086&DEV_7D67");
  auto const amd = render("AMD Radeon 780M Graphics", "Advanced Micro Devices",
                          "AMD Radeon 780M Graphics",
                          "PCI\\VEN_1002&DEV_164E");
  auto const unknown = render("AMD Radeon Graphics", "AMD",
                              "AMD Radeon Graphics",
                              "PCI\\VEN_1002&DEV_FFFF");
  auto low_end_discrete = render("AMD Radeon Graphics", "AMD",
                                 "AMD Radeon Graphics",
                                 "PCI\\VEN_1002&DEV_9999");
  return expect(intel.find("英特尔 Intel Arc Graphics") == std::string::npos &&
                    intel.find("4 Xe") == std::string::npos &&
                    intel.find("核显") == std::string::npos &&
                    intel.find("英特尔 Intel Graphics") != std::string::npos &&
                    amd.find("AMD Radeon 780M Graphics") != std::string::npos &&
                    amd.find("12 CU") != std::string::npos &&
                    unknown.find("计算单元未读取") != std::string::npos &&
                    low_end_discrete.find("核显") == std::string::npos,
                "Intel, AMD and unknown GPU facts must remain tied to authoritative IDs or exact model text");
}

[[nodiscard]] bool core_ultra_9_275hx_intel_graphics_requires_exact_identity() {
  auto observe = [](std::string cpu_name, std::string pnp_id) {
    auto executor = std::make_unique<FakeQueryExecutor>();
    auto* raw_executor = executor.get();
    raw_executor->expected = full_queries();
    raw_executor->expected[0].result.rows = {
        {std::move(cpu_name), "GenuineIntel", "", "OK", "0", "24", "24"}};
    raw_executor->expected[1].result.rows = {
        {"Intel(R) Graphics", "Intel", std::move(pnp_id), "OK", "0",
         "Intel(R) Graphics Family", "0"}};
    raw_executor->dxgi_adapters = {
        {.model_name = "Intel Graphics",
         .dedicated_video_memory = 128ull * 1024ull * 1024ull,
         .shared_system_memory = 32175ull * 1024ull * 1024ull},
    };
    WindowsHardwareObserver observer{std::move(executor)};
    return observer.observe({});
  };
  auto const find_gpu = [](auto const& result) -> HardwareDeviceRecord const* {
    if (!result.observation.has_value()) {
      return nullptr;
    }
    for (auto const& device : result.observation->devices) {
      if (device.kind == HardwareDeviceKind::gpu) {
        return &device;
      }
    }
    return nullptr;
  };

  auto const exact = observe(
      "Intel(R) Core(TM) Ultra 9 275HX",
      "PCI\\VEN_8086&DEV_7D67&SUBSYS_8D41103C");
  auto const bare_device = observe("Intel(R) Core(TM) Ultra 9 275HX",
                                   "PCI\\VEN_8086&DEV_7D67");
  auto const wrong_cpu = observe(
      "Intel(R) Core(TM) Ultra 9 285H",
      "PCI\\VEN_8086&DEV_7D67&SUBSYS_8D41103C");
  auto const* exact_gpu = find_gpu(exact);
  auto const* bare_device_gpu = find_gpu(bare_device);
  auto const* wrong_cpu_gpu = find_gpu(wrong_cpu);

  return expect(exact.succeeded() && exact_gpu != nullptr &&
                    exact.observation->gpu.find("英特尔 Intel Graphics（核显；") !=
                        std::string::npos &&
                    exact.observation->gpu.find("4 Xe 核") != std::string::npos &&
                    exact.observation->gpu.find("共享内存 31.4 GB") !=
                        std::string::npos &&
                    exact.observation->gpu.find("Arc") == std::string::npos &&
                    exact.observation->gpu.find("UHD") == std::string::npos &&
                    exact.observation->gpu.find("惠普 HP") == std::string::npos &&
                    exact_gpu->gpu_type == HardwareGpuType::integrated &&
                    exact_gpu->gpu_compute_unit == HardwareGpuComputeUnit::xe &&
                    exact_gpu->gpu_compute_unit_count == 4 &&
                    exact_gpu->gpu_shared_memory_bytes != 0,
                "the verified 275HX and HP PCI identity must project Intel Graphics with four Xe cores") &&
         expect(bare_device.succeeded() && bare_device_gpu != nullptr &&
                    bare_device_gpu->gpu_type == HardwareGpuType::unknown &&
                    bare_device.observation->gpu.find("4 Xe") == std::string::npos &&
                    bare_device.observation->gpu.find("核显") == std::string::npos,
                "a bare 7d67 device id must not inherit the verified Intel Graphics mapping") &&
         expect(wrong_cpu.succeeded() && wrong_cpu_gpu != nullptr &&
                    wrong_cpu_gpu->gpu_type == HardwareGpuType::unknown &&
                    wrong_cpu.observation->gpu.find("4 Xe") == std::string::npos &&
                    wrong_cpu.observation->gpu.find("核显") == std::string::npos,
                "the 7d67 subsystem also requires the exact verified CPU model");
}

[[nodiscard]] bool display_instances_are_not_merged_by_model_name() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[6].result.rows = {
      {"BOE Display", "DISPLAY\\BOE1234\\1", "OK", "0", "2560", "1600"},
      {"BOE Display", "DISPLAY\\BOE1234\\2", "OK", "0", "2560", "1600"},
  };
  raw_executor->expected[9].result.rows = {
      {"BOE Display", "BOE", "DISPLAY\\BOE1234\\1", "OK", "0", "Monitor"},
      {"BOE Display", "BOE", "DISPLAY\\BOE1234\\2", "OK", "0", "Monitor"},
  };
  raw_executor->connections = {
      {.model_key = "boe1234",
       .instance_ordinal = 1,
       .connection = HardwareDisplayConnection::internal,
       .width = 2560,
       .height = 1600,
       .refresh_rate_hz = 144},
      {.model_key = "boe1234",
       .instance_ordinal = 2,
       .connection = HardwareDisplayConnection::external,
       .width = 3840,
       .height = 2160,
       .refresh_rate_hz = 60},
  };
  raw_executor->edids = {
      {.model_key = "boe1234",
       .instance_ordinal = 1,
       .bytes = range_limit_edid(60, 144, 0, 60, 34)},
      {.model_key = "boe1234",
       .instance_ordinal = 2,
       .bytes = range_limit_edid(60, 60, 0, 34, 22)},
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  if (!expect(result.succeeded() && result.observation.has_value(),
              "two concrete display instances must remain observable")) {
    return false;
  }
  std::size_t display_count = 0;
  bool saw_internal_panel = false;
  bool saw_external_panel = false;
  bool all_singleton = true;
  for (auto const& device : result.observation->devices) {
    if (device.kind != HardwareDeviceKind::display) {
      continue;
    }
    ++display_count;
    all_singleton &= device.quantity == 1;
    saw_internal_panel |= device.display_connection ==
                              HardwareDisplayConnection::internal &&
                          device.display_width == 2560 &&
                          device.display_height == 1600 &&
                          device.display_refresh_rate_hz == 144 &&
                          device.display_size_tenths_inch == 272;
    saw_external_panel |= device.display_connection ==
                              HardwareDisplayConnection::external &&
                          device.display_width == 3840 &&
                          device.display_height == 2160 &&
                          device.display_refresh_rate_hz == 60 &&
                          device.display_size_tenths_inch == 159;
  }
  return expect(display_count == 2 && saw_internal_panel &&
                    saw_external_panel && all_singleton &&
                    result.observation->display.find('\n') != std::string::npos &&
                    result.observation->display.find(" x2") == std::string::npos,
                "same-model displays must use their request ordinals, not merge or exchange facts");
}

[[nodiscard]] bool cpu_core_classes_are_unrecognised_without_reliable_topology() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->topology.reset();
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->cpu.find("8核心16线程") != std::string::npos &&
                    result.observation->cpu.find(
                        "性能核未识别+能效核未识别+低功耗能效核未识别") !=
                        std::string::npos &&
                    result.observation->cpu.find("0性能核") == std::string::npos &&
                    result.observation->cpu.find("0能效核") == std::string::npos &&
                    result.observation->cpu.find("0低功耗能效核") ==
                        std::string::npos &&
                    std::ranges::none_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 (device.performance_core_count != 0 ||
                                  device.efficiency_core_count != 0 ||
                                  device.low_power_efficiency_core_count != 0);
                        }),
                "CPU class labels must stay unrecognised when Windows does not provide a reliable topology");
}

[[nodiscard]] bool cpu_single_efficiency_class_marks_core_classes_unrecognised() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  // The Windows adapter represents one EfficiencyClass as an available
  // homogeneous topology with no reliable P/E split.
  raw_executor->topology = WindowsCpuTopology{
      .logical_processors = 16,
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->cpu.find("8核心16线程") != std::string::npos &&
                    result.observation->cpu.find(
                        "性能核未识别+能效核未识别+低功耗能效核未识别") !=
                        std::string::npos &&
                    result.observation->cpu.find("0性能核") == std::string::npos &&
                    result.observation->cpu.find("0能效核") == std::string::npos &&
                    result.observation->cpu.find("0低功耗能效核") ==
                        std::string::npos &&
                    std::ranges::none_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 (device.performance_core_count != 0 ||
                                  device.efficiency_core_count != 0 ||
                                  device.low_power_efficiency_core_count != 0);
                        }),
                "a single Windows EfficiencyClass must leave every CPU class unrecognised");
}

[[nodiscard]] bool cpu_core_classes_are_unrecognised_when_counts_conflict() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->topology = WindowsCpuTopology{
      .performance_cores = 8,
      .efficiency_cores = 8,
      .logical_processors = 16,
      .split_known = true,
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->cpu.find("8核心16线程") != std::string::npos &&
                    result.observation->cpu.find(
                        "性能核未识别+能效核未识别+低功耗能效核未识别") !=
                        std::string::npos &&
                    result.observation->cpu.find("8性能核+8能效核") ==
                        std::string::npos &&
                    std::ranges::none_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 (device.performance_core_count != 0 ||
                                  device.efficiency_core_count != 0 ||
                                  device.low_power_efficiency_core_count != 0);
                        }),
                "a topology whose core total conflicts with WMI must not claim numeric CPU classes");
}

[[nodiscard]] bool cpu_three_core_classes_are_projected_when_consistent() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[0].result.rows = {
      {"Intel Core Ultra", "GenuineIntel", "", "OK", "0", "24", "24"}};
  raw_executor->topology = WindowsCpuTopology{
      .performance_cores = 8,
      .efficiency_cores = 12,
      .low_power_efficiency_cores = 4,
      .logical_processors = 24,
      .split_known = true,
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->cpu.find(
                        "8性能核+12能效核+4低功耗能效核") !=
                        std::string::npos &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 device.performance_core_count == 8 &&
                                 device.efficiency_core_count == 12 &&
                                 device.low_power_efficiency_core_count == 4;
                        }),
                "a consistent three-class Windows topology must retain every reported core class");
}

[[nodiscard]] bool cpu_two_core_classes_keep_a_real_zero_low_power_count() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[0].result.rows = {
      {"Intel Core Ultra", "GenuineIntel", "", "OK", "0", "16", "16"}};
  raw_executor->topology = WindowsCpuTopology{
      .performance_cores = 8,
      .efficiency_cores = 8,
      .low_power_efficiency_cores = 0,
      .logical_processors = 16,
      .split_known = true,
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->cpu.find(
                        "8性能核+8能效核+0低功耗能效核") !=
                        std::string::npos &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 device.performance_core_count == 8 &&
                                 device.efficiency_core_count == 8 &&
                                 device.low_power_efficiency_core_count == 0;
                        }),
                "a consistent two-class topology must retain zero as the real absent low-power class");
}

[[nodiscard]] bool storage_duplicates_require_stable_instance_identity() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[7].result.rows = {
      {"Samsung SSD 990 PRO", "Samsung", "2199023255552",
       "PCI\\VEN_144D&DEV_A80A", "OK", "0", "NVMe", "SSD"},
      {"Samsung SSD 990 PRO", "Samsung", "2199023255552",
       "PCI\\VEN_144D&DEV_A80A", "OK", "0", "NVMe", "SSD"},
      {"Samsung SSD 990 PRO", "Samsung", "2199023255552",
       "PCI\\VEN_144D&DEV_A80B", "OK", "0", "NVMe", "SSD"},
  };
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  if (!expect(result.succeeded() && result.observation.has_value(),
              "storage rows with stable PNP ids must remain observable")) {
    return false;
  }
  std::size_t storage_count = 0;
  bool all_singleton = true;
  for (auto const& device : result.observation->devices) {
    if (device.kind != HardwareDeviceKind::storage) {
      continue;
    }
    ++storage_count;
    all_singleton &= device.quantity == 1;
  }
  return expect(storage_count == 2 && all_singleton &&
                    result.observation->storage.find('\n') != std::string::npos &&
                    result.observation->storage.find(" x2") == std::string::npos,
                "only an exact stable storage instance may de-duplicate repeated rows");
}

[[nodiscard]] bool crucial_memory_brand_requires_confirmed_identity() {
  auto micron_executor = std::make_unique<FakeQueryExecutor>();
  auto* micron_raw = micron_executor.get();
  micron_raw->expected = full_queries();
  micron_raw->expected[5].result.rows = {
      {"Micron", "17179869184", "5600", "MTC16C2085S1EC48BA1", "DIMM0",
       "Physical Memory 0", "34", "5600"},
  };
  WindowsHardwareObserver micron_observer{std::move(micron_executor)};
  auto const micron = micron_observer.observe({});

  auto micron_crucial_executor = std::make_unique<FakeQueryExecutor>();
  auto* micron_crucial_raw = micron_crucial_executor.get();
  micron_crucial_raw->expected = full_queries();
  micron_crucial_raw->expected[5].result.rows = {
      {"Micron Technology", "25769803776", "5600", "CT24G56C46S5.M8B1",
       "Bottom-Slot 1(left)", "Physical Memory 0", "34", "5600"},
      {"Micron Technology", "25769803776", "5600", "CT24G56C46S5.M8B1",
       "Bottom-Slot 2(right)", "Physical Memory 1", "34", "5600"},
  };
  WindowsHardwareObserver micron_crucial_observer{
      std::move(micron_crucial_executor)};
  auto const micron_crucial = micron_crucial_observer.observe({});

  auto crucial_executor = std::make_unique<FakeQueryExecutor>();
  auto* crucial_raw = crucial_executor.get();
  crucial_raw->expected = full_queries();
  crucial_raw->expected[5].result.rows = {
      {"Crucial", "17179869184", "5600", "MTC16C2085S1EC48BA1", "DIMM0",
       "Physical Memory 0", "34", "5600"},
  };
  WindowsHardwareObserver crucial_observer{std::move(crucial_executor)};
  auto const crucial = crucial_observer.observe({});

  return expect(micron.observation.has_value() &&
                    micron.observation->memory.find("美光 Micron") !=
                        std::string::npos &&
                    micron.observation->memory.find("英睿达 Crucial") ==
                        std::string::npos &&
                    micron_crucial.observation.has_value() &&
                    micron_crucial.observation->memory.find(
                        "英睿达 Crucial DDR5 48GB 5600MHz (24GB + 24GB)") !=
                        std::string::npos &&
                    micron_crucial.observation->memory.find("美光 Micron") ==
                        std::string::npos &&
                    crucial.observation.has_value() &&
                    crucial.observation->memory.find("英睿达 Crucial") !=
                        std::string::npos,
                "Crucial requires an explicit manufacturer or a Micron CT part number");
}

[[nodiscard]] bool display_physical_size_wmi_fallback_is_exact_and_edid_preferred() {
  auto render = [](std::vector<WindowsDisplayEdid> edids,
                   std::vector<WindowsDisplayPhysicalSize> physical_sizes) {
    auto executor = std::make_unique<FakeQueryExecutor>();
    auto* raw_executor = executor.get();
    raw_executor->expected = full_queries();
    raw_executor->expected[6].result.rows = {
        {"BOE Display", "DISPLAY\\BOE1234\\1", "OK", "0", "2560", "1600"},
        {"BOE Display", "DISPLAY\\BOE1234\\2", "OK", "0", "2560", "1600"},
    };
    raw_executor->expected[9].result.rows = {
        {"BOE Display", "BOE", "DISPLAY\\BOE1234\\1", "OK", "0", "Monitor"},
        {"BOE Display", "BOE", "DISPLAY\\BOE1234\\2", "OK", "0", "Monitor"},
    };
    raw_executor->edids = std::move(edids);
    raw_executor->physical_sizes = std::move(physical_sizes);
    WindowsHardwareObserver observer{std::move(executor)};
    auto const result = observer.observe({});
    return result.observation.has_value() ? result.observation->display
                                          : std::string{};
  };

  auto invalid_checksum = range_limit_edid(60, 144, 0, 60, 34);
  invalid_checksum[20] ^= 0x01;
  auto const edid_preferred = render(
      {{.model_key = "boe1234",
        .instance_ordinal = 1,
        .bytes = range_limit_edid(60, 144, 0, 60, 34)}},
      {{.model_key = "boe1234",
        .instance_ordinal = 1,
        .horizontal_centimeters = 34,
        .vertical_centimeters = 22},
       {.model_key = "boe1234",
        .instance_ordinal = 2,
        .horizontal_centimeters = 60,
        .vertical_centimeters = 34}});
  auto const missing_edid = render(
      {}, {{.model_key = "boe1234",
            .instance_ordinal = 1,
            .horizontal_centimeters = 34,
            .vertical_centimeters = 22}});
  auto const invalid_edid = render(
      {{.model_key = "boe1234", .instance_ordinal = 1,
        .bytes = std::move(invalid_checksum)}},
      {{.model_key = "boe1234",
        .instance_ordinal = 1,
        .horizontal_centimeters = 34,
        .vertical_centimeters = 22}});
  auto const model_scoped_wmi = render(
      {}, {{.model_key = "boe1234",
            .instance_ordinal = 0,
            .horizontal_centimeters = 60,
            .vertical_centimeters = 34}});

  return expect(edid_preferred.find("27.2 英寸") != std::string::npos &&
                    edid_preferred.find("15.9 英寸") == std::string::npos,
                "a valid EDID physical size must take precedence over WMI") &&
         expect(missing_edid.find("15.9 英寸") != std::string::npos &&
                    missing_edid.find("英寸未识别") != std::string::npos,
                "a WMI physical size must fill only the exact same-model instance when EDID is absent") &&
         expect(invalid_edid.find("15.9 英寸") != std::string::npos,
                "a WMI physical size must recover from invalid EDID dimensions") &&
         expect(model_scoped_wmi.find("英寸未识别") != std::string::npos &&
                    model_scoped_wmi.find("27.2 英寸") == std::string::npos,
                "a model-scoped WMI physical size must not be assigned to any display instance");
}

[[nodiscard]] bool invalid_or_conflicting_edid_fails_closed() {
  auto render = [](std::vector<WindowsDisplayEdid> edids) {
    auto executor = std::make_unique<FakeQueryExecutor>();
    executor->expected = full_queries();
    executor->edids = std::move(edids);
    WindowsHardwareObserver observer{std::move(executor)};
    auto const result = observer.observe({});
    return result.observation.has_value() ? result.observation->display
                                          : std::string{};
  };

  auto invalid_checksum = range_limit_edid(60, 144);
  invalid_checksum[20] ^= 0x01;
  auto const missing = render({});
  auto const invalid = render({
      {.model_key = "boe1234", .bytes = std::move(invalid_checksum)},
  });
  auto const reserved = render({
      {.model_key = "boe1234", .bytes = range_limit_edid(60, 144, 0x80)},
  });
  auto const conflicting = render({
      {.model_key = "boe1234", .bytes = range_limit_edid(60, 120)},
      {.model_key = "boe1234", .bytes = range_limit_edid(60, 144)},
  });
  auto const failed_closed = [](std::string const& value) {
    return value.find(" Hz") == std::string::npos &&
           value.find("EDID") == std::string::npos &&
           value.find("刷新率未识别") != std::string::npos;
  };
  return expect(failed_closed(missing) && failed_closed(invalid) &&
                    failed_closed(reserved) && failed_closed(conflicting),
                "missing, invalid, reserved, or conflicting raw EDID must not produce a refresh-rate claim but must retain the display slot");
}

[[nodiscard]] bool unknown_media_physical_storage_stays_device_only() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  // Isolate the unknown-media case from the known SSD/HDD rows used by the
  // complete model fixture so category summaries remain unambiguous.
  raw_executor->expected[7].result.rows = {
      {"Example USB Storage", "Example", "1000204886016",
       "USB\\VID_0000&PID_0000", "OK", "0", "USB", "Fixed media"}};
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->storage.find("Example USB Storage") !=
                        std::string::npos &&
                    result.observation->solid_state_storage.empty() &&
                    result.observation->hard_disk_storage.empty() &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::storage &&
                                 device.name.find("Example USB Storage") !=
                                     std::string::npos &&
                                 device.storage_media ==
                                     HardwareStorageMedia::unknown &&
                                 device.capacity_bytes == 1000204886016ull;
                        }),
                "a confirmed physical disk with unknown media class must remain a structured device fact without a UI category summary");
}

[[nodiscard]] bool memory_quantity_rendering_scales_past_two_modules() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[5].result.rows.push_back(
      {"Micron", "25769803776", "5600", "MTC20C2085S1EC48BA1", "DIMM2",
       "Physical Memory 2", "34", "5600"});
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->memory.find(
                        "美光 Micron DDR5 72GB 5600MHz (24GB + 24GB + 24GB)") !=
                        std::string::npos,
                "memory summaries must render every matching module instead of two hard-coded entries");
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

  return expect(complete.has_value() && complete_raw->calls == 11,
                "a complete read-only model probe may provide a cache key") &&
          expect(!partial.has_value() && partial_raw->calls == 11,
                "partial probes must not falsely declare a hardware change");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= complete_read_only_model_facts_are_mapped();
  passed &= disabled_driverless_and_error_devices_are_retained();
  passed &= virtual_software_vpn_loopback_and_unknown_rows_are_filtered();
  passed &= partial_failure_preserves_usable_model_facts();
  passed &= oem_firmware_fallbacks_keep_concrete_models();
  passed &= incomplete_board_rows_fail_closed();
  passed &= real_machine_wmi_field_shapes_are_projected();
  passed &= displayconfig_facts_are_authoritative_and_fail_closed();
  passed &= topology_and_dxgi_enrich_only_verified_models();
  passed &= gpu_model_families_are_expanded_without_guessing();
  passed &= core_ultra_9_275hx_intel_graphics_requires_exact_identity();
  passed &= display_instances_are_not_merged_by_model_name();
  passed &= cpu_core_classes_are_unrecognised_without_reliable_topology();
  passed &= cpu_single_efficiency_class_marks_core_classes_unrecognised();
  passed &= cpu_core_classes_are_unrecognised_when_counts_conflict();
  passed &= cpu_three_core_classes_are_projected_when_consistent();
  passed &= cpu_two_core_classes_keep_a_real_zero_low_power_count();
  passed &= storage_duplicates_require_stable_instance_identity();
  passed &= crucial_memory_brand_requires_confirmed_identity();
  passed &= display_physical_size_wmi_fallback_is_exact_and_edid_preferred();
  passed &= invalid_or_conflicting_edid_fails_closed();
  passed &= unknown_media_physical_storage_stays_device_only();
  passed &= memory_quantity_rendering_scales_past_two_modules();
  passed &= permission_denial_and_cancellation_are_terminal();
  passed &= model_change_probe_requires_complete_facts();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
