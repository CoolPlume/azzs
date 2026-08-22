#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "azzs/domain/controlled_install_profiles.hpp"
#include "azzs/domain/offline_package_cache.hpp"
#include "azzs/domain/software_selection.hpp"

namespace azzs::domain::installation_batch {

namespace catalog = domain::software_catalog;
namespace cache = domain::offline_package_cache;
namespace selection = domain::software_selection;

enum class FrozenPreferenceValue {
  accept,
  decline,
};

enum class FrozenResourceKind {
  cached_package,
  controlled_download,
  managed_source,
};

struct FrozenPreferenceChoice final {
  std::string preference_id;
  FrozenPreferenceValue value{FrozenPreferenceValue::decline};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenPreferenceChoice const&) const = default;
};

// This is a value copy of every closed profile fact that determines execution.
// It intentionally uses the catalog-owned enums rather than a second, mutable
// interpretation of profile semantics.
struct FrozenExecutionProfile final {
  std::string profile_id;
  catalog::InstallerBaseline baseline;
  std::vector<FrozenPreferenceChoice> choices;
  catalog::ControlledWindowsExecutionKind executor{
      catalog::ControlledWindowsExecutionKind::project_owned_windows_executor};
  catalog::WindowsExecutionReadiness execution{
      catalog::WindowsExecutionReadiness::declaration_only};
  catalog::InstallationCompletionBoundary completion_boundary{
      catalog::InstallationCompletionBoundary::post_install_then_result_detection};
  catalog::PostInstallBehavior post_install{
      catalog::PostInstallBehavior::none};
  catalog::RestartVerification restart{
      catalog::RestartVerification::not_required};
  catalog::ResultDetectionStrategy result_detection{
      catalog::ResultDetectionStrategy::project_owned_presence_probe};
  catalog::InstallerInteractionScope interaction_scope{
      catalog::InstallerInteractionScope::non_identity_preferences_only};
  catalog::InteractionDisposition interaction_disposition{
      catalog::InteractionDisposition::controlled_automatic};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenExecutionProfile const&) const = default;
};

// The raw bytes are retained only to prove which already accepted catalog was
// frozen. They are never reparsed by the runner and are not passed to effects.
struct FrozenCatalogSnapshot final {
  std::string raw_catalog_bytes;
  std::string content_identity;
  std::string application_id;
  std::uint32_t schema_version{};
  std::uint64_t revision{};
  catalog::ReleaseState release_state{catalog::ReleaseState::draft};
  bool local_trial{true};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenCatalogSnapshot const&) const = default;
};

struct FrozenInstallationItem final {
  std::string item_id;
  std::vector<std::string> dependencies;
  selection::ResolvedSourceSnapshot source;
  selection::ResolvedPackage selected_package;
  FrozenExecutionProfile execution_profile;
  FrozenResourceKind resource_kind{FrozenResourceKind::controlled_download};
  cache::CacheAsset cache_asset;
  cache::ControlledCacheRoot cache_root;

  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] bool is_external_handoff() const noexcept;
  auto operator<=>(FrozenInstallationItem const&) const = default;
};

struct FrozenBatchPlan final {
  std::string batch_id;
  std::string correlation_id;
  std::optional<std::string> retry_of_batch_id;
  FrozenCatalogSnapshot catalog;
  std::vector<FrozenInstallationItem> items;
  std::int64_t frozen_at_milliseconds{};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(FrozenBatchPlan const&) const = default;
};

enum class InstallationItemState {
  pending,
  downloading,
  download_paused,
  installer_running,
  force_termination_confirmation_pending,
  waiting_network,
  source_invalid,
  installer_interaction_pending,
  result_confirmation_pending,
  waiting_restart,
  failed,
  skipped_installed,
  dependency_blocked,
  succeeded,
  stop_pending,
};

enum class InstallationBatchState {
  ready,
  running,
  download_paused,
  stopping,
  awaiting_user,
  waiting_restart,
  closing,
  stopped,
  completed,
  recovery_required,
  failed_closed,
};

enum class InstallationItemCommand {
  start,
  retry,
  stop,
  pause_download,
  resume_download,
  request_force_termination,
  confirm_force_termination,
  cancel_force_termination,
  user_complete_installer_interaction,
  user_complete_confirmation,
  read_only_verify,
};

struct InstallationItemProgress final {
  std::string item_id;
  InstallationItemState state{InstallationItemState::pending};
  std::uint32_t attempt{};
  // This is durably set before a launch effect. It remains true across an
  // interrupted launch so recovery can observe rather than issue it again.
  bool launch_requested{false};
  bool installer_started{false};
  // The request and observed forced termination are retained separately: a
  // confirmation never implies that an installer was actually terminated.
  bool force_termination_confirmation_requested{false};
  bool force_termination_completed{false};
  // A controlled post-install action needs an explicit completion fact before
  // result detection may establish this item as complete.
  bool post_install_completed{false};
  std::optional<std::string> opaque_installer_handle;
  std::string detail;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(InstallationItemProgress const&) const = default;
};

struct DurableLeaseBinding final {
  std::string kind;
  std::string operation_id;
  std::string correlation_id;
  // A durable record needs to recognize its own occupancy lease after a
  // restart, but must not expose or persist the bearer token itself.
  std::string lease_token_fingerprint;
  std::uint64_t occupancy_revision{};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(DurableLeaseBinding const&) const = default;
};

enum class DurableTransitionOutcome {
  committed,
  outcome_unknown,
  failed_closed,
};

struct LastDurableTransition final {
  std::uint64_t generation{};
  std::string item_id;
  InstallationItemState item_state{InstallationItemState::pending};
  DurableTransitionOutcome outcome{DurableTransitionOutcome::failed_closed};
  bool coverage_gap{false};

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(LastDurableTransition const&) const = default;
};

struct InstallationBatchRecord final {
  FrozenBatchPlan plan;
  InstallationBatchState state{InstallationBatchState::ready};
  bool close_requested{false};
  // Normal stop waits for an already-running installer but permanently
  // prevents any later item in this frozen batch from starting.
  bool stop_requested{false};
  std::vector<InstallationItemProgress> items;
  std::uint64_t generation{};
  std::optional<DurableLeaseBinding> active_lease;
  LastDurableTransition last_transition;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(InstallationBatchRecord const&) const = default;
};

struct InstallationBatchHistory final {
  FrozenBatchPlan plan;
  InstallationBatchState final_state{InstallationBatchState::failed_closed};
  std::vector<InstallationItemProgress> items;
  std::string reason;

  [[nodiscard]] bool valid() const noexcept;
  auto operator<=>(InstallationBatchHistory const&) const = default;
};

struct InstallationBatchSnapshot final {
  std::optional<InstallationBatchRecord> active;
  std::vector<InstallationBatchHistory> history;
  bool writable{false};
  std::string error;
};

[[nodiscard]] bool is_terminal(InstallationItemState state) noexcept;
[[nodiscard]] bool blocks_batch(InstallationItemState state) noexcept;
// These terminal failures may be retried only by creating a new, separately
// frozen batch. Pending user interaction, unverified results and restarts are
// deliberately not retryable failures.
[[nodiscard]] bool requires_fresh_retry_snapshot(InstallationItemState state) noexcept;
[[nodiscard]] bool command_allowed(InstallationItemState state,
                                   InstallationItemCommand command) noexcept;
[[nodiscard]] char const* to_string(FrozenResourceKind value) noexcept;
[[nodiscard]] char const* to_string(InstallationItemState value) noexcept;
[[nodiscard]] char const* to_string(InstallationBatchState value) noexcept;

}  // namespace azzs::domain::installation_batch
