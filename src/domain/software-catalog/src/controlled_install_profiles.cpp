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

std::array<SoftwareInstallFacts, 11> const k_initial_facts{{
    {.software_id = "qq"},
    {.software_id = "sogou-input"},
    {.software_id = "game-cheats-manager"},
    {.software_id = "cheat-engine"},
    {.software_id = "office-tool-plus",
     .capabilities = {.architectures = {
         .knowledge = FactKnowledge::confirmed,
         .values = {InstallerArchitecture::x64, InstallerArchitecture::arm64},
     }}},
    {.software_id = "internet-download-manager"},
    {.software_id = "the-geometers-sketchpad"},
    {.software_id = "java-runtime"},
    {.software_id = "dotnet-runtime"},
    {.software_id = "directx-runtime"},
    {.software_id = "powershell-7",
     .capabilities = {
         .architectures = {
             .knowledge = FactKnowledge::confirmed,
             .values = {InstallerArchitecture::x64, InstallerArchitecture::arm64},
         },
     }},
}};

std::array<ControlledInstallProfile, 1> const k_initial_profiles{{
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
  return value == WindowsExecutionReadiness::declaration_only;
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

    if (!valid_execution_readiness(profile.execution)) {
      add_issue(validation,
                ControlledInstallProfileIssueCode::invalid_execution_readiness,
                profile.id,
                "issue 19 declarations cannot register executable behavior");
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
        .runtime_status = InstallProfileRuntimeStatus::missing,
        .release_ready = false,
    });
    if (profile.software_id == "sogou-input") {
      policy.required_install_profiles.push_back({
          .software_id = profile.software_id,
          .profile_id = profile.id,
      });
    }
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
