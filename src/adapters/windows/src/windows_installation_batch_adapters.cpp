#include "azzs/adapters/windows/windows_installation_batch_adapters.hpp"
#include "azzs/adapters/windows/windows_controlled_acquisition.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <optional>
#include <objbase.h>
#include <windows.h>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace azzs::adapters::windows {
namespace {

namespace batch = application::installation_batch;
namespace cache = application::offline_package_cache;

enum class ControlledInstallerKind {
  exe,
  msi,
};

struct ControlledInstallerRegistration final {
  std::string_view profile_id;
  std::string_view software_id;
  ControlledInstallerKind kind;
};

// This table is the complete project-owned launch surface.  No catalog text,
// cache filename, source URL, or user input can select an executable or add
// installer arguments.
constexpr std::array k_controlled_installer_registrations{
    ControlledInstallerRegistration{"qq-windows-v1", "qq", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"sogou-input-defaults-v1", "sogou-input", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"game-cheats-manager-windows-v1", "game-cheats-manager", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"cheat-engine-windows-v1", "cheat-engine", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"office-tool-plus-windows-v1", "office-tool-plus", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"internet-download-manager-windows-v1", "internet-download-manager", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"the-geometers-sketchpad-windows-v1", "the-geometers-sketchpad", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"java-runtime-windows-v1", "java-runtime", ControlledInstallerKind::msi},
    ControlledInstallerRegistration{"dotnet-runtime-windows-v1", "dotnet-runtime", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"directx-runtime-windows-v1", "directx-runtime", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"powershell-7-windows-v1", "powershell-7", ControlledInstallerKind::msi},
};

[[nodiscard]] std::optional<ControlledInstallerKind> controlled_installer_kind(
    domain::installation_batch::FrozenExecutionProfile const& profile,
    std::string_view software_id) noexcept {
  auto const found = std::ranges::find_if(
      k_controlled_installer_registrations,
      [&](ControlledInstallerRegistration const& registration) {
        return registration.profile_id == profile.profile_id &&
               registration.software_id == software_id;
      });
  if (found == k_controlled_installer_registrations.end()) {
    return std::nullopt;
  }
  return found->kind;
}

[[nodiscard]] bool profile_registered(
    domain::installation_batch::FrozenExecutionProfile const& profile,
    std::string_view software_id) {
  if (!profile.valid()) {
    return false;
  }
  auto const profiles = domain::software_catalog::initial_controlled_install_profiles();
  auto const found = std::ranges::find(profiles, profile.profile_id,
                                       &domain::software_catalog::ControlledInstallProfile::id);
  return found != profiles.end() && found->software_id == software_id &&
         found->execution_kind == profile.executor &&
         found->execution == profile.execution &&
         found->execution == domain::software_catalog::WindowsExecutionReadiness::project_executor_registered &&
         found->completion_boundary == profile.completion_boundary &&
         found->post_install_behavior == profile.post_install &&
         found->restart_verification == profile.restart &&
         found->result_detection == profile.result_detection &&
         found->interaction_scope == profile.interaction_scope &&
         std::ranges::find(found->baselines, profile.baseline) != found->baselines.end();
}

[[nodiscard]] bool profile_registered(
    domain::installation_batch::FrozenExecutionProfile const& profile) {
  auto const profiles = domain::software_catalog::initial_controlled_install_profiles();
  auto const found = std::ranges::find(profiles, profile.profile_id,
                                       &domain::software_catalog::ControlledInstallProfile::id);
  return found != profiles.end() && profile_registered(profile, found->software_id);
}

[[nodiscard]] bool has_msi_signature(std::span<std::byte const> bytes) noexcept {
  static constexpr std::array<std::byte, 8> k_signature{
      std::byte{0xD0}, std::byte{0xCF}, std::byte{0x11}, std::byte{0xE0},
      std::byte{0xA1}, std::byte{0xB1}, std::byte{0x1A}, std::byte{0xE1}};
  return bytes.size() >= k_signature.size() &&
         std::ranges::equal(bytes.first(k_signature.size()), k_signature);
}

[[nodiscard]] bool has_pe_signature(std::span<std::byte const> bytes) noexcept {
  return bytes.size() >= 2U && bytes[0] == std::byte{'M'} &&
         bytes[1] == std::byte{'Z'};
}

[[nodiscard]] std::optional<std::filesystem::path> controlled_payload_path(
    std::span<std::byte const> bytes,
    std::wstring_view extension) {
  wchar_t temp_buffer[MAX_PATH]{};
  auto const length = ::GetTempPathW(static_cast<DWORD>(std::size(temp_buffer)),
                                     temp_buffer);
  if (length == 0 || length >= std::size(temp_buffer)) {
    return std::nullopt;
  }
  auto directory = std::filesystem::path{temp_buffer} / L"azzs-controlled-install";
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    return std::nullopt;
  }
  auto const directory_attributes = ::GetFileAttributesW(directory.c_str());
  if (directory_attributes == INVALID_FILE_ATTRIBUTES ||
      (directory_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (directory_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return std::nullopt;
  }

  GUID guid{};
  if (FAILED(::CoCreateGuid(&guid))) {
    return std::nullopt;
  }
  wchar_t guid_text[64]{};
  if (::StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) <= 0) {
    return std::nullopt;
  }
  auto const path = directory / (std::wstring{guid_text} + std::wstring{extension});
  auto const native = path.wstring();
  HANDLE file = ::CreateFileW(
      native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  bool written = true;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const remaining = bytes.size() - offset;
    auto const chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
    DWORD written_bytes{};
    if (!::WriteFile(file, bytes.data() + offset, chunk, &written_bytes, nullptr) ||
        written_bytes != chunk) {
      written = false;
      break;
    }
    offset += written_bytes;
  }
  if (written && !::FlushFileBuffers(file)) {
    written = false;
  }
  ::CloseHandle(file);
  if (!written) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    return std::nullopt;
  }
  return path;
}

[[nodiscard]] std::wstring quote_argument(std::wstring_view value) {
  std::wstring quoted;
  quoted.reserve(value.size() + 2U);
  quoted.push_back(L'"');
  quoted.append(value);
  quoted.push_back(L'"');
  return quoted;
}

[[nodiscard]] std::optional<std::wstring> system_msiexec_path() {
  wchar_t directory[MAX_PATH]{};
  auto const length = ::GetSystemDirectoryW(directory, static_cast<UINT>(std::size(directory)));
  if (length == 0 || length >= std::size(directory)) {
    return std::nullopt;
  }
  return std::wstring{directory, length} + L"\\msiexec.exe";
}

[[nodiscard]] bool valid_verification_phase(
    batch::InstallVerificationPhase phase) noexcept {
  switch (phase) {
    case batch::InstallVerificationPhase::before_launch:
    case batch::InstallVerificationPhase::after_process_exit:
    case batch::InstallVerificationPhase::recovery_read_only:
    case batch::InstallVerificationPhase::after_restart_read_only:
      return true;
  }
  return false;
}

[[nodiscard]] batch::InstallationDownloadObservation download_failure(
    std::string detail) {
  return {.code = batch::InstallationDownloadCode::failed,
          .detail = std::move(detail)};
}

[[nodiscard]] bool cached_asset_is_present(
    cache::OfflinePackageCacheService& service,
    domain::offline_package_cache::CacheAssetIdentity const& identity) {
  auto const snapshot = service.snapshot();
  return std::ranges::any_of(snapshot.items, [&identity](auto const& item) {
    return item.asset.identity == identity && item.cache_present;
  });
}

[[nodiscard]] bool frozen_cache_root_is_selected(
    cache::OfflinePackageCacheService& service,
    domain::offline_package_cache::ControlledCacheRoot const& root) {
  return service.snapshot().selected_root == root;
}

[[nodiscard]] char const* stage_for(
    batch::InstallationFactKind const kind) noexcept {
  switch (kind) {
    case batch::InstallationFactKind::batch_created:
      return "batch-created";
    case batch::InstallationFactKind::state_persisted:
      return "state-persisted";
    case batch::InstallationFactKind::launch_requested:
      return "launch-requested";
    case batch::InstallationFactKind::verification_observed:
      return "verification-observed";
    case batch::InstallationFactKind::batch_paused:
      return "batch-paused";
    case batch::InstallationFactKind::download_paused:
      return "download-paused";
    case batch::InstallationFactKind::download_resumed:
      return "download-resumed";
    case batch::InstallationFactKind::normal_stop_requested:
      return "normal-stop-requested";
    case batch::InstallationFactKind::forced_termination_confirmation_requested:
      return "forced-termination-confirmation-requested";
    case batch::InstallationFactKind::forced_termination_confirmation_cancelled:
      return "forced-termination-confirmation-cancelled";
    case batch::InstallationFactKind::forced_termination_observed:
      return "forced-termination-observed";
    case batch::InstallationFactKind::normal_close_requested:
      return "normal-close-requested";
    case batch::InstallationFactKind::recovery_continued:
      return "recovery-continued";
    case batch::InstallationFactKind::recovery_observed:
      return "recovery-observed";
    case batch::InstallationFactKind::coverage_gap:
      return "coverage-gap";
  }
  return "unknown";
}

}  // namespace

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::advance(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return {.code = batch::InstallationDownloadCode::source_invalid,
            .detail = "the frozen batch cache root is unavailable"};
  }

  auto const result = cache_.download(target.cache_asset.identity);
  switch (result.code) {
    case cache::CacheActionCode::completed:
    case cache::CacheActionCode::already_available:
      return {.code = batch::InstallationDownloadCode::cached_ready,
              .detail = "the controlled batch cache has a completed artifact"};
    case cache::CacheActionCode::paused:
      return {.code = batch::InstallationDownloadCode::paused,
              .detail = "the controlled batch cache transfer is paused"};
    case cache::CacheActionCode::waiting_for_network:
      return {.code = batch::InstallationDownloadCode::waiting_network,
              .detail = "the controlled batch cache requires a network"};
    case cache::CacheActionCode::unsupported:
    case cache::CacheActionCode::asset_not_current:
      return {.code = batch::InstallationDownloadCode::source_invalid,
              .detail = "the frozen batch asset is unavailable from its controlled cache"};
    case cache::CacheActionCode::restarted_from_zero:
    case cache::CacheActionCode::location_unavailable:
    case cache::CacheActionCode::insufficient_space:
    case cache::CacheActionCode::space_unknown:
    case cache::CacheActionCode::busy:
    case cache::CacheActionCode::retry_not_available:
    case cache::CacheActionCode::interrupted:
    case cache::CacheActionCode::failed:
      return download_failure(result.detail.empty()
                                  ? "the controlled batch cache could not provide an artifact"
                                  : result.detail);
  }
  return download_failure("the controlled batch cache returned an unknown result");
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::pause(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }
  // A synchronous cache transfer cannot honestly promise to interrupt an
  // in-flight transport at an arbitrary instant. Retain the existing transfer
  // state and require a registered downloader pause primitive before claiming
  // a user-requested pause.
  return download_failure(
      "no registered controlled downloader pause operation is available for this batch item");
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::resume(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }
  auto const result = cache_.resume(target.cache_asset.identity);
  switch (result.code) {
    case cache::CacheActionCode::completed:
    case cache::CacheActionCode::already_available:
      return {.code = batch::InstallationDownloadCode::cached_ready,
              .detail = result.detail};
    case cache::CacheActionCode::paused:
      return {.code = batch::InstallationDownloadCode::paused,
              .detail = result.detail};
    case cache::CacheActionCode::restarted_from_zero:
      return {.code = batch::InstallationDownloadCode::restart_required,
              .detail = result.detail.empty()
                            ? "download continuation is unavailable and must restart from zero"
                            : result.detail};
    case cache::CacheActionCode::waiting_for_network:
      return {.code = batch::InstallationDownloadCode::waiting_network,
              .detail = result.detail};
    case cache::CacheActionCode::unsupported:
    case cache::CacheActionCode::asset_not_current:
      return {.code = batch::InstallationDownloadCode::source_invalid,
              .detail = result.detail};
    case cache::CacheActionCode::interrupted:
    case cache::CacheActionCode::location_unavailable:
    case cache::CacheActionCode::insufficient_space:
    case cache::CacheActionCode::space_unknown:
    case cache::CacheActionCode::busy:
    case cache::CacheActionCode::retry_not_available:
    case cache::CacheActionCode::failed:
      return download_failure(result.detail.empty()
                                  ? "the controlled batch download could not be resumed"
                                  : result.detail);
  }
  return download_failure("the controlled batch cache returned an unknown resume result");
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::stop(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }

  auto const result = cache_.abandon(target.cache_asset.identity);
  switch (result.code) {
    case cache::CacheActionCode::interrupted:
      return {.code = batch::InstallationDownloadCode::stopped,
              .detail = "the controlled batch download temporary bytes were discarded"};
    case cache::CacheActionCode::asset_not_current:
    case cache::CacheActionCode::unsupported:
      return {.code = batch::InstallationDownloadCode::source_invalid,
              .detail = result.detail};
    case cache::CacheActionCode::completed:
    case cache::CacheActionCode::already_available:
    case cache::CacheActionCode::paused:
    case cache::CacheActionCode::restarted_from_zero:
    case cache::CacheActionCode::waiting_for_network:
    case cache::CacheActionCode::location_unavailable:
    case cache::CacheActionCode::insufficient_space:
    case cache::CacheActionCode::space_unknown:
    case cache::CacheActionCode::busy:
    case cache::CacheActionCode::retry_not_available:
    case cache::CacheActionCode::failed:
      return download_failure(result.detail.empty()
                                  ? "the controlled batch download could not be stopped"
                                  : result.detail);
  }
  return download_failure("the controlled batch cache returned an unknown stop result");
}

batch::ControlledProfileReadiness
WindowsControlledProfileReadinessAdapter::observe(
    domain::installation_batch::FrozenExecutionProfile const& profile) {
  if (!profile.valid()) {
    return {.code = batch::ControlledProfileReadinessCode::failed,
            .detail = "the frozen controlled profile is invalid"};
  }
  if (profile.execution ==
      domain::software_catalog::WindowsExecutionReadiness::declaration_only ||
      !profile_registered(profile)) {
    return {.code = batch::ControlledProfileReadinessCode::unavailable,
            .detail = "the controlled profile has no matching project-owned Windows executor"};
  }
  return {.code = batch::ControlledProfileReadinessCode::registered,
          .detail = "project-owned Windows executor is registered for this profile"};
}

WindowsOpaqueCacheInstallerLauncher::~WindowsOpaqueCacheInstallerLauncher() {
  std::scoped_lock lock{mutex_};
  for (auto found = operations_.begin(); found != operations_.end();) {
    auto current = found++;
    auto const native = static_cast<HANDLE>(current->second.process);
    if (native != nullptr && ::WaitForSingleObject(native, 0) == WAIT_TIMEOUT) {
      // Closing the workbench must not terminate an official installer. There
      // is no durable operation record for this adapter, so the next process
      // cannot adopt this handle and must report the persisted batch outcome
      // as unknown. Keep the materialized payload while the child may still
      // use it; removing it here would race the running installer. The
      // in-memory record is dropped with this adapter and is not recoverable.
      ::CloseHandle(native);
      current->second.process = nullptr;
      continue;
    }
    cleanup_operation_locked(current, false);
  }
  // Any running child above is intentionally left outside this adapter's
  // observation boundary. Completed or terminated children have already had
  // their temporary payload removed by cleanup_operation_locked().
  operations_.clear();
}

bool WindowsOpaqueCacheInstallerLauncher::operation_matches_target(
    OwnedProcess const& operation,
    batch::InstallationEffectTarget const& target) const noexcept {
  return operation.item_id == target.item_id &&
         operation.profile_id == target.execution_profile.profile_id &&
         operation.baseline == target.execution_profile.baseline &&
         operation.cache_asset_identity == target.cache_asset.identity &&
         operation.cache_root == target.cache_root;
}

void WindowsOpaqueCacheInstallerLauncher::cleanup_operation_locked(
    std::unordered_map<std::string, OwnedProcess>::iterator found,
    bool terminate) noexcept {
  if (found == operations_.end()) {
    return;
  }
  auto const native = static_cast<HANDLE>(found->second.process);
  if (native != nullptr && terminate &&
      ::WaitForSingleObject(native, 0) == WAIT_TIMEOUT) {
    static_cast<void>(::TerminateProcess(native, 1));
    static_cast<void>(::WaitForSingleObject(native, 5'000));
  }
  if (native != nullptr) {
    ::CloseHandle(native);
  }
  std::error_code error;
  std::filesystem::remove(found->second.payload, error);
  operations_.erase(found);
}

batch::ControlledInstallerObservation
WindowsOpaqueCacheInstallerLauncher::launch(
    batch::ControlledInstallerLaunch const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "the controlled installer launch target is invalid"};
  }
  if (!frozen_cache_root_is_selected(cache_, request.target.cache_root)) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = "the frozen batch cache root is unavailable"};
  }
  if (!cached_asset_is_present(cache_, request.target.cache_asset.identity)) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = "the frozen controlled cache artifact is unavailable"};
  }
  auto const kind = controlled_installer_kind(request.target.execution_profile,
                                              request.target.item_id);
  if (!kind.has_value() || !profile_registered(request.target.execution_profile,
                                                request.target.item_id)) {
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "the frozen profile is not a registered project-owned Windows profile"};
  }
  auto const payload = cache_.read_completed_payload(request.target.cache_asset.identity);
  if (payload.code != cache::CompletedCachePayloadReadCode::found || payload.bytes.empty()) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = payload.detail.empty() ? "controlled cache payload is unavailable" : payload.detail};
  }
  auto const extension = *kind == ControlledInstallerKind::msi ? L".msi" : L".exe";
  auto const payload_is_valid = *kind == ControlledInstallerKind::msi
                                    ? has_msi_signature(payload.bytes)
                                    : has_pe_signature(payload.bytes);
  if (!payload_is_valid) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = "controlled cache payload is not a matching MSI or PE installer"};
  }
  auto path = controlled_payload_path(payload.bytes, extension);
  if (!path.has_value()) {
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "controlled installer payload could not be materialized"};
  }
  auto const native_path = path->wstring();
  std::wstring application_path = native_path;
  std::wstring command_line = quote_argument(native_path);
  if (*kind == ControlledInstallerKind::msi) {
    auto msiexec = system_msiexec_path();
    if (!msiexec.has_value() ||
        ::GetFileAttributesW(msiexec->c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::error_code error;
      std::filesystem::remove(*path, error);
      return {.code = batch::InstallerLaunchCode::failed,
              .detail = "the fixed Windows Installer executable is unavailable"};
    }
    application_path = *msiexec;
    command_line = quote_argument(application_path) + L" /i " + quote_argument(native_path) +
                   L" /norestart";
  }
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!::CreateProcessW(application_path.c_str(), command_line.data(), nullptr, nullptr,
                        FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
    std::error_code error;
    std::filesystem::remove(*path, error);
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "controlled installer process could not be created"};
  }
  ::CloseHandle(process.hThread);
  std::scoped_lock lock{mutex_};
  auto const handle = std::string{"azzs-op-"} +
                      std::to_string(next_operation_++);
  operations_.emplace(handle, OwnedProcess{.process = process.hProcess,
                                            .payload = std::move(*path),
                                            .item_id = request.target.item_id,
                                            .profile_id = request.target.execution_profile.profile_id,
                                            .baseline = request.target.execution_profile.baseline,
                                            .cache_asset_identity =
                                                request.target.cache_asset.identity,
                                            .cache_root = request.target.cache_root});
  return {.code = batch::InstallerLaunchCode::started,
          .opaque_operation_handle = handle,
          .detail = "controlled installer process started with an owned process handle"};
}

batch::ControlledInstallerCompletionObservation
WindowsOpaqueCacheInstallerLauncher::observe_completion(
    batch::ControlledInstallerCompletionRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "the controlled installer completion target is invalid"};
  }
  if (!request.opaque_operation_handle.has_value()) {
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer operation handle is unavailable"};
  }
  if (!profile_registered(request.target.execution_profile, request.target.item_id)) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "the frozen profile is not bound to this software item"};
  }
  std::scoped_lock lock{mutex_};
  auto found = operations_.find(*request.opaque_operation_handle);
  if (found == operations_.end() || found->second.process == nullptr) {
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer operation handle is not owned by this adapter"};
  }
  if (!operation_matches_target(found->second, request.target)) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "controlled installer operation is bound to a different frozen target"};
  }
  auto const native = static_cast<HANDLE>(found->second.process);
  auto const wait = ::WaitForSingleObject(native, 0);
  if (wait == WAIT_TIMEOUT) {
    return {.code = batch::InstallerCompletionCode::running,
            .detail = "controlled installer process is still running"};
  }
  if (wait != WAIT_OBJECT_0) {
    cleanup_operation_locked(found, false);
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer process wait observation failed"};
  }
  DWORD exit_code{};
  if (!::GetExitCodeProcess(native, &exit_code)) {
    cleanup_operation_locked(found, false);
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer process exit code is unavailable"};
  }
  auto const exit_success = exit_code == 0;
  cleanup_operation_locked(found, false);
  if (!exit_success) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "controlled installer exited unsuccessfully; result remains unconfirmed"};
  }
  return {.code = batch::InstallerCompletionCode::completed,
          .post_install = batch::PostInstallCompletionCode::not_required,
          .detail = "controlled installer process completed; registry verification is still required"};
}

batch::ControlledInstallerTerminationObservation
WindowsOpaqueCacheInstallerLauncher::force_terminate(
    batch::ControlledInstallerTerminationRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "the controlled installer termination target is invalid"};
  }
  if (!request.opaque_operation_handle.has_value() ||
      request.opaque_operation_handle->empty()) {
    return {.code = batch::InstallerTerminationCode::unknown,
            .detail = "the controlled installer operation handle is unavailable"};
  }
  std::scoped_lock lock{mutex_};
  auto found = operations_.find(*request.opaque_operation_handle);
  if (found == operations_.end() || found->second.process == nullptr) {
    return {.code = batch::InstallerTerminationCode::unknown,
            .detail = "controlled installer operation handle is not owned by this adapter"};
  }
  if (!operation_matches_target(found->second, request.target)) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "controlled installer operation is bound to a different frozen target"};
  }
  if (!profile_registered(request.target.execution_profile, request.target.item_id)) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "the frozen profile is not bound to this software item"};
  }
  auto const native = static_cast<HANDLE>(found->second.process);
  if (::WaitForSingleObject(native, 0) == WAIT_OBJECT_0) {
    cleanup_operation_locked(found, false);
    return {.code = batch::InstallerTerminationCode::terminated,
            .detail = "controlled installer process had already exited and owned resources were cleaned"};
  }
  if (!::TerminateProcess(native, 1) ||
      ::WaitForSingleObject(native, 5'000) != WAIT_OBJECT_0) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "owned controlled installer process could not be terminated"};
  }
  cleanup_operation_locked(found, false);
  return {.code = batch::InstallerTerminationCode::terminated,
          .detail = "owned controlled installer process was force-terminated"};
}

batch::InstallVerificationObservation
WindowsInstallationResultVerifier::verify(
    batch::InstallVerificationRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the controlled installation verification target is invalid"};
  }
  if (!valid_verification_phase(request.phase)) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the controlled verification phase is not recognized"};
  }
  if (!profile_registered(request.target.execution_profile, request.target.item_id)) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the frozen profile is not bound to this software item"};
  }
  if (request.target.execution_profile.result_detection !=
      domain::software_catalog::ResultDetectionStrategy::project_owned_presence_probe) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = "the frozen profile does not authorize a project-owned presence probe"};
  }
  if (request.phase != batch::InstallVerificationPhase::before_launch &&
      (!request.opaque_operation_handle.has_value() ||
       request.opaque_operation_handle->empty())) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = "the post-launch verification operation handle is unavailable"};
  }
  auto const observed = presence_.detect_version(
      request.target.item_id, request.target.execution_profile.baseline.version);
  if (!observed.completed) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = observed.detail.empty() ? "Windows presence observation is incomplete" : observed.detail};
  }
  if (observed.present) {
    return {.code = batch::InstallVerificationCode::installed,
            .detail = "fixed Windows uninstall registry presence was observed"};
  }
  return {.code = batch::InstallVerificationCode::absent,
          .detail = "fixed Windows uninstall registry presence was not observed"};
}

WindowsInstallationFactSink::WindowsInstallationFactSink(
    application::ExecutionLog& log)
    : log_(log), correlation_(log_.begin_correlation()) {}

void WindowsInstallationFactSink::observe(
    batch::InstallationFact const& fact) noexcept {
  try {
    application::ExecutionEvent event{
        .kind = fact.kind == batch::InstallationFactKind::coverage_gap
                    ? application::ExecutionEventKind::coverage_gap
                    : application::ExecutionEventKind::adapter_result,
        .component = "installation-batch-windows",
        .stage = stage_for(fact.kind),
        .result = fact.result,
        .fields = {
            {"batch_id", fact.batch_id,
             application::DiagnosticValueDisposition::retain},
            {"item_id", fact.item_id,
             application::DiagnosticValueDisposition::retain},
            {"item_state", domain::installation_batch::to_string(fact.item_state),
             application::DiagnosticValueDisposition::retain},
        },
    };
    static_cast<void>(log_.append(correlation_, event));
  } catch (...) {
    // The runner owns its durable outcome decision. Best-effort adapter facts
    // must never alter that decision or terminate a Windows host callback.
  }
}

}  // namespace azzs::adapters::windows
