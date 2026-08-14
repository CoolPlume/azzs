#pragma once

#include "azzs/application/execution_log.hpp"
#include "azzs/application/installation_batch.hpp"
#include "azzs/application/offline_package_cache.hpp"

namespace azzs::adapters::windows {

// This port talks to the batch-owned cache service only. It deliberately
// receives opaque cache identities rather than a filename or a source URL.
class WindowsInstallationDownloadAdapter final
    : public application::installation_batch::InstallationDownloadPort {
 public:
  explicit WindowsInstallationDownloadAdapter(
      application::offline_package_cache::OfflinePackageCacheService& cache)
      : cache_(cache) {}

  [[nodiscard]] application::installation_batch::InstallationDownloadObservation
  advance(application::installation_batch::InstallationEffectTarget const& target)
      override;
  [[nodiscard]] application::installation_batch::InstallationDownloadObservation
  pause(application::installation_batch::InstallationEffectTarget const& target)
      override;
  [[nodiscard]] application::installation_batch::InstallationDownloadObservation
  resume(application::installation_batch::InstallationEffectTarget const& target)
      override;
  [[nodiscard]] application::installation_batch::InstallationDownloadObservation
  stop(application::installation_batch::InstallationEffectTarget const& target)
      override;

 private:
  application::offline_package_cache::OfflinePackageCacheService& cache_;
};

// Profile declarations are intentionally independent from real Windows
// execution registrations. This adapter keeps an unregistered or incomplete
// profile unavailable rather than treating the declaration as authorization.
class WindowsControlledProfileReadinessAdapter final
    : public application::installation_batch::ControlledProfileReadinessPort {
 public:
  [[nodiscard]] application::installation_batch::ControlledProfileReadiness
  observe(domain::installation_batch::FrozenExecutionProfile const& profile)
      override;
};

// A cached artifact can be addressed only by its controlled cache identity.
// This adapter owns the cache observation, but deliberately has no registered
// process launcher until a project-owned production profile is completed.
class WindowsOpaqueCacheInstallerLauncher final
    : public application::installation_batch::ControlledInstallerExecutor {
 public:
  explicit WindowsOpaqueCacheInstallerLauncher(
      application::offline_package_cache::OfflinePackageCacheService& cache)
      : cache_(cache) {}

  [[nodiscard]] application::installation_batch::ControlledInstallerObservation
  launch(application::installation_batch::ControlledInstallerLaunch const& request)
      override;
  [[nodiscard]] application::installation_batch::ControlledInstallerCompletionObservation
  observe_completion(
      application::installation_batch::ControlledInstallerCompletionRequest const& request)
      override;
  [[nodiscard]] application::installation_batch::ControlledInstallerTerminationObservation
  force_terminate(
      application::installation_batch::ControlledInstallerTerminationRequest const& request)
      override;

 private:
  application::offline_package_cache::OfflinePackageCacheService& cache_;
};

// Process exit is never converted into success here. Until an exact
// project-owned result verifier is registered, every executable result stays
// unknown and therefore keeps the batch paused.
class WindowsInstallationResultVerifier final
    : public application::installation_batch::InstallResultVerifier {
 public:
  [[nodiscard]] application::installation_batch::InstallVerificationObservation
  verify(application::installation_batch::InstallVerificationRequest const& request)
      override;
};

// This records only bounded, stable identifiers and state names. It never
// sends a cache path, source URL, command line, installer handle, or detail
// text into durable diagnostics.
class WindowsInstallationFactSink final
    : public application::installation_batch::InstallationFactSink {
 public:
  explicit WindowsInstallationFactSink(application::ExecutionLog& log);

  void observe(
      application::installation_batch::InstallationFact const& fact) noexcept
      override;

 private:
  application::ExecutionLog& log_;
  application::CorrelationId correlation_;
};

}  // namespace azzs::adapters::windows
