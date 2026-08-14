#include "azzs/adapters/windows/windows_installation_batch_adapters.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <utility>

namespace azzs::adapters::windows {
namespace {

namespace batch = application::installation_batch;
namespace cache = application::offline_package_cache;

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
WindowsInstallationDownloadAdapter::stop(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }

  // The current cache contract performs a transfer synchronously and exposes
  // no per-item cancellation operation. Reporting a pause would be false.
  return download_failure(
      "the controlled batch cache has no registered per-item cancellation operation");
}

batch::ControlledProfileReadiness
WindowsControlledProfileReadinessAdapter::observe(
    domain::installation_batch::FrozenExecutionProfile const& profile) {
  if (!profile.valid()) {
    return {.code = batch::ControlledProfileReadinessCode::failed,
            .detail = "the frozen controlled profile is invalid"};
  }
  if (profile.execution ==
      domain::software_catalog::WindowsExecutionReadiness::declaration_only) {
    return {.code = batch::ControlledProfileReadinessCode::unavailable,
            .detail = "the controlled profile is declaration-only"};
  }
  return {.code = batch::ControlledProfileReadinessCode::unavailable,
          .detail = "no complete production Windows executor is registered for this profile"};
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
  return {.code = batch::InstallerLaunchCode::failed,
          .detail = "no complete production Windows installer launcher is registered"};
}

batch::InstallVerificationObservation
WindowsInstallationResultVerifier::verify(
    batch::InstallVerificationRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the controlled installation verification target is invalid"};
  }
  if (request.target.execution_profile.execution ==
      domain::software_catalog::WindowsExecutionReadiness::declaration_only) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = "the controlled profile is declaration-only; no result may be inferred"};
  }
  return {.code = batch::InstallVerificationCode::unknown,
          .detail = "no complete production Windows installation verifier is registered"};
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
