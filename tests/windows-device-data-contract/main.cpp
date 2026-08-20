#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <Aclapi.h>
#include <Sddl.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "azzs/adapters/infrastructure/local_file_log_storage.hpp"
#include "azzs/adapters/infrastructure/structured_execution_log.hpp"
#include "azzs/adapters/windows/windows_device_data_environment.hpp"
#include "azzs/adapters/windows/windows_state_file_system.hpp"
#include "azzs/application/device_state_store.hpp"
#include "azzs/testing/fixed_clock.hpp"
#include "azzs/testing/windows_device_data_environment_test_seam.hpp"

namespace {

using azzs::adapters::windows::DeviceDataEnvironmentError;
using azzs::adapters::windows::DeviceDataEnvironmentOptions;
using azzs::adapters::windows::WindowsDeviceDataEnvironment;
using azzs::adapters::windows::WindowsStateFileSystem;
using azzs::adapters::infrastructure::LocalFileLogStorage;
using azzs::adapters::infrastructure::StructuredExecutionLog;
using azzs::testing::WindowsDeviceDataIdentityEvidence;
using azzs::testing::WindowsDeviceDataTestOptions;

using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;
struct SecurityInfo final {
  LocalMemory descriptor{nullptr, &::LocalFree};
  PSID owner{};
  PACL dacl{};

  SecurityInfo() = default;
  SecurityInfo(SecurityInfo&&) noexcept = default;
  SecurityInfo& operator=(SecurityInfo&&) noexcept = default;
  SecurityInfo(SecurityInfo const&) = delete;
  SecurityInfo& operator=(SecurityInfo const&) = delete;
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows device data contract failed: " << message << '\n';
  }
  return condition;
}

[[nodiscard]] std::string utf8(std::filesystem::path const& path) {
  auto const value = path.u8string();
  return {reinterpret_cast<char const*>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string const& value) {
  auto const* begin = reinterpret_cast<char8_t const*>(value.data());
  return std::filesystem::path{std::u8string{begin, begin + value.size()}};
}

[[nodiscard]] std::optional<std::string> sid_string(PSID sid) {
  wchar_t* raw = nullptr;
  if (!::ConvertSidToStringSidW(sid, &raw)) {
    return std::nullopt;
  }
  LocalMemory memory{raw, &::LocalFree};
  auto const count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw,
                                           -1, nullptr, 0, nullptr, nullptr);
  if (count <= 1) {
    return std::nullopt;
  }
  std::string value(static_cast<std::size_t>(count), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, raw, -1,
                            value.data(), count, nullptr, nullptr) != count) {
    return std::nullopt;
  }
  value.pop_back();
  return value;
}

[[nodiscard]] std::optional<std::string> current_user_sid() {
  HANDLE raw_token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return std::nullopt;
  }
  struct HandleCloser final {
    void operator()(void* handle) const noexcept {
      if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
        ::CloseHandle(handle);
      }
    }
  };
  std::unique_ptr<void, HandleCloser> token{raw_token};
  DWORD required = 0;
  ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
  if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return std::nullopt;
  }
  std::vector<std::byte> bytes(required);
  if (!::GetTokenInformation(token.get(), TokenUser, bytes.data(), required,
                             &required)) {
    return std::nullopt;
  }
  auto const* user = reinterpret_cast<TOKEN_USER const*>(bytes.data());
  return sid_string(user->User.Sid);
}

[[nodiscard]] std::string token_integrity_label() {
  HANDLE raw_token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    return "unavailable";
  }
  std::unique_ptr<void, decltype(&::CloseHandle)> token{raw_token,
                                                         &::CloseHandle};
  DWORD required = 0;
  ::SetLastError(ERROR_SUCCESS);
  ::GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0,
                        &required);
  if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
    return "unavailable";
  }
  std::vector<std::byte> bytes(required);
  if (!::GetTokenInformation(token.get(), TokenIntegrityLevel, bytes.data(),
                             required, &required)) {
    return "unavailable";
  }
  auto const* label =
      reinterpret_cast<TOKEN_MANDATORY_LABEL const*>(bytes.data());
  if (!::IsValidSid(label->Label.Sid)) {
    return "unavailable";
  }
  auto const count = *::GetSidSubAuthorityCount(label->Label.Sid);
  if (count == 0) {
    return "unavailable";
  }
  auto const integrity =
      *::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(count - 1));
  if (integrity >= SECURITY_MANDATORY_SYSTEM_RID) {
    return "system";
  }
  if (integrity >= SECURITY_MANDATORY_HIGH_RID) {
    return "high";
  }
  if (integrity >= SECURITY_MANDATORY_MEDIUM_RID) {
    return "medium";
  }
  if (integrity >= SECURITY_MANDATORY_LOW_RID) {
    return "low";
  }
  return "untrusted";
}

[[nodiscard]] std::optional<std::filesystem::path> isolated_contract_path(
    std::wstring_view name) {
  auto const required = ::GetEnvironmentVariableW(
      L"AZZS_WINDOWS_DEVICE_DATA_CONTRACT_ROOT", nullptr, 0);
  if (required == 0) {
    return std::nullopt;
  }
  std::vector<wchar_t> root_buffer(required);
  auto const copied = ::GetEnvironmentVariableW(
      L"AZZS_WINDOWS_DEVICE_DATA_CONTRACT_ROOT", root_buffer.data(),
      static_cast<DWORD>(root_buffer.size()));
  if (copied == 0 || copied >= root_buffer.size()) {
    return std::nullopt;
  }
  auto const base = std::filesystem::path{root_buffer.data()};
  std::error_code error;
  std::filesystem::create_directories(base, error);
  if (error) {
    return std::nullopt;
  }
  static std::atomic_uint64_t sequence{0};
  for (;;) {
    auto const candidate =
        base / (std::wstring{name} + L"-" +
                std::to_wstring(::GetCurrentProcessId()) + L"-" +
                std::to_wstring(::GetTickCount64()) + L"-" +
                std::to_wstring(sequence.fetch_add(1)));
    ::SetLastError(ERROR_SUCCESS);
    auto const attributes = ::GetFileAttributesW(candidate.c_str());
    auto const last_error = ::GetLastError();
    if (attributes == INVALID_FILE_ATTRIBUTES &&
        (last_error == ERROR_FILE_NOT_FOUND ||
         last_error == ERROR_PATH_NOT_FOUND)) {
      return candidate;
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return std::nullopt;
    }
  }
}

[[nodiscard]] std::optional<SecurityInfo> security_info(
    std::filesystem::path const& path) {
  SecurityInfo info;
  PSECURITY_DESCRIPTOR raw = nullptr;
  auto const error = ::GetNamedSecurityInfoW(
      const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &info.owner,
      nullptr, &info.dacl, nullptr, &raw);
  if (error != ERROR_SUCCESS || raw == nullptr || info.owner == nullptr ||
      info.dacl == nullptr) {
    if (raw != nullptr) {
      ::LocalFree(raw);
    }
    return std::nullopt;
  }
  info.descriptor.reset(raw);
  return info;
}

[[nodiscard]] bool dacl_is_protected(SecurityInfo const& info) {
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  return ::GetSecurityDescriptorControl(info.descriptor.get(), &control,
                                        &revision) &&
         (control & SE_DACL_PROTECTED) != 0;
}

[[nodiscard]] bool acl_only_full_control_for(
    SecurityInfo const& info,
    std::span<PSID const> expected_sids,
    BYTE expected_ace_flags) {
  std::vector<bool> found(expected_sids.size(), false);
  for (DWORD index = 0; index < info.dacl->AceCount; ++index) {
    void* raw_ace = nullptr;
    if (!::GetAce(info.dacl, index, &raw_ace)) {
      return false;
    }
    auto const* header = static_cast<ACE_HEADER const*>(raw_ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE ||
        header->AceFlags != expected_ace_flags) {
      return false;
    }
    auto const* ace = static_cast<ACCESS_ALLOWED_ACE const*>(raw_ace);
    auto sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
    bool expected = false;
    for (std::size_t expected_index = 0;
         expected_index < expected_sids.size(); ++expected_index) {
      if (::EqualSid(sid, expected_sids[expected_index])) {
        expected = true;
        found[expected_index] = true;
        break;
      }
    }
    if (!expected || ace->Mask != FILE_ALL_ACCESS) {
      return false;
    }
  }
  for (bool present : found) {
    if (!present) {
      return false;
    }
  }
  return static_cast<std::size_t>(info.dacl->AceCount) ==
         expected_sids.size();
}

[[nodiscard]] bool secure_directory_matches(
    std::filesystem::path const& path,
    PSID expected_owner,
    std::span<PSID const> expected_sids) {
  auto const attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;
  }
  auto info = security_info(path);
  return info.has_value() && ::EqualSid(info->owner, expected_owner) &&
         dacl_is_protected(*info) &&
         acl_only_full_control_for(
             *info, expected_sids,
             OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE);
}

[[nodiscard]] bool secure_file_matches(
    std::filesystem::path const& path,
    PSID expected_owner,
    std::span<PSID const> expected_sids) {
  auto const attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return false;
  }
  auto info = security_info(path);
  return info.has_value() && ::EqualSid(info->owner, expected_owner) &&
         dacl_is_protected(*info) &&
         acl_only_full_control_for(*info, expected_sids, 0);
}

[[nodiscard]] bool fixed_local_volume(std::filesystem::path const& path) {
  wchar_t volume[MAX_PATH]{};
  return ::GetVolumePathNameW(path.c_str(), volume, MAX_PATH) &&
         ::GetDriveTypeW(volume) == DRIVE_FIXED;
}

[[nodiscard]] bool verify_layout_acl_and_slots() {
  auto const root = isolated_contract_path(L"layout");
  if (!root.has_value()) {
    return expect(false,
                  "the Windows device data contract root must be available");
  }
  std::error_code cleanup_error;

  auto const current_sid_text = current_user_sid();
  bool passed = expect(current_sid_text.has_value(),
                       "the process token must expose a user SID");
  if (!current_sid_text.has_value()) {
    return passed;
  }
  auto first_result = azzs::testing::prepare_windows_device_data_for_test(
      WindowsDeviceDataTestOptions{.root_override_utf8 = utf8(*root),
                                   .test_identity_evidence =
                                       WindowsDeviceDataIdentityEvidence{
                                           .process_sid = *current_sid_text,
                                           .wts_session_sid = *current_sid_text}});
  passed &= expect(static_cast<bool>(first_result),
                   "an isolated Windows device root must be prepared");
  if (!first_result) {
    return passed;
  }

  constexpr auto administrators_sid_text = "S-1-5-32-544";
  auto second_result = azzs::testing::prepare_windows_device_data_for_test(
      WindowsDeviceDataTestOptions{.root_override_utf8 = utf8(*root),
                                   .test_identity_evidence =
                                       WindowsDeviceDataIdentityEvidence{
                                           .process_sid = *current_sid_text,
                                           .wts_session_sid = *current_sid_text},
                                   .subject_override =
                                       administrators_sid_text});
  passed &= expect(static_cast<bool>(second_result),
                   "a second subject must reuse the stable machine root");
  if (!second_result) {
    return passed;
  }

  auto const& environment = *first_result.environment;
  auto const subject_path =
      *root / L"subjects" / std::filesystem::path{environment.subject_id};
  auto const second_subject_path =
      *root / L"subjects" / L"S-1-5-32-544";
  passed &= expect(
      fixed_local_volume(*root) &&
          std::filesystem::is_directory(*root / L"device" / L"state") &&
          std::filesystem::is_directory(*root / L"locks") &&
          std::filesystem::is_directory(*root / L"subjects") &&
          std::filesystem::is_directory(subject_path / L"state") &&
          std::filesystem::is_directory(subject_path / L"logs") &&
          std::filesystem::is_directory(subject_path / L"exports") &&
          std::filesystem::is_directory(second_subject_path / L"state"),
      "device and subject data must use separate stable partitions");

  BYTE administrators_buffer[SECURITY_MAX_SID_SIZE]{};
  DWORD administrators_size = sizeof(administrators_buffer);
  BYTE system_buffer[SECURITY_MAX_SID_SIZE]{};
  DWORD system_size = sizeof(system_buffer);
  passed &= expect(
      ::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                           administrators_buffer, &administrators_size) &&
          ::CreateWellKnownSid(WinLocalSystemSid, nullptr, system_buffer,
                               &system_size),
      "well-known ACL subjects must be available");

  PSID current_sid = nullptr;
  std::wstring current_sid_wide(current_sid_text->begin(),
                                current_sid_text->end());
  passed &= expect(
      ::ConvertStringSidToSidW(current_sid_wide.c_str(), &current_sid),
      "the current state subject must be represented by a SID");
  LocalMemory current_sid_memory{current_sid, &::LocalFree};
  if (current_sid != nullptr) {
    std::array<PSID, 2> const machine_sids{system_buffer,
                                          administrators_buffer};
    for (auto const& path :
         {*root, *root / L"device", *root / L"device" / L"state",
          *root / L"locks", *root / L"subjects"}) {
      passed &= expect(
          secure_directory_matches(path, administrators_buffer, machine_sids),
          "machine directories must be protected for system and administrators only");
    }

    std::array<PSID, 2> const first_subject_sids{system_buffer, current_sid};
    for (auto const& path :
         {subject_path, subject_path / L"state", subject_path / L"logs",
          subject_path / L"exports"}) {
      passed &= expect(
          secure_directory_matches(path, current_sid, first_subject_sids),
          "the first subject subtree must retain its isolated owner and ACL");
    }

    std::array<PSID, 2> const second_subject_sids{system_buffer,
                                                  administrators_buffer};
    for (auto const& path :
         {second_subject_path, second_subject_path / L"state",
          second_subject_path / L"logs",
          second_subject_path / L"exports"}) {
      passed &= expect(
          secure_directory_matches(path, administrators_buffer,
                                   second_subject_sids),
          "a second subject must not rewrite another subject or machine ACL");
    }
  }

  azzs::testing::FixedClock clock{
      azzs::application::WallClockTime{std::chrono::milliseconds{1234}}};
  WindowsStateFileSystem first_files{environment};
  WindowsStateFileSystem second_files{environment};
  azzs::application::DeviceStateStore first_store{first_files, clock};
  azzs::application::DeviceStateStore second_store{second_files, clock};
  auto const key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"windows-adapter-contract"});
  azzs::domain::DeviceState initial{
      .value = {.payload = {std::byte{'v'}, std::byte{'1'}}},
  };
  auto initialized = first_store.initialize(key, std::move(initial));
  auto observed = second_store.inspect(key);
  passed &= expect(
      initialized.status == azzs::application::StateCommitStatus::committed &&
          observed.snapshot.has_value() &&
          observed.snapshot->revision == initialized.snapshot->revision,
      "two Windows filesystem adapters must share one validated authority");
  passed &= expect(
      std::filesystem::is_regular_file(
          *root / L"device" / L"state" / L"windows-adapter-contract" /
          L"authority.state"),
      "the Windows adapter must persist device state below the injected root");

  auto const subject_key = azzs::domain::StateKey::for_subject(
      azzs::domain::StateSubject{environment.subject_id},
      azzs::domain::AggregateId{"windows-subject-contract"});
  azzs::domain::DeviceState subject_initial{
      .value = {.payload = {std::byte{'s'}, std::byte{'1'}}},
  };
  auto subject_initialized =
      first_store.initialize(subject_key, std::move(subject_initial));
  passed &= expect(
      subject_initialized.status ==
          azzs::application::StateCommitStatus::committed,
      "the Windows adapter must persist an isolated subject aggregate");

  if (current_sid != nullptr) {
    std::array<PSID, 2> const machine_sids{system_buffer,
                                          administrators_buffer};
    auto const machine_aggregate =
        *root / L"device" / L"state" / L"windows-adapter-contract";
    passed &= expect(
        secure_directory_matches(machine_aggregate, administrators_buffer,
                                 machine_sids) &&
            secure_file_matches(machine_aggregate / L"authority.state",
                                administrators_buffer, machine_sids),
        "machine aggregate directories and files must retain protected machine ACLs");

    std::array<PSID, 2> const subject_sids{system_buffer, current_sid};
    auto const subject_aggregate =
        subject_path / L"state" / L"windows-subject-contract";
    passed &= expect(
        secure_directory_matches(subject_aggregate, current_sid,
                                 subject_sids) &&
            secure_file_matches(subject_aggregate / L"authority.state",
                                current_sid, subject_sids),
        "subject aggregate directories and files must retain protected subject ACLs");

    bool observed_lock = false;
    for (auto const& entry :
         std::filesystem::directory_iterator{*root / L"locks"}) {
      if (entry.path().filename().wstring().starts_with(L"state-") &&
          entry.path().extension() == L".lock") {
        observed_lock = true;
        passed &= expect(
            secure_file_matches(entry.path(), administrators_buffer,
                                machine_sids),
            "state lock files must retain protected machine ACLs");
      }
    }
    passed &= expect(observed_lock,
                     "state commits must leave a reusable protected lock file");

    LocalFileLogStorage log_storage{environment.root_utf8,
                                    environment.subject_id};
    StructuredExecutionLog execution_log{log_storage, clock};
    auto const correlation = execution_log.begin_correlation();
    auto const logged = execution_log.append(
        correlation,
        azzs::application::ExecutionEvent{
            .kind = azzs::application::ExecutionEventKind::adapter_result,
            .component = "windows-security-contract",
            .stage = "persist",
            .result = azzs::application::ExecutionResult::succeeded,
        });
    auto const exported = execution_log.export_diagnostic(
        azzs::application::DiagnosticContext{
            .workbench_build = "windows-security-contract",
            .coverage_started_at = clock.now(),
            .coverage_ended_at = clock.now(),
        });
    auto const execution_log_path = subject_path / L"logs" / L"execution.log";
    auto const diagnostic_path = path_from_utf8(exported.file_name);
    auto const log_lock_path =
        *root / L"locks" /
        std::filesystem::path{L"execution-log-" +
                              std::wstring{current_sid_wide} + L".lock"};
    if (!logged.persisted) {
      std::cerr << "first execution log write failed: " << logged.error << '\n';
    }
    if (!exported.produced) {
      std::cerr << "diagnostic export failed: " << exported.error << '\n';
    }
    passed &= expect(logged.persisted,
                     "the Windows execution log must be persisted and verified");
    passed &= expect(exported.produced,
                     "the Windows diagnostic export must be persisted and verified");
    passed &= expect(
        secure_file_matches(execution_log_path, current_sid, subject_sids),
        "execution logs must retain protected subject file ACLs");
    passed &= expect(
        exported.produced &&
            secure_file_matches(diagnostic_path, current_sid, subject_sids),
        "diagnostic exports must retain protected subject file ACLs");
    passed &= expect(
        secure_file_matches(log_lock_path, administrators_buffer,
                            machine_sids),
        "execution log lock files must retain protected machine file ACLs");

    auto const outside_target =
        root->parent_path() /
        (L"azzs-log-reparse-target-" +
         std::to_wstring(::GetCurrentProcessId()) + L".txt");
    auto const transaction_link =
        subject_path / L"logs" / L"execution.transaction";
    constexpr std::string_view outside_marker{"outside-target-must-not-change"};
    {
      std::ofstream target_stream{outside_target, std::ios::binary};
      target_stream.write(outside_marker.data(),
                          static_cast<std::streamsize>(outside_marker.size()));
    }
    std::error_code link_error;
    std::filesystem::remove(transaction_link, link_error);
    auto link_created = ::CreateSymbolicLinkW(
        transaction_link.c_str(), outside_target.c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (!link_created) {
      link_created = ::CreateSymbolicLinkW(transaction_link.c_str(),
                                           outside_target.c_str(), 0);
    }
    passed &= expect(
        link_created != FALSE,
        "the log reparse contract requires a transaction file symlink");
    if (link_created) {
      LocalFileLogStorage attacked_storage{environment.root_utf8,
                                           environment.subject_id};
      StructuredExecutionLog attacked_log{attacked_storage, clock};
      auto const attacked = attacked_log.append(
          correlation,
          azzs::application::ExecutionEvent{
              .kind = azzs::application::ExecutionEventKind::adapter_result,
              .component = "windows-security-contract",
              .stage = "reparse-attack",
              .result = azzs::application::ExecutionResult::failed,
          });
      std::ifstream target_stream{outside_target, std::ios::binary};
      std::string target_bytes{std::istreambuf_iterator<char>{target_stream},
                               std::istreambuf_iterator<char>{}};
      passed &= expect(
          !attacked.persisted && target_bytes == outside_marker,
          "log writes must reject a reparse transaction without touching its target");
      ::DeleteFileW(transaction_link.c_str());
    }
    std::filesystem::remove(outside_target, link_error);
  }

  if (subject_initialized.snapshot.has_value()) {
    azzs::application::StateCheckpoint checkpoint{
        .base_revision = subject_initialized.snapshot->revision,
        .payload = {std::byte{'c'}, std::byte{'p'}},
    };
    auto written = first_store.write_checkpoint(subject_key, checkpoint);
    auto read = second_store.read_checkpoint(
        subject_key, subject_initialized.snapshot->revision);
    auto consumed = second_store.consume_checkpoint(
        subject_key, subject_initialized.snapshot->revision);
    passed &= expect(
        written.status == azzs::application::CheckpointStatus::available &&
            read.status == azzs::application::CheckpointStatus::available &&
            consumed.status == azzs::application::CheckpointStatus::consumed,
        "checkpoint staging, publication, read, and consumption must share stable slots");
  }

  auto const slot_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"windows-slot-contract"});
  auto const slot_directory =
      *root / L"device" / L"state" / L"windows-slot-contract";
  std::array<std::byte, 1> const marker{std::byte{0x2a}};
  std::array slot_contracts{
      std::pair{azzs::application::StateFileSlot::checkpoint_staging,
                L"checkpoint.transaction"},
      std::pair{azzs::application::StateFileSlot::checkpoint_consumed_staging,
                L"checkpoint-consumed.transaction"},
      std::pair{azzs::application::StateFileSlot::corrupt_archive,
                L"corrupt-evidence.archive"},
      std::pair{azzs::application::StateFileSlot::corrupt_archive_staging,
                L"corrupt-evidence.transaction"},
  };
  for (auto const& [slot, file_name] : slot_contracts) {
    auto write = first_files.write(slot_key, slot, marker);
    passed &= expect(
        write.succeeded() &&
            std::filesystem::is_regular_file(slot_directory / file_name) &&
            !std::filesystem::exists(slot_directory / L"invalid"),
        "every state slot must have a stable Windows file name");
    static_cast<void>(first_files.remove(slot_key, slot));
  }

  auto const replace_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"windows-replace-contract"});
  std::array<std::byte, 2> const replaced_bytes{std::byte{0x4a},
                                                std::byte{0x5b}};
  auto target_write = first_files.write(
      replace_key, azzs::application::StateFileSlot::current, marker);
  auto source_write = first_files.write(
      replace_key, azzs::application::StateFileSlot::candidate,
      replaced_bytes);
  auto replaced = first_files.replace(
      replace_key, azzs::application::StateFileSlot::candidate,
      azzs::application::StateFileSlot::current);
  auto replaced_target = first_files.read(
      replace_key, azzs::application::StateFileSlot::current);
  auto consumed_source = first_files.read(
      replace_key, azzs::application::StateFileSlot::candidate);
  passed &= expect(
      target_write.succeeded() && source_write.succeeded() &&
          replaced.succeeded() &&
          replaced_target.status == azzs::application::StateIoStatus::succeeded &&
          replaced_target.bytes == azzs::domain::StateBytes(
                                       replaced_bytes.begin(),
                                       replaced_bytes.end()) &&
          consumed_source.status == azzs::application::StateIoStatus::not_found,
      "replace must release its validated existing target before publishing the source");

  auto const rejected_key = azzs::domain::StateKey::machine(
      azzs::domain::AggregateId{"windows-replace-rejection-contract"});
  auto rejected_source_write = first_files.write(
      rejected_key, azzs::application::StateFileSlot::candidate,
      replaced_bytes);
  auto const rejected_target =
      *root / L"device" / L"state" / L"windows-replace-rejection-contract" /
      L"authority.state";
  std::error_code rejected_target_error;
  auto const rejected_target_created =
      std::filesystem::create_directory(rejected_target,
                                        rejected_target_error);
  auto rejected = first_files.replace(
      rejected_key, azzs::application::StateFileSlot::candidate,
      azzs::application::StateFileSlot::current);
  auto retained_source = first_files.read(
      rejected_key, azzs::application::StateFileSlot::candidate);
  passed &= expect(
      rejected_source_write.succeeded() && rejected_target_created &&
          !rejected.succeeded() &&
          retained_source.status == azzs::application::StateIoStatus::succeeded &&
          retained_source.bytes == azzs::domain::StateBytes(
                                       replaced_bytes.begin(),
                                       replaced_bytes.end()) &&
          std::filesystem::is_directory(rejected_target),
      "replace must reject an invalid target without consuming the source");
  std::filesystem::remove(rejected_target, rejected_target_error);

  std::filesystem::remove_all(*root, cleanup_error);
  return passed;
}

[[nodiscard]] bool verify_non_local_root_is_rejected() {
  auto const result = azzs::testing::prepare_windows_device_data_for_test(
      WindowsDeviceDataTestOptions{
          .root_override_utf8 =
              R"(\\127.0.0.1\azzs-unreachable\device-data)",
          .subject_override = "S-1-5-32-544"});
  return expect(
      !result &&
          result.error ==
              DeviceDataEnvironmentError::unsupported_storage_location,
      "UNC and non-fixed roots must be rejected before storage access");
}

[[nodiscard]] bool verify_diagnostic_root_guards() {
  auto const empty = WindowsDeviceDataEnvironment::prepare(
      DeviceDataEnvironmentOptions{.diagnostic_root_utf8 = ""});
  auto const relative = WindowsDeviceDataEnvironment::prepare(
      DeviceDataEnvironmentOptions{
          .diagnostic_root_utf8 = "diagnostic-device-data-root"});
  auto const wrong_drive = WindowsDeviceDataEnvironment::prepare(
      DeviceDataEnvironmentOptions{
          .diagnostic_root_utf8 = R"(C:\azzs-diagnostic-contract)"});
  return expect(!empty &&
                    empty.error ==
                        DeviceDataEnvironmentError::unsupported_storage_location,
                "an empty diagnostic root must fail closed") &&
         expect(!relative &&
                    relative.error ==
                        DeviceDataEnvironmentError::unsupported_storage_location,
                "a relative diagnostic root must fail closed") &&
         expect(!wrong_drive &&
                    wrong_drive.error ==
                        DeviceDataEnvironmentError::unsupported_storage_location,
                "a diagnostic root outside drive D: must fail closed");
}

[[nodiscard]] bool verify_reparse_root_is_rejected() {
  auto const base = isolated_contract_path(L"reparse");
  if (!base.has_value()) {
    return expect(false,
                  "the Windows reparse contract root must be available");
  }
  auto const target = *base / L"target";
  auto const link = *base / L"device-data";
  std::error_code error;
  std::filesystem::create_directories(target, error);
  if (error) {
    return expect(false, "the reparse test target must be created");
  }
  auto created = ::CreateSymbolicLinkW(
      link.c_str(), target.c_str(),
      SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  if (!created) {
    created = ::CreateSymbolicLinkW(link.c_str(), target.c_str(),
                                    SYMBOLIC_LINK_FLAG_DIRECTORY);
  }
  bool passed = expect(created != FALSE,
                       "the reparse contract requires a directory symlink");
  if (created) {
    auto const result = azzs::testing::prepare_windows_device_data_for_test(
        WindowsDeviceDataTestOptions{.root_override_utf8 = utf8(link),
                                     .subject_override = "S-1-5-32-544"});
    passed &= expect(
        !result &&
            result.error == DeviceDataEnvironmentError::unsafe_storage_path,
        "a reparse point in the managed root must fail closed");
    ::RemoveDirectoryW(link.c_str());
  }
  std::filesystem::remove_all(*base, error);
  return passed;
}

[[nodiscard]] bool verify_existing_insecure_root_is_rejected() {
  auto const base = isolated_contract_path(L"insecure-acl");
  if (!base.has_value()) {
    return expect(false,
                  "the existing ACL contract root must be available");
  }
  std::error_code error;
  std::filesystem::create_directories(*base, error);
  if (error) {
    return expect(false, "the existing ACL contract directory must be created");
  }

  auto const root = *base / L"device-data";
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  auto const descriptor_created =
      ::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &raw_descriptor, nullptr);
  LocalMemory descriptor{raw_descriptor, &::LocalFree};
  bool passed = expect(
      descriptor_created != FALSE && raw_descriptor != nullptr,
      "the insecure ACL descriptor must be prepared");
  if (descriptor_created != FALSE && raw_descriptor != nullptr) {
    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = raw_descriptor,
        .bInheritHandle = FALSE,
    };
    auto const created = ::CreateDirectoryW(root.c_str(), &attributes);
    passed &= expect(
        created != FALSE,
        "the existing insecure ACL root must be created before preparation");
  }

  if (passed) {
    auto const result = azzs::testing::prepare_windows_device_data_for_test(
        WindowsDeviceDataTestOptions{.root_override_utf8 = utf8(root),
                                     .subject_override = "S-1-5-32-544"});
    passed &= expect(
        !result &&
            result.error == DeviceDataEnvironmentError::unsafe_storage_path &&
            result.raw_error == ERROR_INVALID_SECURITY_DESCR,
        "an existing root with an insecure ACL must fail closed");
    passed &= expect(
        std::filesystem::is_directory(root) &&
            !std::filesystem::exists(root / L"device") &&
            !std::filesystem::exists(root / L"locks") &&
            !std::filesystem::exists(root / L"subjects"),
        "an insecure existing root must not receive managed child directories");
  }

  std::filesystem::remove_all(*base, error);
  return passed;
}

[[nodiscard]] bool verify_nested_reparse_is_rejected() {
  auto const base = isolated_contract_path(L"nested-reparse");
  if (!base.has_value()) {
    return expect(false,
                  "the nested reparse contract root must be available");
  }
  auto const root = *base / L"device-data";
  auto const target = *base / L"target";
  std::error_code error;
  std::filesystem::create_directories(target, error);
  if (error) {
    return expect(false, "the nested reparse target must be created");
  }
  auto environment = azzs::testing::prepare_windows_device_data_for_test(
      WindowsDeviceDataTestOptions{.root_override_utf8 = utf8(root),
                                   .subject_override = "S-1-5-32-544"});
  bool passed = expect(static_cast<bool>(environment),
                       "the nested reparse test root must be prepared");
  if (!environment) {
    std::filesystem::remove_all(*base, error);
    return passed;
  }

  auto const aggregate_link =
      root / L"device" / L"state" / L"nested-reparse";
  auto created = ::CreateSymbolicLinkW(
      aggregate_link.c_str(), target.c_str(),
      SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  if (!created) {
    created = ::CreateSymbolicLinkW(aggregate_link.c_str(), target.c_str(),
                                    SYMBOLIC_LINK_FLAG_DIRECTORY);
  }
  passed &= expect(created != FALSE,
                   "the nested reparse contract requires a directory symlink");
  if (created) {
    WindowsStateFileSystem files{*environment.environment};
    auto const key = azzs::domain::StateKey::machine(
        azzs::domain::AggregateId{"nested-reparse"});
    std::array<std::byte, 1> const marker{std::byte{0x3b}};
    auto const write = files.write(
        key, azzs::application::StateFileSlot::candidate, marker);
    passed &= expect(
        !write.succeeded() &&
            !std::filesystem::exists(target / L"candidate.transaction"),
        "state writes must not follow a reparse aggregate introduced after preparation");
    ::RemoveDirectoryW(aggregate_link.c_str());
  }
  std::filesystem::remove_all(*base, error);
  return passed;
}

[[nodiscard]] bool verify_interactive_identity_evidence() {
  auto const base = isolated_contract_path(L"identity");
  if (!base.has_value()) {
    return expect(false,
                  "the Windows identity contract root must be available");
  }
  std::error_code cleanup_error;
  std::filesystem::create_directories(*base, cleanup_error);
  if (cleanup_error) {
    return expect(false, "the isolated identity contract directory must be created");
  }

  auto const current_sid = current_user_sid();
  bool passed = expect(current_sid.has_value(),
                       "the identity fallback contract requires a process SID");
  if (!current_sid.has_value()) {
    std::filesystem::remove_all(*base, cleanup_error);
    return passed;
  }

  auto const wts_mismatch_root = *base / L"wts-mismatch";
  auto const wts_mismatch =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .wts_session_sid = "S-1-5-32-544",
              .desktop_shell_sid = *current_sid,
          });
  passed &= expect(
      !wts_mismatch &&
          wts_mismatch.error ==
              DeviceDataEnvironmentError::alternate_credentials_not_supported &&
          wts_mismatch.raw_error == ERROR_ACCESS_DENIED &&
          !std::filesystem::exists(wts_mismatch_root),
      "a different WTS SID must reject before a matching shell fallback is considered");

  auto const wts_matching_root = *base / L"matching-wts";
  auto const wts_matching =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .wts_session_sid = *current_sid,
              .desktop_shell_sid = "S-1-5-32-544",
          });
  passed &= expect(
      static_cast<bool>(wts_matching) && wts_matching.subject_id == *current_sid,
      "a matching WTS SID must be authoritative even when shell evidence differs");

  auto const missing_domain_root = *base / L"missing-domain";
  auto const missing_domain =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .wts_user_name = "unqualified-account-must-not-be-looked-up",
              .wts_domain_name = std::string{},
              .desktop_shell_raw_error = ERROR_NOT_FOUND,
          });
  passed &= expect(
      !missing_domain &&
          missing_domain.error ==
              DeviceDataEnvironmentError::interactive_subject_unavailable &&
          missing_domain.raw_error == ERROR_NONE_MAPPED &&
          !std::filesystem::exists(missing_domain_root),
      "a WTS username without a domain must not be resolved as an account name");

  auto const domain_rpc_matching =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .desktop_shell_sid = *current_sid,
              .wts_user_name = "wts-user-with-unavailable-domain",
              .wts_domain_raw_error = RPC_S_SERVER_UNAVAILABLE,
              .desktop_shell_session_matches = true,
          });
  passed &= expect(
      static_cast<bool>(domain_rpc_matching) &&
          domain_rpc_matching.subject_id == *current_sid,
      "a WTS domain RPC failure may use a matching same-session shell SID");

  auto const domain_rpc_unavailable =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .wts_user_name = "wts-user-with-unavailable-domain",
              .wts_domain_raw_error = RPC_S_SERVER_UNAVAILABLE,
              .desktop_shell_raw_error = ERROR_NOT_FOUND,
          });
  passed &= expect(
      !domain_rpc_unavailable &&
          domain_rpc_unavailable.error ==
              DeviceDataEnvironmentError::interactive_subject_unavailable &&
          domain_rpc_unavailable.raw_error == RPC_S_SERVER_UNAVAILABLE,
      "a WTS domain RPC error must survive an unavailable shell fallback");

  auto const matching_root = *base / L"matching-shell";
  auto const matching =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .desktop_shell_sid = *current_sid,
              .wts_raw_error = RPC_S_SERVER_UNAVAILABLE,
              .desktop_shell_session_matches = true,
          });
  passed &= expect(
      static_cast<bool>(matching) && matching.subject_id == *current_sid,
      "an unavailable WTS/RPC path may use a matching same-session shell SID");

  auto const non_rpc_wts_failure =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .desktop_shell_sid = *current_sid,
              .wts_raw_error = ERROR_ACCESS_DENIED,
              .desktop_shell_session_matches = true,
          });
  passed &= expect(
      !non_rpc_wts_failure &&
          non_rpc_wts_failure.error ==
              DeviceDataEnvironmentError::interactive_subject_unavailable &&
          non_rpc_wts_failure.raw_error == ERROR_ACCESS_DENIED,
      "a non-RPC WTS failure must not use a matching shell fallback");

  auto const unmapped_wts_identity =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .desktop_shell_sid = *current_sid,
              .wts_raw_error = ERROR_NONE_MAPPED,
              .desktop_shell_session_matches = true,
          });
  passed &= expect(
      !unmapped_wts_identity &&
          unmapped_wts_identity.error ==
              DeviceDataEnvironmentError::interactive_subject_unavailable &&
          unmapped_wts_identity.raw_error == ERROR_NONE_MAPPED,
      "an unmapped WTS identity must fail closed despite a matching shell SID");

  auto const mismatched_session_root =
      *base / L"mismatched-shell-session";
  auto const mismatched_session =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .desktop_shell_sid = *current_sid,
              .wts_raw_error = RPC_S_SERVER_UNAVAILABLE,
              .desktop_shell_session_matches = false,
          });
  passed &= expect(
      !mismatched_session &&
          mismatched_session.error ==
              DeviceDataEnvironmentError::interactive_subject_unavailable &&
          mismatched_session.raw_error == RPC_S_SERVER_UNAVAILABLE &&
          !std::filesystem::exists(mismatched_session_root),
      "a shell from another session must not become the fallback subject");

  auto const alternate_root = *base / L"alternate-shell";
  auto const alternate =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .desktop_shell_sid = "S-1-5-32-544",
              .wts_raw_error = RPC_S_SERVER_UNAVAILABLE,
              .desktop_shell_session_matches = true,
          });
  passed &= expect(
      !alternate &&
          alternate.error ==
              DeviceDataEnvironmentError::alternate_credentials_not_supported &&
          alternate.raw_error == ERROR_ACCESS_DENIED &&
          !std::filesystem::exists(alternate_root),
      "a different desktop shell SID must reject alternate administrator credentials");

  auto const unavailable_root = *base / L"missing-shell";
  auto const unavailable =
      azzs::testing::resolve_windows_device_data_subject_for_test(
          WindowsDeviceDataIdentityEvidence{
              .process_sid = *current_sid,
              .wts_raw_error = RPC_S_SERVER_UNAVAILABLE,
              .desktop_shell_raw_error = ERROR_NOT_FOUND,
          });
  passed &= expect(
      !unavailable &&
          unavailable.error ==
              DeviceDataEnvironmentError::interactive_subject_unavailable &&
          unavailable.raw_error == RPC_S_SERVER_UNAVAILABLE &&
          !std::filesystem::exists(unavailable_root),
      "the primary WTS/RPC error must survive an unavailable shell fallback");

  std::filesystem::remove_all(*base, cleanup_error);
  return passed;
}

}  // namespace

int main() {
  std::cout << "token integrity: " << token_integrity_label() << '\n';
  bool passed = true;
  passed &= verify_layout_acl_and_slots();
  passed &= verify_non_local_root_is_rejected();
  passed &= verify_diagnostic_root_guards();
  passed &= verify_reparse_root_is_rejected();
  passed &= verify_existing_insecure_root_is_rejected();
  passed &= verify_nested_reparse_is_rejected();
  passed &= verify_interactive_identity_evidence();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "windows device data contract passed\n";
  return EXIT_SUCCESS;
}
