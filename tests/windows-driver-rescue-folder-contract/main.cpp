#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "azzs/adapters/windows/windows_driver_acquisition.hpp"

namespace {

namespace drivers = azzs::application::driver_acquisition;
using azzs::adapters::windows::WindowsDriverHandoffPlatform;
using azzs::adapters::windows::WindowsRescueFolderExplorer;

struct MountPointReparseData final {
  ULONG reparse_tag;
  USHORT reparse_data_length;
  USHORT reserved;
  USHORT substitute_name_offset;
  USHORT substitute_name_length;
  USHORT print_name_offset;
  USHORT print_name_length;
  wchar_t path_buffer[1];
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows driver rescue folder contract failed: " << message
              << '\n';
  }
  return condition;
}

[[nodiscard]] std::filesystem::path module_directory() {
  std::array<wchar_t, 32768> buffer{};
  auto const length = ::GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    return {};
  }
  auto const module = std::filesystem::path{std::wstring{buffer.data(), length}};
  return module.parent_path();
}

[[nodiscard]] std::string read_file(std::filesystem::path const& path) {
  std::ifstream input{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] bool create_directory_junction(
    std::filesystem::path const& link, std::filesystem::path const& target) {
  if (!::CreateDirectoryW(link.c_str(), nullptr)) {
    return false;
  }

  auto const substitute_name = L"\\??\\" + target.wstring();
  auto const print_name = target.wstring();
  auto const substitute_bytes =
      substitute_name.size() * sizeof(wchar_t);
  auto const print_bytes = print_name.size() * sizeof(wchar_t);
  auto const path_bytes = substitute_bytes + sizeof(wchar_t) + print_bytes +
                          sizeof(wchar_t);
  auto const reparse_data_length = sizeof(USHORT) * 4 + path_bytes;
  auto const input_length =
      offsetof(MountPointReparseData, path_buffer) +
      path_bytes;
  if (reparse_data_length >
          static_cast<std::size_t>((std::numeric_limits<USHORT>::max)()) ||
      input_length > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
    ::RemoveDirectoryW(link.c_str());
    return false;
  }

  HANDLE directory = ::CreateFileW(
      link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    ::RemoveDirectoryW(link.c_str());
    return false;
  }

  alignas(MountPointReparseData)
      std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
  auto* reparse = reinterpret_cast<MountPointReparseData*>(storage.data());
  reparse->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
  reparse->reparse_data_length =
      static_cast<USHORT>(reparse_data_length);
  reparse->reserved = 0;
  reparse->substitute_name_offset = 0;
  reparse->substitute_name_length = static_cast<USHORT>(substitute_bytes);
  reparse->print_name_offset =
      static_cast<USHORT>(substitute_bytes + sizeof(wchar_t));
  reparse->print_name_length = static_cast<USHORT>(print_bytes);
  auto* names = reparse->path_buffer;
  std::copy(substitute_name.begin(), substitute_name.end(), names);
  names[substitute_name.size()] = L'\0';
  auto* print_destination = names + substitute_name.size() + 1;
  std::copy(print_name.begin(), print_name.end(), print_destination);
  print_destination[print_name.size()] = L'\0';

  DWORD ignored{};
  auto const created = ::DeviceIoControl(
      directory, FSCTL_SET_REPARSE_POINT, reparse,
      static_cast<DWORD>(input_length), nullptr, 0, &ignored, nullptr);
  ::CloseHandle(directory);
  if (!created) {
    ::RemoveDirectoryW(link.c_str());
  }
  return created != FALSE;
}

class RecordingExplorer final : public WindowsRescueFolderExplorer {
 public:
  [[nodiscard]] bool open_folder(std::wstring const& folder,
                                 std::string&) override {
    opened_folders.emplace_back(folder);
    return true;
  }

  std::vector<std::filesystem::path> opened_folders;
};

[[nodiscard]] bool fixed_folders_are_created_and_opened_without_executable_handoff() {
  auto explorer = std::make_unique<RecordingExplorer>();
  auto* recorded = explorer.get();
  WindowsDriverHandoffPlatform platform{std::move(explorer)};

  auto const parent = module_directory();
  auto const generic = parent / L"rescue-tools" / L"generic-network-driver";
  auto const diagnostics =
      parent / L"rescue-tools" / L"offline-network-diagnostics";
  bool passed = expect(!parent.empty(), "the contract executable must have a module parent");
  if (!passed) {
    return false;
  }

  std::string error;
  auto const generic_opened = platform.open_rescue_folder(
      drivers::RescueToolTarget::generic_network_driver, error);
  passed &= expect(generic_opened && error.empty() &&
                       std::filesystem::is_directory(generic) &&
                       recorded->opened_folders.size() == 1 &&
                       recorded->opened_folders.front() == generic,
                   "a missing generic rescue folder must be created below the module parent and opened as a directory");

  auto const later_executable = generic / L"later-added-tool.exe";
  if (!std::filesystem::exists(later_executable)) {
    std::ofstream output{later_executable, std::ios::binary};
    output << "not a launched tool";
  }
  auto const contents_before = read_file(later_executable);
  auto const timestamp_before = std::filesystem::last_write_time(later_executable);
  error.clear();
  auto const reopened = platform.open_rescue_folder(
      drivers::RescueToolTarget::generic_network_driver, error);
  passed &= expect(reopened && error.empty() &&
                       read_file(later_executable) == contents_before &&
                       std::filesystem::last_write_time(later_executable) ==
                           timestamp_before &&
                       recorded->opened_folders.size() == 2 &&
                       recorded->opened_folders.back() == generic,
                   "opening an existing rescue folder must leave later-added executable content untouched and pass only the folder to Explorer");

  error.clear();
  auto const diagnostics_opened = platform.open_rescue_folder(
      drivers::RescueToolTarget::offline_network_diagnostics, error);
  passed &= expect(diagnostics_opened && error.empty() &&
                       std::filesystem::is_directory(diagnostics) &&
                       recorded->opened_folders.size() == 3 &&
                       recorded->opened_folders.back() == diagnostics &&
                       generic != diagnostics,
                   "the two fixed rescue targets must remain distinct and cannot be exchanged");

  error.clear();
  auto const invalid_opened = platform.open_rescue_folder(
      static_cast<drivers::RescueToolTarget>(999), error);
  return passed && expect(!invalid_opened && !error.empty() &&
                              recorded->opened_folders.size() == 3,
                          "an unsupported rescue target must not reach Explorer");
}

[[nodiscard]] bool reparse_parent_is_rejected_before_any_external_handoff() {
  auto const parent = module_directory();
  auto const rescue_root = parent / L"rescue-tools";
  auto const outside = parent /
                       (L"rescue-tools-outside-" +
                        std::to_wstring(::GetCurrentProcessId()));
  std::error_code filesystem_error;
  std::filesystem::remove_all(rescue_root, filesystem_error);
  bool passed = expect(!parent.empty() && !filesystem_error,
                       "the reparse contract must prepare the module directory");
  std::filesystem::remove_all(outside, filesystem_error);
  std::filesystem::create_directories(outside, filesystem_error);
  passed &= expect(!filesystem_error,
                   "the reparse contract must prepare an outside target");
  if (!passed) {
    return false;
  }

  auto const linked = create_directory_junction(rescue_root, outside);
  passed &= expect(linked,
                   "the rescue-folder contract requires a parent directory junction");
  if (linked) {
    auto explorer = std::make_unique<RecordingExplorer>();
    auto* recorded = explorer.get();
    WindowsDriverHandoffPlatform platform{std::move(explorer)};
    std::string error;
    auto const opened = platform.open_rescue_folder(
        drivers::RescueToolTarget::generic_network_driver, error);
    passed &= expect(
        !opened && !error.empty() && recorded->opened_folders.empty() &&
            !std::filesystem::exists(outside / L"generic-network-driver"),
        "a reparse rescue parent must not create or hand off an outside directory");
    ::RemoveDirectoryW(rescue_root.c_str());
  }
  std::filesystem::remove_all(outside, filesystem_error);
  return passed;
}

}  // namespace

int main() {
  return fixed_folders_are_created_and_opened_without_executable_handoff() &&
                 reparse_parent_is_rejected_before_any_external_handoff()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
