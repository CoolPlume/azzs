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
using azzs::adapters::windows::WindowsGpuMemory;
using azzs::application::HardwareObservationCode;
using azzs::application::HardwareDisplayConnection;
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
  std::optional<WindowsCpuTopology> topology;
  std::vector<WindowsGpuMemory> dxgi_adapters;
  std::vector<WindowsDisplayEdid> edids;
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
};

[[nodiscard]] std::vector<std::uint8_t> range_limit_edid(
    std::uint8_t minimum_vertical_hz, std::uint8_t maximum_vertical_hz,
    std::uint8_t range_offset_flags = 0) {
  std::vector<std::uint8_t> edid(128, 0);
  std::uint8_t const header[]{0x00, 0xff, 0xff, 0xff,
                             0xff, 0xff, 0xff, 0x00};
  std::ranges::copy(header, edid.begin());
  edid[18] = 1;
  edid[19] = 4;
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
                 expect(result.observation->cpu.find("核心 8，线程 16") !=
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
                     result.observation->display.find("类型未确认") !=
                         std::string::npos &&
                      result.observation->display.find("2560 × 1600") !=
                          std::string::npos &&
                      result.observation->display.find(
                          "物理刷新率上限（EDID）：未读取") !=
                          std::string::npos &&
                     result.observation->storage.find("PC801 SK 海力士") !=
                         std::string::npos &&
                     result.observation->storage.find("三星 Samsung SSD 990 PRO") !=
                         std::string::npos &&
                     result.observation->storage.find("接口：NVMe") !=
                         std::string::npos &&
                     result.observation->storage.find("PCIe 代际：PCIe 4.0") !=
                         std::string::npos &&
                     result.observation->storage.find("NAND 颗粒：TLC") !=
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
                     result.observation->devices.size() == 14 &&
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
                    result.observation->cpu.find("核心 8，线程 16") !=
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
                    result.observation->cpu.find("核心 24，线程 24") !=
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
      {"Samsung SSD 990 PRO 2TB", "(标准磁盘驱动器)", "2000396321280",
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
      {.model_key = "boe0cd1", .bytes = range_limit_edid(60, 240, 0x0c)},
      {.model_key = "lhc907d", .bytes = range_limit_edid(48, 160)},
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
         expect(observation->cpu.find("核心 24，线程 24；P 核 8，E 核 16") !=
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
                     "美光 Micron Technology DDR5 48GB 5600MHz (24GB + 24GB)") !=
                     std::string::npos,
                "matching physical DIMMs must be aggregated by part number") &&
         expect(observation->display.find("京东方 BOE0CD1（内建") !=
                          std::string::npos &&
                     observation->display.find(
                         "物理刷新率上限（EDID）：240 Hz") !=
                         std::string::npos &&
                     observation->display.find("泰坦军团 TITAN ARMY P275MV PLUS（外接；") !=
                          std::string::npos &&
                     observation->display.find("3840 × 2160") !=
                         std::string::npos &&
                     observation->display.find(
                         "物理刷新率上限（EDID）：160 Hz") !=
                         std::string::npos &&
                     line_separated(observation->display) &&
                     observation->display.find("默认监视器") == std::string::npos,
                "validated raw EDID range limits must enrich physical monitor names without using desktop modes") &&
         expect(observation->solid_state_storage.find("Samsung SSD 990 PRO 2TB") !=
                         std::string::npos &&
                     observation->solid_state_storage.find(
                         "SK 海力士 PCB01 HFS001TFM9X187N") !=
                          std::string::npos &&
                     observation->solid_state_storage.find(
                         "PCIe 代际：PCIe 5.0 x4") != std::string::npos &&
                     observation->solid_state_storage.find(
                         "NAND 颗粒：238 层 4D TLC（资料识别；SLC 缓存：支持）") !=
                         std::string::npos &&
                     observation->solid_state_storage.find(
                         "SK 海力士 PCB01 HFS999TFM9X187N 954GB（接口：NVMe；PCIe 代际：未读取；NAND 颗粒：未读取）") !=
                         std::string::npos &&
                     observation->solid_state_storage.find("PCIe 代际：未读取") !=
                         std::string::npos &&
                    observation->solid_state_storage.find("NAND 颗粒：未读取") !=
                        std::string::npos &&
                     observation->hard_disk_storage.empty() &&
                     observation->storage.find("标准磁盘驱动器") ==
                         std::string::npos &&
                     line_separated(observation->solid_state_storage) &&
                     raw_executor->requested_display_pnp_ids.size() == 2 &&
                     refresh_fingerprint_changed,
                    "only the exact PCB01 product and validated raw EDID model keys may receive model-specific facts");
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
       "PCI\\VEN_10DE&DEV_1F00", "OK", "0", "", "4293918720"},
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
         expect(result.observation->cpu.find("P 核 8，E 核 16") !=
                    std::string::npos &&
                    result.observation->gpu.find("专用显存 16 GB") !=
                        std::string::npos &&
                    result.observation->gpu.find("专用显存 128 MB；共享内存 31.4 GB") !=
                        std::string::npos &&
                    result.observation->gpu.find("专用显存 4 GB") ==
                        std::string::npos &&
                    std::ranges::any_of(
                        result.observation->devices, [](auto const& device) {
                          return device.kind == HardwareDeviceKind::cpu &&
                                 device.core_count == 24 &&
                                 device.thread_count == 24 &&
                                 device.performance_core_count == 8 &&
                                 device.efficiency_core_count == 16;
                        }),
                "CPU P/E and GPU memory must use the platform facts, never the truncated WMI AdapterRAM value");
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
    return value.find("物理刷新率上限（EDID）：未读取") !=
               std::string::npos &&
           value.find("物理刷新率上限（EDID）：144 Hz") ==
               std::string::npos;
  };
  return expect(failed_closed(missing) && failed_closed(invalid) &&
                    failed_closed(reserved) && failed_closed(conflicting),
                "missing, invalid, reserved, or conflicting raw EDID must remain unread");
}

[[nodiscard]] bool unclassified_physical_storage_remains_visible() {
  auto executor = std::make_unique<FakeQueryExecutor>();
  auto* raw_executor = executor.get();
  raw_executor->expected = full_queries();
  raw_executor->expected[7].result.rows.push_back(
      {"Example USB Storage", "Example", "1000204886016",
       "USB\\VID_0000&PID_0000", "OK", "0", "USB", "Fixed media"});
  WindowsHardwareObserver observer{std::move(executor)};
  auto const result = observer.observe({});
  return expect(result.succeeded() && result.observation.has_value() &&
                    result.observation->storage.find("Example USB Storage") !=
                        std::string::npos &&
                    result.observation->unclassified_storage.find(
                        "Example USB Storage") != std::string::npos &&
                    result.observation->unclassified_storage.find("接口：USB") !=
                        std::string::npos,
                "a confirmed physical disk with unknown media class must remain visible in its own summary");
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
  passed &= topology_and_dxgi_enrich_only_verified_models();
  passed &= invalid_or_conflicting_edid_fails_closed();
  passed &= unclassified_physical_storage_remains_visible();
  passed &= memory_quantity_rendering_scales_past_two_modules();
  passed &= permission_denial_and_cancellation_are_terminal();
  passed &= model_change_probe_requires_complete_facts();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
