#include "azzs/domain/controlled_install_profiles.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <unordered_set>
#include <utility>

namespace azzs::domain::software_catalog {
namespace {

constexpr std::array k_required_disposition_order{
    InteractionDisposition::controlled_automatic,
    InteractionDisposition::workbench_confirmation,
    InteractionDisposition::official_installer,
};

// These facts intentionally remain unknown until a real third-party
// installation has produced evidence on a Windows candidate. Registering a
// project-owned executor only establishes that the workbench can attempt the
// controlled operation; it does not establish installer or post-install facts.
SoftwareInstallFacts make_initial_facts(std::string software_id) {
  return {.software_id = std::move(software_id)};
}

std::array<SoftwareInstallFacts, 11> const k_initial_facts{{
    make_initial_facts("qq"),
    make_initial_facts("sogou-input"),
    make_initial_facts("game-cheats-manager"),
    make_initial_facts("cheat-engine"),
    make_initial_facts("office-tool-plus"),
    make_initial_facts("internet-download-manager"),
    make_initial_facts("the-geometers-sketchpad"),
    make_initial_facts("java-runtime"),
    make_initial_facts("dotnet-runtime"),
    make_initial_facts("directx-runtime"),
    make_initial_facts("powershell-7"),
}};

std::array<ControlledInstallProfile, 11> const k_initial_profiles{{
    {
        .id = "sogou-input-defaults-v1",
        .software_id = "sogou-input",
        .baselines = {{
            .id = "sogou-input-windows-16.7",
            .version = "16.7",
        }},
        .preferences = {{
            .id = "sogou-input-disable-search-candidates-v1",
            .phase = InstallPhase::custom_install,
            .effect = InstallPreferenceEffect::disable_sogou_search_candidates,
            .default_choice = PreferenceDefault::decline,
            .disposition_order = k_required_disposition_order,
            .required_for_first_release = true,
        },
                        {
                            .id = "sogou-input-decline-tencent-yuanbao-v1",
                            .phase = InstallPhase::installation_complete,
                            .effect =
                                InstallPreferenceEffect::decline_sogou_tencent_yuanbao,
                            .default_choice = PreferenceDefault::decline,
                            .disposition_order = k_required_disposition_order,
                            .required_for_first_release = true,
                        }},
        .execution_kind =
            ControlledWindowsExecutionKind::project_owned_windows_executor,
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary =
            InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::controlled_preferences,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope =
            InstallerInteractionScope::non_identity_preferences_only,
    },
    {
        .id = "qq-windows-v1",
        .software_id = "qq",
        .baselines = {{.id = "qq-windows-9.9.33", .version = "9.9.33"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "game-cheats-manager-windows-v1",
        .software_id = "game-cheats-manager",
        .baselines = {{.id = "game-cheats-manager-windows-2.4.6", .version = "2.4.6"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "cheat-engine-windows-v1",
        .software_id = "cheat-engine",
        .baselines = {{.id = "cheat-engine-windows-7.7", .version = "7.7"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "office-tool-plus-windows-v1",
        .software_id = "office-tool-plus",
        .baselines = {{.id = "office-tool-plus-windows-11.5.7.0", .version = "11.5.7.0"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "internet-download-manager-windows-v1",
        .software_id = "internet-download-manager",
        .baselines = {{.id = "internet-download-manager-windows-trial-2026-08", .version = "trial-2026-08"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "the-geometers-sketchpad-windows-v1",
        .software_id = "the-geometers-sketchpad",
        .baselines = {{.id = "the-geometers-sketchpad-windows-version-5", .version = "5"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "java-runtime-windows-v1",
        .software_id = "java-runtime",
        .baselines = {{.id = "java-runtime-windows-jdk-25", .version = "25"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "dotnet-runtime-windows-v1",
        .software_id = "dotnet-runtime",
        .baselines = {{.id = "dotnet-runtime-windows-10.0.11", .version = "10.0.11"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "directx-runtime-windows-v1",
        .software_id = "directx-runtime",
        .baselines = {{.id = "directx-runtime-windows-9.29.1974.1", .version = "9.29.1974.1"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
    {
        .id = "powershell-7-windows-v1",
        .software_id = "powershell-7",
        .baselines = {{.id = "powershell-7-windows-7.6.4", .version = "7.6.4"}},
         .execution = WindowsExecutionReadiness::project_executor_registered,
        .completion_boundary = InstallationCompletionBoundary::post_install_then_result_detection,
        .post_install_behavior = PostInstallBehavior::none,
        .restart_verification = RestartVerification::not_required,
        .result_detection = ResultDetectionStrategy::project_owned_presence_probe,
        .interaction_scope = InstallerInteractionScope::official_identity_required,
    },
}};

[[nodiscard]] bool valid_architecture(InstallerArchitecture value) noexcept {
  switch (value) {
    case InstallerArchitecture::x86:
    case InstallerArchitecture::x64:
    case InstallerArchitecture::arm64:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_fact_knowledge(FactKnowledge value) noexcept {
  switch (value) {
    case FactKnowledge::unknown:
    case FactKnowledge::confirmed:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_capability(CapabilitySupport value) noexcept {
  switch (value) {
    case CapabilitySupport::unknown:
    case CapabilitySupport::supported:
    case CapabilitySupport::unsupported:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_phase(InstallPhase value) noexcept {
  switch (value) {
    case InstallPhase::custom_install:
    case InstallPhase::installation_complete:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_effect(InstallPreferenceEffect value) noexcept {
  switch (value) {
    case InstallPreferenceEffect::disable_sogou_search_candidates:
    case InstallPreferenceEffect::decline_sogou_tencent_yuanbao:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_preference_default(PreferenceDefault value) noexcept {
  switch (value) {
    case PreferenceDefault::accept:
    case PreferenceDefault::decline:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_execution_readiness(
    WindowsExecutionReadiness value) noexcept {
  switch (value) {
    case WindowsExecutionReadiness::declaration_only:
    case WindowsExecutionReadiness::project_executor_registered:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_execution_kind(
    ControlledWindowsExecutionKind value) noexcept {
  return value ==
         ControlledWindowsExecutionKind::project_owned_windows_executor;
}

[[nodiscard]] bool valid_completion_boundary(
    InstallationCompletionBoundary value) noexcept {
  switch (value) {
    case InstallationCompletionBoundary::post_install_then_result_detection:
    case InstallationCompletionBoundary::post_install_then_restart_verification:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_post_install_behavior(
    PostInstallBehavior value) noexcept {
  switch (value) {
    case PostInstallBehavior::none:
    case PostInstallBehavior::controlled_preferences:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_restart_verification(
    RestartVerification value) noexcept {
  switch (value) {
    case RestartVerification::not_required:
    case RestartVerification::required_after_restart:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_result_detection(
    ResultDetectionStrategy value) noexcept {
  switch (value) {
    case ResultDetectionStrategy::project_owned_presence_probe:
    case ResultDetectionStrategy::user_confirmation_only:
      return true;
  }
  return false;
}

[[nodiscard]] bool valid_interaction_scope(
    InstallerInteractionScope value) noexcept {
  switch (value) {
    case InstallerInteractionScope::non_identity_preferences_only:
    case InstallerInteractionScope::official_identity_required:
      return true;
  }
  return false;
}

void add_issue(ControlledInstallProfileValidation& validation,
               ControlledInstallProfileIssueCode code,
               std::string profile_id, std::string message) {
  validation.issues.push_back({
      .code = code,
      .profile_id = std::move(profile_id),
      .message = std::move(message),
  });
}

void validate_facts(std::string const& item_id,
                    InstallCapabilityFacts const& facts,
                    ControlledInstallProfileValidation& validation) {
  auto const& architectures = facts.architectures;
  std::unordered_set<InstallerArchitecture> seen_architectures;
  auto invalid_architectures = !valid_fact_knowledge(architectures.knowledge) ||
                              (architectures.knowledge == FactKnowledge::unknown &&
                               !architectures.values.empty()) ||
                              (architectures.knowledge == FactKnowledge::confirmed &&
                               architectures.values.empty());
  for (auto const architecture : architectures.values) {
    invalid_architectures = invalid_architectures || !valid_architecture(architecture) ||
                             !seen_architectures.insert(architecture).second;
  }
  if (invalid_architectures) {
    add_issue(validation,
              ControlledInstallProfileIssueCode::invalid_architecture_facts,
              item_id,
              "architecture facts must be known without duplicates or explicitly unknown");
  }

  auto const invalid_capabilities =
      !valid_capability(facts.offline_install) ||
      !valid_capability(facts.silent_install) ||
      !valid_capability(facts.completion_boundary) ||
      !valid_capability(facts.post_install_behavior) ||
      !valid_capability(facts.restart_verification) ||
      !valid_capability(facts.result_detection);
  if (invalid_capabilities) {
    add_issue(validation,
              ControlledInstallProfileIssueCode::invalid_capability_facts,
              item_id,
              "install capability facts contain an unrecognized value");
  }
}

[[nodiscard]] RequiredReleaseInstallFacts release_requirement_from(
    SoftwareInstallFacts const& facts) {
  auto const architectures_confirmed =
      facts.capabilities.architectures.knowledge == FactKnowledge::confirmed &&
      !facts.capabilities.architectures.values.empty();
  auto const capability_confirmed = [](CapabilitySupport value) {
    return value != CapabilitySupport::unknown;
  };
  return {
      .software_id = facts.software_id,
      .architectures_confirmed = architectures_confirmed,
      .offline_install_confirmed =
          capability_confirmed(facts.capabilities.offline_install),
      .silent_install_confirmed =
          capability_confirmed(facts.capabilities.silent_install),
      .completion_boundary_confirmed =
          capability_confirmed(facts.capabilities.completion_boundary),
      .post_install_behavior_confirmed =
          capability_confirmed(facts.capabilities.post_install_behavior),
      .restart_verification_confirmed =
          capability_confirmed(facts.capabilities.restart_verification),
      .result_detection_confirmed =
          capability_confirmed(facts.capabilities.result_detection),
  };
}

}  // namespace

std::span<ControlledInstallProfile const>
initial_controlled_install_profiles() noexcept {
  return k_initial_profiles;
}

std::span<SoftwareInstallFacts const> initial_software_install_facts() noexcept {
  return k_initial_facts;
}

ControlledInstallProfileValidation validate_software_install_facts(
    std::span<SoftwareInstallFacts const> facts) {
  ControlledInstallProfileValidation validation;
  std::unordered_set<std::string> software_ids;
  for (auto const& fact : facts) {
    if (!valid_stable_id(fact.software_id)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_stable_id,
                fact.software_id,
                "software install facts require a stable software identifier");
    }
    if (!software_ids.insert(fact.software_id).second) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::duplicate_software_id,
                fact.software_id,
                "software install facts must identify each software once");
    }
    validate_facts(fact.software_id, fact.capabilities, validation);
  }
  return validation;
}

ControlledInstallProfileValidation validate_controlled_install_profiles(
    std::span<ControlledInstallProfile const> profiles) {
  ControlledInstallProfileValidation validation;
  std::unordered_set<std::string> profile_ids;
  std::unordered_set<std::string> software_ids;

  for (auto const& profile : profiles) {
    if (!valid_stable_id(profile.id) || !valid_stable_id(profile.software_id)) {
      add_issue(validation, ControlledInstallProfileIssueCode::invalid_stable_id,
                profile.id,
                "controlled install profiles require stable profile and software identifiers");
    }
    if (!profile_ids.insert(profile.id).second) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::duplicate_profile_id,
                profile.id, "controlled install profile id is duplicated");
    }
    if (!software_ids.insert(profile.software_id).second) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::duplicate_software_id,
                profile.id,
                "one software item may have only one controlled install profile");
    }

    std::unordered_set<std::string> baseline_ids;
    for (auto const& baseline : profile.baselines) {
      if (!valid_stable_id(baseline.id) || baseline.version.empty()) {
        add_issue(validation,
                  ControlledInstallProfileIssueCode::invalid_baseline,
                  profile.id,
                  "installer baselines require a stable id and observed version");
      } else if (!baseline_ids.insert(baseline.id).second) {
        add_issue(validation,
                  ControlledInstallProfileIssueCode::duplicate_baseline_id,
                  profile.id, "installer baseline id is duplicated");
      }
    }

    std::unordered_set<std::string> preference_ids;
    for (auto const& preference : profile.preferences) {
      auto const valid_preference =
          valid_stable_id(preference.id) && valid_phase(preference.phase) &&
          valid_effect(preference.effect) &&
          valid_preference_default(preference.default_choice) &&
          preference.disposition_order == k_required_disposition_order;
      if (!valid_preference) {
        add_issue(validation,
                  ControlledInstallProfileIssueCode::invalid_preference,
                  profile.id,
                  "install preferences require recognized phase, effect, default, and disposition order");
      } else if (!preference_ids.insert(preference.id).second) {
        add_issue(validation,
                  ControlledInstallProfileIssueCode::duplicate_preference_id,
                  profile.id, "install preference id is duplicated");
      }
    }

    if (!valid_execution_kind(profile.execution_kind)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_execution_kind,
                profile.id,
                "controlled install profiles require a project-owned execution kind");
    }
    if (!valid_execution_readiness(profile.execution)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_execution_readiness,
                profile.id,
                "controlled install profile execution readiness is unrecognized");
    }
    if (!valid_completion_boundary(profile.completion_boundary)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_completion_boundary,
                profile.id,
                "controlled install profile completion boundary is unrecognized");
    }
    if (!valid_post_install_behavior(profile.post_install_behavior)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_post_install_behavior,
                profile.id,
                "controlled install profile post-install behavior is unrecognized");
    }
    if (!valid_restart_verification(profile.restart_verification)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_restart_verification,
                profile.id,
                "controlled install profile restart verification is unrecognized");
    }
    if (!valid_result_detection(profile.result_detection)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_result_detection_strategy,
                profile.id,
                "controlled install profile result detection is unrecognized");
    }
    if (!valid_interaction_scope(profile.interaction_scope)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_interaction_scope,
                profile.id,
                "controlled install profile interaction scope is unrecognized");
    }
    auto const restart_boundary =
        profile.completion_boundary ==
        InstallationCompletionBoundary::post_install_then_restart_verification;
    auto const restart_required =
        profile.restart_verification == RestartVerification::required_after_restart;
    if (restart_boundary != restart_required) {
      add_issue(
          validation,
          ControlledInstallProfileIssueCode::inconsistent_completion_semantics,
          profile.id,
          "restart completion boundaries and restart verification must agree");
    }
  }
  return validation;
}

SoftwareCatalogPolicy initial_software_catalog_policy() {
  SoftwareCatalogPolicy policy{
      .supported_schema_version = 1,
      .supported_driver_hardware_kinds = {"gpu"},
  };
  for (auto const& profile : initial_controlled_install_profiles()) {
    policy.install_profiles.push_back({
        .id = profile.id,
        .software_ids = {profile.software_id},
        .runtime_status = profile.execution ==
                                  WindowsExecutionReadiness::project_executor_registered
                              ? InstallProfileRuntimeStatus::available
                              : InstallProfileRuntimeStatus::missing,
        // Runtime registration is not evidence that a vendor installer has
        // been validated; the formal release gate remains closed.
        .release_ready = false,
    });
    policy.required_install_profiles.push_back({
        .software_id = profile.software_id,
        .profile_id = profile.id,
    });
  }
  for (auto const& fact : initial_software_install_facts()) {
    policy.required_release_software.push_back(fact.software_id);
    policy.required_release_install_facts.push_back(
        release_requirement_from(fact));
  }
  policy.required_release_drivers = {
      {
          .id = "amd-auto-detect-and-install",
          .entry_type = DriverEntryType::assistant,
          .hardware_kind = "gpu",
          .primary_source_address =
              "https://www.amd.com/en/support/download/drivers.html",
      },
      {
          .id = "intel-gpu-driver-page",
          .entry_type = DriverEntryType::vendor_page,
          .hardware_kind = "gpu",
          .primary_source_address =
              "https://www.intel.com/content/www/us/en/download-center/home.html",
      },
      {
          .id = "nvidia-gpu-driver-page",
          .entry_type = DriverEntryType::vendor_page,
          .hardware_kind = "gpu",
          .primary_source_address = "https://www.nvidia.com/Download/index.aspx",
      },
  };
  return policy;
}

}  // namespace azzs::domain::software_catalog
