#include "azzs/application/installation_batch_creation.hpp"

#include <algorithm>
#include <functional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace azzs::application::installation_batch {
namespace {

namespace batch = domain::installation_batch;
namespace catalog = domain::software_catalog;
namespace selection = domain::software_selection;
namespace cache = domain::offline_package_cache;
namespace cache_app = offline_package_cache;

constexpr std::size_t kMaxBatchItems = 128;

[[nodiscard]] bool valid_request_id(std::string const& value) noexcept {
  return catalog::valid_stable_id(value);
}

[[nodiscard]] InstallationBatchCreationAssessment assessment(
    InstallationBatchCreationCode code, std::string detail = {},
    std::size_t item_count = 0) {
  return {.code = code, .item_count = item_count, .detail = std::move(detail)};
}

[[nodiscard]] bool contains_disposition(
    catalog::ControlledInstallPreference const& preference,
    catalog::InteractionDisposition disposition) noexcept {
  return std::ranges::find(preference.disposition_order, disposition) !=
         preference.disposition_order.end();
}

[[nodiscard]] batch::FrozenPreferenceValue frozen_value(
    catalog::PreferenceDefault value) noexcept {
  return value == catalog::PreferenceDefault::accept
             ? batch::FrozenPreferenceValue::accept
             : batch::FrozenPreferenceValue::decline;
}

[[nodiscard]] bool usable_cache_availability(
    cache_app::OfflinePackageAvailability availability) noexcept {
  switch (availability) {
    case cache_app::OfflinePackageAvailability::built_in_available:
    case cache_app::OfflinePackageAvailability::cached_available:
    case cache_app::OfflinePackageAvailability::ready_to_download:
    case cache_app::OfflinePackageAvailability::online_installer_requires_network:
      return true;
    case cache_app::OfflinePackageAvailability::waiting_for_network:
    case cache_app::OfflinePackageAvailability::managed_source_requires_network:
    case cache_app::OfflinePackageAvailability::unsupported:
      return false;
  }
  return false;
}

[[nodiscard]] std::optional<catalog::RuntimeSoftware const*> find_runtime(
    catalog::RuntimeSoftwareCatalog const& runtime, std::string const& software_id) {
  auto const found = std::ranges::find(runtime.software, software_id,
                                       [](catalog::RuntimeSoftware const& item) {
                                         return item.definition.id;
                                       });
  if (found == runtime.software.end()) {
    return std::nullopt;
  }
  return &*found;
}

[[nodiscard]] bool contains_selected(
    selection::SelectionState const& selected, std::string const& software_id) {
  return std::ranges::find(selected.selected_software_ids, software_id) !=
         selected.selected_software_ids.end();
}

[[nodiscard]] bool cache_item_matches(
    cache_app::OfflinePackageItemSnapshot const& item,
    cache::CacheAsset const& asset) noexcept {
  return item.asset == asset;
}

[[nodiscard]] bool result_holds_active_batch(
    InstallationBatchActionResult const& result,
    std::string const& batch_id) noexcept {
  return result.snapshot.active.has_value() &&
         result.snapshot.active->plan.batch_id == batch_id;
}

}  // namespace

LiveInstallationBatchPlanningState::LiveInstallationBatchPlanningState(
    software_catalog::SoftwareCatalogLifecycle const& catalogs,
    software_selection::SoftwareSelectionLifecycle const& selections,
    offline_package_cache::OfflinePackageCacheService& cache) noexcept
    : catalogs_(catalogs), selections_(selections), cache_(cache) {}

InstallationBatchPlanningSnapshot LiveInstallationBatchPlanningState::snapshot() {
  return {.catalog = catalogs_.snapshot(),
          .selection = selections_.snapshot(),
          .cache = cache_.snapshot()};
}

std::span<catalog::ControlledInstallProfile const>
InitialControlledInstallProfileCatalog::profiles() const noexcept {
  return catalog::initial_controlled_install_profiles();
}

bool FrozenBatchAssetRegistry::stage(batch::FrozenBatchPlan const& plan,
                                     EntryKind kind) noexcept {
  if (!plan.valid() ||
      ((kind == EntryKind::initial && plan.retry_of_batch_id.has_value()) ||
       (kind == EntryKind::retry && !plan.retry_of_batch_id.has_value())) ||
      std::ranges::any_of(entries_, [&](Entry const& entry) {
        return entry.plan.batch_id == plan.batch_id;
      })) {
    return false;
  }
  try {
    entries_.push_back({.plan = plan, .kind = kind});
  } catch (...) {
    return false;
  }
  return true;
}

bool FrozenBatchAssetRegistry::stage_initial(batch::FrozenBatchPlan const& plan) noexcept {
  return stage(plan, EntryKind::initial);
}

bool FrozenBatchAssetRegistry::stage_retry(batch::FrozenBatchPlan const& plan) noexcept {
  return stage(plan, EntryKind::retry);
}

void FrozenBatchAssetRegistry::discard(std::string const& batch_id) noexcept {
  auto const found = std::ranges::find(entries_, batch_id,
                                       [](Entry const& entry) -> std::string const& {
                                         return entry.plan.batch_id;
                                       });
  if (found != entries_.end() && !found->committed) {
    entries_.erase(found);
  }
}

void FrozenBatchAssetRegistry::commit(std::string const& batch_id) noexcept {
  auto const found = std::ranges::find(entries_, batch_id,
                                       [](Entry const& entry) -> std::string const& {
                                         return entry.plan.batch_id;
                                       });
  if (found != entries_.end()) {
    found->committed = true;
  }
}

std::optional<batch::FrozenBatchPlan> FrozenBatchAssetRegistry::find(
    std::string const& batch_id) const {
  auto const found = std::ranges::find(entries_, batch_id,
                                       [](Entry const& entry) -> std::string const& {
                                         return entry.plan.batch_id;
                                       });
  if (found == entries_.end()) {
    return std::nullopt;
  }
  return found->plan;
}

FrozenBatchPlanAdmission FrozenBatchAssetRegistry::admit_kind(
    batch::FrozenBatchPlan const& plan, EntryKind expected) const {
  auto const found = std::ranges::find_if(entries_, [&](Entry const& entry) {
    return entry.kind == expected && entry.plan == plan;
  });
  if (found == entries_.end()) {
    return {.code = FrozenBatchPlanAdmissionCode::rejected,
            .detail = "the frozen batch plan was not staged by the creation service"};
  }
  return {.code = FrozenBatchPlanAdmissionCode::accepted};
}

FrozenBatchPlanAdmission FrozenBatchAssetRegistry::admit(
    batch::FrozenBatchPlan const& plan) const {
  return admit_kind(plan, EntryKind::initial);
}

FrozenBatchPlanAdmission FrozenBatchAssetRegistry::admit_retry(
    batch::FrozenBatchPlan const& plan) const {
  return admit_kind(plan, EntryKind::retry);
}

void FrozenBatchAssetRegistry::observe_restored(
    batch::FrozenBatchPlan const& plan) const noexcept {
  if (!plan.valid()) {
    return;
  }
  auto const found = std::ranges::find(entries_, plan.batch_id,
                                       [](Entry const& entry) -> std::string const& {
                                         return entry.plan.batch_id;
                                       });
  try {
    if (found != entries_.end()) {
      found->plan = plan;
      found->kind = EntryKind::restored;
      found->committed = true;
      return;
    }
    entries_.push_back({.plan = plan, .kind = EntryKind::restored, .committed = true});
  } catch (...) {
  }
}

InstallationBatchCreationService::InstallationBatchCreationService(
    InstallationBatchService& batches, InstallationBatchPlanningStatePort& state,
    ControlledProfileReadinessPort& readiness,
    ControlledInstallProfileCatalog const& profiles,
    FrozenBatchAssetRegistry& assets) noexcept
    : batches_(batches), state_(state), readiness_(readiness), profiles_(profiles), assets_(assets) {}

InstallationBatchCreationService::PlanAssessment
InstallationBatchCreationService::assess_plan(
    InstallationBatchCreateRequest const& request) {
  if (!valid_request_id(request.batch_id) || !valid_request_id(request.correlation_id) ||
      request.frozen_at_milliseconds <= 0 || request.packages.empty() ||
      request.packages.size() > kMaxBatchItems) {
    return {.assessment = assessment(InstallationBatchCreationCode::invalid_request,
                                     "the batch request has an invalid identity, time, or size")};
  }

  std::unordered_set<std::string> request_ids;
  for (auto const& choice : request.packages) {
    if (!catalog::valid_stable_id(choice.software_id) ||
        !request_ids.insert(choice.software_id).second) {
      return {.assessment = assessment(InstallationBatchCreationCode::invalid_request,
                                       "each selected package must have one stable identity")};
    }
  }

  auto const planning = state_.snapshot();
  auto const& catalog_snapshot = planning.catalog;
  auto const& selection_snapshot = planning.selection;
  auto const& cache_snapshot = planning.cache;
  if (catalog_snapshot.mode != software_catalog::CatalogLifecycleMode::ready ||
      !catalog_snapshot.current.has_value() ||
      !catalog_snapshot.current_toml_bytes.has_value() ||
      !catalog_snapshot.current_catalog.has_value()) {
    return {.assessment = assessment(InstallationBatchCreationCode::not_ready,
                                     "the active software catalog is not ready")};
  }
  if (selection_snapshot.mode != software_selection::SelectionLifecycleMode::ready ||
      !selection_snapshot.has_current_catalog || !selection_snapshot.selection.initialized ||
      !selection_snapshot.active_catalog.has_value()) {
    return {.assessment = assessment(InstallationBatchCreationCode::selection_incomplete,
                                     "the controlled software selection is not ready")};
  }
  auto const& active_catalog = *catalog_snapshot.current;
  auto const& selected_catalog = *selection_snapshot.active_catalog;
  if (active_catalog.content_identity != selected_catalog.content_identity ||
      active_catalog.application_id != selected_catalog.application_id ||
      active_catalog.revision != selected_catalog.revision) {
    return {.assessment = assessment(InstallationBatchCreationCode::selection_incomplete,
                                     "the selection belongs to a different catalog revision")};
  }
  if (selection_snapshot.selection.selected_software_ids.size() != request_ids.size() ||
      !std::ranges::all_of(selection_snapshot.selection.selected_software_ids,
                           [&](std::string const& id) { return request_ids.contains(id); })) {
    return {.assessment = assessment(InstallationBatchCreationCode::selection_incomplete,
                                     "the request must freeze the complete current selection")};
  }
  if (!cache_snapshot.selected_root.valid() ||
      cache_snapshot.location_state != cache_app::CacheLocationState::available) {
    return {.assessment = assessment(InstallationBatchCreationCode::cache_unavailable,
                                     "the controlled cache root is unavailable")};
  }

  std::vector<InstallationBatchItemAssessment> item_assessments;
  item_assessments.reserve(request.packages.size());
  std::unordered_map<std::string, std::size_t> item_assessment_index;
  for (auto const& choice : request.packages) {
    item_assessment_index.emplace(choice.software_id, item_assessments.size());
    item_assessments.push_back({.item_id = choice.software_id});
  }
  auto mark_unavailable = [&](std::size_t index,
                              InstallationBatchCreationCode code,
                              std::string detail) {
    item_assessments[index].code = code;
    item_assessments[index].detail = std::move(detail);
  };
  auto mark_dependency_incomplete = [&](std::string const& software_id) {
    auto const found = item_assessment_index.find(software_id);
    if (found != item_assessment_index.end() &&
        item_assessments[found->second].ready()) {
      mark_unavailable(found->second,
                       InstallationBatchCreationCode::dependency_incomplete,
                       "the selected dependency closure is incomplete or cyclic");
    }
  };

  std::unordered_map<std::string, batch::FrozenInstallationItem> frozen_by_id;
  auto const profiles = profiles_.profiles();
  for (std::size_t choice_index = 0; choice_index < request.packages.size();
       ++choice_index) {
    auto const& choice = request.packages[choice_index];
    auto const runtime = find_runtime(*catalog_snapshot.current_catalog, choice.software_id);
    if (!runtime.has_value()) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::package_unavailable,
                       "a selected software item is unavailable");
      continue;
    }
    switch ((*runtime)->availability) {
      case catalog::ItemAvailability::available:
        break;
      case catalog::ItemAvailability::install_profile_unavailable:
        mark_unavailable(choice_index,
                         InstallationBatchCreationCode::profile_unavailable,
                         "the selected software has no controlled installation profile");
        continue;
      case catalog::ItemAvailability::missing_dependency:
      case catalog::ItemAvailability::dependency_cycle:
      case catalog::ItemAvailability::unavailable_dependency:
        mark_unavailable(choice_index,
                         InstallationBatchCreationCode::dependency_incomplete,
                         "the selected software has an incomplete dependency closure");
        continue;
    }
    if (!(*runtime)->definition.install_profile.has_value()) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::profile_unavailable,
                       "the selected software has no controlled installation profile");
      continue;
    }
    auto const selection_item = std::ranges::find(
        selection_snapshot.items, choice.software_id,
        [](selection::SelectionItem const& item) { return item.software_id; });
    if (selection_item == selection_snapshot.items.end() || !selection_item->selected ||
        !selection_item->available || selection_item->requires_reselection ||
        !contains_selected(selection_snapshot.selection, choice.software_id)) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::selection_incomplete,
                       "a selected software item is incomplete");
      continue;
    }

    auto const declared = std::ranges::find_if((*runtime)->definition.sources,
                                                [&](catalog::CatalogSource const& source) {
      return source.purpose.has_value() && *source.purpose == choice.declared_purpose &&
             source.address == choice.declared_address;
    });
    if (declared == (*runtime)->definition.sources.end()) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::source_unresolved,
                       "the requested source is not declared by the active catalog");
      continue;
    }
    auto const source = std::ranges::find_if(
        selection_snapshot.sources, [&](selection::ResolvedSourceSnapshot const& candidate) {
          return candidate.software_id == choice.software_id &&
                 candidate.declared_purpose == choice.declared_purpose &&
                 candidate.declared_address == choice.declared_address;
        });
    if (source == selection_snapshot.sources.end() || !source->valid() ||
        !source->stable_version() ||
        std::ranges::count_if(selection_snapshot.sources,
                              [&](selection::ResolvedSourceSnapshot const& candidate) {
          return candidate.software_id == choice.software_id &&
                 candidate.declared_purpose == choice.declared_purpose &&
                 candidate.declared_address == choice.declared_address;
                               }) != 1) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::source_unresolved,
                       "the controlled source snapshot is missing or ambiguous");
      continue;
    }
    auto const package = std::ranges::find(source->packages, choice.package_identity,
                                           [](selection::ResolvedPackage const& candidate) {
                                             return candidate.candidate.identity;
                                           });
    if (package == source->packages.end() ||
        package->package_type == selection::PackageType::external_handoff ||
        !package->complete_package &&
            package->package_type == selection::PackageType::full_package) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::package_unavailable,
                       "the selected package cannot be controlled");
      continue;
    }

    auto const profile = std::ranges::find_if(
        profiles, [&](catalog::ControlledInstallProfile const& candidate) {
          return candidate.id == *(*runtime)->definition.install_profile &&
                 candidate.software_id == choice.software_id;
        });
    if (profile == profiles.end()) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::profile_unavailable,
                       "the selected software has no controlled installation profile");
      continue;
    }
    auto const baseline = std::ranges::find(profile->baselines, source->version,
                                            [](catalog::InstallerBaseline const& candidate) {
                                              return candidate.version;
                                            });
    if (baseline == profile->baselines.end() ||
        profile->execution != catalog::WindowsExecutionReadiness::project_executor_registered) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::profile_unavailable,
                       "the controlled installation profile is not executable");
      continue;
    }

    std::unordered_map<std::string, batch::FrozenPreferenceValue> requested_choices;
    bool invalid_preference = false;
    for (auto const& preference : choice.preferences) {
      auto const known = std::ranges::find(profile->preferences, preference.preference_id,
                                           [](catalog::ControlledInstallPreference const& value) {
                                             return value.id;
                                           });
      if (!preference.valid() || known == profile->preferences.end() ||
          !requested_choices.emplace(preference.preference_id, preference.value).second) {
        mark_unavailable(choice_index,
                         InstallationBatchCreationCode::profile_unavailable,
                         "the request contains an unknown or duplicate preference");
        invalid_preference = true;
        break;
      }
    }
    if (invalid_preference) {
      continue;
    }
    if (profile->preferences.empty() &&
        (choice.interaction_disposition != catalog::InteractionDisposition::controlled_automatic ||
         !choice.preferences.empty())) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::profile_unavailable,
                       "a preference-free profile allows only controlled automation");
      continue;
    }
    std::vector<batch::FrozenPreferenceChoice> frozen_choices;
    frozen_choices.reserve(profile->preferences.size());
    bool invalid_disposition = false;
    for (auto const& preference : profile->preferences) {
      if (!contains_disposition(preference, choice.interaction_disposition)) {
        mark_unavailable(choice_index,
                         InstallationBatchCreationCode::profile_unavailable,
                         "the requested interaction disposition is not permitted");
        invalid_disposition = true;
        break;
      }
      auto const requested = requested_choices.find(preference.id);
        frozen_choices.push_back({.preference_id = preference.id,
                                .value = requested == requested_choices.end()
                                             ? frozen_value(preference.default_choice)
                                              : requested->second});
    }
    if (invalid_disposition) {
      continue;
    }
    batch::FrozenExecutionProfile frozen_profile{
        .profile_id = profile->id,
        .baseline = *baseline,
        .choices = std::move(frozen_choices),
        .executor = profile->execution_kind,
        .execution = profile->execution,
        .completion_boundary = profile->completion_boundary,
        .post_install = profile->post_install_behavior,
        .restart = profile->restart_verification,
        .result_detection = profile->result_detection,
        .interaction_scope = profile->interaction_scope,
        .interaction_disposition = choice.interaction_disposition,
    };
    if (!frozen_profile.valid() ||
        readiness_.observe(frozen_profile).code != ControlledProfileReadinessCode::registered) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::profile_unavailable,
                       "the project-owned controlled executor is not registered");
      continue;
    }

    auto const asset = cache_app::make_cache_asset(*source, *package);
    if (!asset.has_value() || asset->kind == cache::CacheAssetKind::managed_source ||
        asset->kind == cache::CacheAssetKind::online_only ||
        asset->kind == cache::CacheAssetKind::unsupported) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::cache_unavailable,
                       "the selected source does not have a controlled cache asset");
      continue;
    }
    auto const cached = std::ranges::find_if(cache_snapshot.items,
                                             [&](cache_app::OfflinePackageItemSnapshot const& item) {
      return cache_item_matches(item, *asset);
    });
    if (cached == cache_snapshot.items.end() || !usable_cache_availability(cached->availability)) {
      mark_unavailable(choice_index,
                       InstallationBatchCreationCode::cache_unavailable,
                       "the frozen cache asset is unavailable");
      continue;
    }
    auto const resource_kind =
        cached->cache_present ||
                cached->availability == cache_app::OfflinePackageAvailability::built_in_available
            ? batch::FrozenResourceKind::cached_package
            : batch::FrozenResourceKind::controlled_download;
    item_assessments[choice_index].code = InstallationBatchCreationCode::ready;
    frozen_by_id.emplace(choice.software_id,
                         batch::FrozenInstallationItem{
                             .item_id = choice.software_id,
                             .dependencies = (*runtime)->definition.dependencies,
                             .source = *source,
                             .selected_package = *package,
                             .execution_profile = std::move(frozen_profile),
                             .resource_kind = resource_kind,
                             .cache_asset = *asset,
                              .cache_root = cache_snapshot.selected_root,
                          });
  }

  std::unordered_map<std::string, int> visit_state;
  std::vector<batch::FrozenInstallationItem> ordered;
  ordered.reserve(frozen_by_id.size());
  std::function<bool(std::string const&, std::vector<batch::FrozenInstallationItem>&)> visit =
      [&](std::string const& id, std::vector<batch::FrozenInstallationItem>& local) {
    auto const item = frozen_by_id.find(id);
    if (item == frozen_by_id.end()) {
      return false;
    }
    auto& state = visit_state[id];
    if (state == 1) {
      mark_dependency_incomplete(id);
      return false;
    }
    if (state == 3) {
      return false;
    }
    if (state == 2) {
      return true;
    }
    state = 1;
    for (auto const& dependency : item->second.dependencies) {
      if (!request_ids.contains(dependency) || !visit(dependency, local)) {
        mark_dependency_incomplete(id);
        state = 3;
        return false;
      }
    }
    state = 2;
    local.push_back(item->second);
    return true;
  };
  for (auto const& choice : request.packages) {
    if (!frozen_by_id.contains(choice.software_id)) {
      continue;
    }
    std::vector<batch::FrozenInstallationItem> local;
    if (visit(choice.software_id, local)) {
      for (auto& item : local) {
        if (std::ranges::none_of(ordered, [&](batch::FrozenInstallationItem const& existing) {
              return existing.item_id == item.item_id;
            })) {
          ordered.push_back(std::move(item));
        }
      }
    }
  }

  auto with_items = [&](InstallationBatchCreationAssessment result) {
    result.items = item_assessments;
    return result;
  };
  if (ordered.empty()) {
    auto const failed = std::ranges::find_if(
        item_assessments, [](InstallationBatchItemAssessment const& item) {
          return !item.ready();
        });
    if (failed != item_assessments.end()) {
      return {.assessment = with_items(
                  assessment(failed->code, failed->detail))};
    }
    return {.assessment = with_items(
                assessment(InstallationBatchCreationCode::dependency_incomplete,
                           "the selected dependency closure is incomplete or cyclic"))};
  }

  batch::FrozenBatchPlan plan{
      .batch_id = request.batch_id,
      .correlation_id = request.correlation_id,
      .catalog = {.raw_catalog_bytes = *catalog_snapshot.current_toml_bytes,
                  .content_identity = active_catalog.content_identity,
                  .application_id = active_catalog.application_id,
                  .schema_version = catalog_snapshot.current_catalog->schema_version,
                  .revision = active_catalog.revision,
                  .release_state = catalog_snapshot.current_catalog->release_state,
                  .local_trial = active_catalog.identity ==
                                 software_catalog::EffectiveCatalogIdentity::local_trial},
      .items = std::move(ordered),
      .frozen_at_milliseconds = request.frozen_at_milliseconds,
  };
  if (!plan.valid()) {
    return {.assessment = with_items(
                assessment(InstallationBatchCreationCode::invalid_request,
                           "the requested frozen batch plan is invalid"))};
  }
  auto planned_assessment = assessment(
      InstallationBatchCreationCode::ready,
      ordered.size() == request.packages.size()
          ? std::string{}
          : "some selected software items are unavailable",
      plan.items.size());
  planned_assessment.items = std::move(item_assessments);
  return {.assessment = std::move(planned_assessment),
          .plan = std::move(plan)};
}

InstallationBatchCreationAssessment InstallationBatchCreationService::assess(
    InstallationBatchCreateRequest const& request) {
  return assess_plan(request).assessment;
}

InstallationBatchCreationResult InstallationBatchCreationService::create(
    InstallationBatchCreateRequest const& request) {
  auto planned = assess_plan(request);
  if (!planned.plan.has_value()) {
    return {.assessment = std::move(planned.assessment)};
  }
  auto const batch_id = planned.plan->batch_id;
  if (!assets_.stage_initial(*planned.plan)) {
    planned.assessment.code = InstallationBatchCreationCode::admission_rejected;
    planned.assessment.detail = "the frozen batch plan could not be staged";
    return {.assessment = std::move(planned.assessment)};
  }
  auto result = batches_.create(*planned.plan);
  if (result.succeeded()) {
    assets_.commit(batch_id);
    return {.assessment = std::move(planned.assessment), .batch = std::move(result)};
  }
  if (result_holds_active_batch(result, batch_id)) {
    assets_.commit(batch_id);
  } else {
    assets_.discard(batch_id);
  }
  planned.assessment.code = InstallationBatchCreationCode::batch_rejected;
  planned.assessment.detail = result.message;
  return {.assessment = std::move(planned.assessment),
           .batch = std::move(result)};
}

InstallationBatchCreationResult InstallationBatchCreationService::retry_current(
    InstallationBatchRetryRequest const& request) {
  if (!valid_request_id(request.batch_id) || !valid_request_id(request.correlation_id) ||
      request.frozen_at_milliseconds <= 0) {
    return {.assessment = assessment(InstallationBatchCreationCode::invalid_request,
                                     "the retry request has an invalid identity or time")};
  }
  auto const snapshot = batches_.snapshot();
  if (!snapshot.active.has_value()) {
    return {.assessment = assessment(InstallationBatchCreationCode::no_retryable_item,
                                     "there is no active installation batch to retry")};
  }
  auto const progress = std::ranges::find_if(
      snapshot.active->items, [](batch::InstallationItemProgress const& item) {
        return batch::requires_fresh_retry_snapshot(item.state) &&
               batch::command_allowed(item.state, batch::InstallationItemCommand::retry);
      });
  if (progress == snapshot.active->items.end()) {
    return {.assessment = assessment(InstallationBatchCreationCode::no_retryable_item,
                                     "the active batch has no retryable item")};
  }
  auto const original = assets_.find(snapshot.active->plan.batch_id);
  if (!original.has_value()) {
    return {.assessment = assessment(InstallationBatchCreationCode::admission_rejected,
                                     "the original frozen assets are unavailable for retry")};
  }
  auto const item = std::ranges::find(original->items, progress->item_id,
                                      [](batch::FrozenInstallationItem const& value) {
                                        return value.item_id;
                                      });
  if (item == original->items.end() || request.batch_id == original->batch_id) {
    return {.assessment = assessment(InstallationBatchCreationCode::invalid_request,
                                     "the retry must use a new batch identity")};
  }
  batch::FrozenBatchPlan retry{
      .batch_id = request.batch_id,
      .correlation_id = request.correlation_id,
      .retry_of_batch_id = original->batch_id,
      .catalog = original->catalog,
      .items = {*item},
      .frozen_at_milliseconds = request.frozen_at_milliseconds,
  };
  if (!retry.valid() || !assets_.stage_retry(retry)) {
    return {.assessment = assessment(InstallationBatchCreationCode::admission_rejected,
                                     "the retry plan could not be staged")};
  }
  auto result = batches_.retry_current(retry);
  if (result.succeeded()) {
    assets_.commit(request.batch_id);
    return {.assessment = assessment(InstallationBatchCreationCode::ready, {}, 1),
            .batch = std::move(result)};
  }
  if (result_holds_active_batch(result, request.batch_id)) {
    assets_.commit(request.batch_id);
  } else {
    assets_.discard(request.batch_id);
  }
  return {.assessment = assessment(InstallationBatchCreationCode::batch_rejected,
                                    result.message),
          .batch = std::move(result)};
}

char const* to_string(InstallationBatchCreationCode value) noexcept {
  switch (value) {
    case InstallationBatchCreationCode::ready:
      return "ready";
    case InstallationBatchCreationCode::not_ready:
      return "not-ready";
    case InstallationBatchCreationCode::invalid_request:
      return "invalid-request";
    case InstallationBatchCreationCode::selection_incomplete:
      return "selection-incomplete";
    case InstallationBatchCreationCode::source_unresolved:
      return "source-unresolved";
    case InstallationBatchCreationCode::package_unavailable:
      return "package-unavailable";
    case InstallationBatchCreationCode::profile_unavailable:
      return "profile-unavailable";
    case InstallationBatchCreationCode::cache_unavailable:
      return "cache-unavailable";
    case InstallationBatchCreationCode::dependency_incomplete:
      return "dependency-incomplete";
    case InstallationBatchCreationCode::admission_rejected:
      return "admission-rejected";
    case InstallationBatchCreationCode::no_retryable_item:
      return "no-retryable-item";
    case InstallationBatchCreationCode::batch_rejected:
      return "batch-rejected";
  }
  return "invalid-request";
}

}  // namespace azzs::application::installation_batch
