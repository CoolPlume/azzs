#include "azzs/adapters/infrastructure/system_settings_recovery_store.hpp"

#include <cstdint>
#include <cstring>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace azzs::adapters::infrastructure {
namespace {

constexpr std::uint32_t k_format_version = 3;

[[nodiscard]] domain::StateKey key() {
  return domain::StateKey::machine(domain::AggregateId{"system-settings-recovery"});
}

class Writer final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
  void u16(std::uint16_t value) {
    for (unsigned index = 0; index < 2; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
    }
  }
  void u32(std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
    }
  }
  void u64(std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
      u8(static_cast<std::uint8_t>((value >> (index * 8)) & 0xffU));
    }
  }
  void text(std::string const& value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (auto byte : value) {
      u8(static_cast<std::uint8_t>(byte));
    }
  }
  [[nodiscard]] domain::StateBytes finish() && { return std::move(bytes_); }

 private:
  domain::StateBytes bytes_;
};

class Reader final {
 public:
  explicit Reader(domain::StateBytes const& bytes) : bytes_(bytes) {}
  [[nodiscard]] bool u8(std::uint8_t& value) {
    if (offset_ >= bytes_.size()) return false;
    value = static_cast<std::uint8_t>(bytes_[offset_++]);
    return true;
  }
  [[nodiscard]] bool u16(std::uint16_t& value) {
    value = 0;
    for (unsigned index = 0; index < 2; ++index) {
      std::uint8_t byte{};
      if (!u8(byte)) return false;
      value |= static_cast<std::uint16_t>(byte) << (index * 8);
    }
    return true;
  }
  [[nodiscard]] bool u32(std::uint32_t& value) {
    value = 0;
    for (unsigned index = 0; index < 4; ++index) {
      std::uint8_t byte{};
      if (!u8(byte)) return false;
      value |= static_cast<std::uint32_t>(byte) << (index * 8);
    }
    return true;
  }
  [[nodiscard]] bool u64(std::uint64_t& value) {
    value = 0;
    for (unsigned index = 0; index < 8; ++index) {
      std::uint8_t byte{};
      if (!u8(byte)) return false;
      value |= static_cast<std::uint64_t>(byte) << (index * 8);
    }
    return true;
  }
  [[nodiscard]] bool text(std::string& value) {
    std::uint32_t size{};
    if (!u32(size) || size > 1U * 1024U * 1024U ||
        bytes_.size() - offset_ < size) {
      return false;
    }
    value.assign(reinterpret_cast<char const*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }
  [[nodiscard]] bool done() const noexcept { return offset_ == bytes_.size(); }

 private:
  domain::StateBytes const& bytes_;
  std::size_t offset_{0};
};

void encode_value(Writer& writer,
                  application::WindowsSystemSettingValue const& value) {
  if (auto const* classic =
          std::get_if<application::ClassicContextMenuMode>(&value)) {
    writer.u8(1);
    writer.u8(static_cast<std::uint8_t>(*classic));
  } else {
    writer.u8(2);
    writer.u8(static_cast<std::uint8_t>(
        std::get<application::ExplorerPresentationMode>(value)));
  }
}

[[nodiscard]] bool decode_value(
    Reader& reader, application::WindowsSystemSettingValue& value) {
  std::uint8_t type{};
  std::uint8_t raw{};
  if (!reader.u8(type) || !reader.u8(raw)) return false;
  if (type == 1 && raw <= 1) {
    value = static_cast<application::ClassicContextMenuMode>(raw);
    return true;
  }
  if (type == 2 && raw <= 1) {
    value = static_cast<application::ExplorerPresentationMode>(raw);
    return true;
  }
  return false;
}

void encode_windows_version(
    Writer& writer, domain::settings_catalog::WindowsVersion const& version) {
  writer.u8(static_cast<std::uint8_t>(version.generation));
  writer.u16(version.feature_update_year);
  writer.u8(version.feature_update_half);
}

[[nodiscard]] bool decode_windows_version(
    Reader& reader, domain::settings_catalog::WindowsVersion& version) {
  std::uint8_t generation{};
  if (!reader.u8(generation) || !reader.u16(version.feature_update_year) ||
      !reader.u8(version.feature_update_half) ||
      generation > static_cast<std::uint8_t>(
                       domain::settings_catalog::WindowsGeneration::windows_11)) {
    return false;
  }
  version.generation =
      static_cast<domain::settings_catalog::WindowsGeneration>(generation);
  return version.valid();
}

void encode_range(Writer& writer,
                  domain::settings_catalog::WindowsVersionRange const& range) {
  writer.u8(range.minimum.has_value() ? 1 : 0);
  if (range.minimum.has_value()) {
    encode_windows_version(writer, *range.minimum);
  }
  writer.u8(range.maximum.has_value() ? 1 : 0);
  if (range.maximum.has_value()) {
    encode_windows_version(writer, *range.maximum);
  }
}

[[nodiscard]] bool decode_range(
    Reader& reader, domain::settings_catalog::WindowsVersionRange& range) {
  std::uint8_t has_minimum{};
  std::uint8_t has_maximum{};
  if (!reader.u8(has_minimum) || has_minimum > 1) {
    return false;
  }
  if (has_minimum == 1) {
    domain::settings_catalog::WindowsVersion value;
    if (!decode_windows_version(reader, value)) return false;
    range.minimum = value;
  }
  if (!reader.u8(has_maximum) || has_maximum > 1) {
    return false;
  }
  if (has_maximum == 1) {
    domain::settings_catalog::WindowsVersion value;
    if (!decode_windows_version(reader, value)) return false;
    range.maximum = value;
  }
  return range.valid();
}

void encode_optional_value(
    Writer& writer,
    std::optional<application::WindowsSystemSettingValue> const& value) {
  writer.u8(value.has_value() ? 1 : 0);
  if (value.has_value()) {
    encode_value(writer, *value);
  }
}

[[nodiscard]] bool decode_optional_value(
    Reader& reader, std::optional<application::WindowsSystemSettingValue>& value) {
  std::uint8_t present{};
  if (!reader.u8(present) || present > 1) return false;
  if (present == 1) {
    application::WindowsSystemSettingValue decoded;
    if (!decode_value(reader, decoded)) return false;
    value = std::move(decoded);
  }
  return true;
}

void encode_operation_fact(Writer& writer,
                           application::SystemSettingsOperationFact const& fact) {
  writer.u64(fact.fact_id);
  writer.u8(static_cast<std::uint8_t>(fact.operation));
  writer.u8(static_cast<std::uint8_t>(fact.catalog_availability));
  writer.text(fact.catalog_identity);
  writer.u64(fact.catalog_revision);
  writer.text(fact.catalog_reason);
  writer.u8(fact.selected_plan_id.has_value() ? 1 : 0);
  if (fact.selected_plan_id.has_value()) {
    writer.text(fact.selected_plan_id->value);
  }
  writer.text(fact.selected_plan_name);
  writer.u8(static_cast<std::uint8_t>(fact.windows_environment.availability));
  writer.text(fact.windows_environment.display_version);
  writer.u32(fact.windows_environment.internal_build);
  writer.text(fact.windows_environment.reason);
  writer.u8(fact.explorer_restart_requested ? 1 : 0);
  writer.u8(static_cast<std::uint8_t>(fact.explorer_restart_result));
  writer.u8(fact.windows_restart_barrier ? 1 : 0);
  writer.u8(static_cast<std::uint8_t>(fact.status));
  writer.text(fact.reason);
  writer.u32(static_cast<std::uint32_t>(fact.settings.size()));
  for (auto const& setting : fact.settings) {
    writer.text(setting.setting_id.value);
    writer.text(setting.display_name);
    writer.text(setting.controlled_identity);
    writer.u64(setting.catalog_revision);
    writer.u8(static_cast<std::uint8_t>(setting.declared_range_availability));
    encode_range(writer, setting.declared_windows_range);
    writer.text(setting.declared_range_reason);
    encode_optional_value(writer, setting.original_value);
    encode_optional_value(writer, setting.target_value);
    writer.u8(setting.recovery_record_id.has_value() ? 1 : 0);
    if (setting.recovery_record_id.has_value()) {
      writer.u64(*setting.recovery_record_id);
    }
    writer.u8(static_cast<std::uint8_t>(setting.restart_requirement));
    writer.u8(setting.force_attempt_confirmed ? 1 : 0);
    writer.u8(static_cast<std::uint8_t>(setting.status));
    writer.text(setting.reason);
  }
  writer.u32(static_cast<std::uint32_t>(fact.timeline.size()));
  for (auto const& entry : fact.timeline) {
    writer.u32(entry.ordinal);
    writer.text(entry.stage);
    writer.u8(static_cast<std::uint8_t>(entry.status));
    writer.text(entry.reason);
  }
}

[[nodiscard]] bool decode_operation_fact(
    Reader& reader, application::SystemSettingsOperationFact& fact) {
  std::uint8_t operation{};
  std::uint8_t catalog_availability{};
  std::uint8_t plan_present{};
  std::uint8_t environment_availability{};
  std::uint8_t restart_requested{};
  std::uint8_t restart_result{};
  std::uint8_t restart_barrier{};
  std::uint8_t status{};
  std::uint32_t setting_count{};
  if (!reader.u64(fact.fact_id) || !reader.u8(operation) ||
      !reader.u8(catalog_availability) || !reader.text(fact.catalog_identity) ||
      !reader.u64(fact.catalog_revision) || !reader.text(fact.catalog_reason) ||
      !reader.u8(plan_present) || plan_present > 1) {
    return false;
  }
  if (plan_present == 1 && !reader.text(fact.selected_plan_id.emplace().value)) {
    return false;
  }
  if (!reader.text(fact.selected_plan_name) ||
      !reader.u8(environment_availability) ||
      !reader.text(fact.windows_environment.display_version) ||
      !reader.u32(fact.windows_environment.internal_build) ||
      !reader.text(fact.windows_environment.reason) ||
      !reader.u8(restart_requested) || restart_requested > 1 ||
      !reader.u8(restart_result) || !reader.u8(restart_barrier) ||
      restart_barrier > 1 || !reader.u8(status) || !reader.text(fact.reason) ||
      !reader.u32(setting_count) || setting_count > 100'000 ||
      operation > static_cast<std::uint8_t>(
                      application::SystemSettingsOperationKind::restart_explorer) ||
      catalog_availability > static_cast<std::uint8_t>(
                                 application::SystemSettingsFactAvailability::
                                     not_obtained) ||
      environment_availability > static_cast<std::uint8_t>(
                                     application::SystemSettingsFactAvailability::
                                         not_obtained) ||
      restart_result > static_cast<std::uint8_t>(
                           application::SystemSettingsExplorerRestartResult::
                               verification_failed) ||
      status > static_cast<std::uint8_t>(
                   application::SystemSettingsOperationStatus::blocked)) {
    return false;
  }
  fact.operation = static_cast<application::SystemSettingsOperationKind>(operation);
  fact.catalog_availability =
      static_cast<application::SystemSettingsFactAvailability>(catalog_availability);
  fact.windows_environment.availability =
      static_cast<application::SystemSettingsFactAvailability>(
          environment_availability);
  fact.explorer_restart_requested = restart_requested == 1;
  fact.explorer_restart_result =
      static_cast<application::SystemSettingsExplorerRestartResult>(restart_result);
  fact.windows_restart_barrier = restart_barrier == 1;
  fact.status = static_cast<application::SystemSettingsOperationStatus>(status);
  fact.settings.clear();
  fact.settings.reserve(setting_count);
  for (std::uint32_t index = 0; index < setting_count; ++index) {
    application::SystemSettingsOperationSettingFact setting;
    std::uint8_t range_availability{};
    std::uint8_t recovery_present{};
    std::uint8_t restart_requirement{};
    std::uint8_t force_attempt{};
    std::uint8_t setting_status{};
    if (!reader.text(setting.setting_id.value) || !reader.text(setting.display_name) ||
        !reader.text(setting.controlled_identity) ||
        !reader.u64(setting.catalog_revision) || !reader.u8(range_availability) ||
        !decode_range(reader, setting.declared_windows_range) ||
        !reader.text(setting.declared_range_reason) ||
        !decode_optional_value(reader, setting.original_value) ||
        !decode_optional_value(reader, setting.target_value) ||
        !reader.u8(recovery_present) || recovery_present > 1) {
      return false;
    }
    if (recovery_present == 1) {
      std::uint64_t value{};
      if (!reader.u64(value)) return false;
      setting.recovery_record_id = value;
    }
    if (!reader.u8(restart_requirement) ||
        restart_requirement > static_cast<std::uint8_t>(
                                  domain::settings_catalog::RestartRequirement::
                                      windows) ||
        !reader.u8(force_attempt) || force_attempt > 1 ||
        !reader.u8(setting_status) || !reader.text(setting.reason) ||
        range_availability > static_cast<std::uint8_t>(
                                 application::SystemSettingsFactAvailability::
                                     not_obtained) ||
        setting_status > static_cast<std::uint8_t>(
                             application::SystemSettingsOperationStatus::blocked)) {
      return false;
    }
    setting.declared_range_availability =
        static_cast<application::SystemSettingsFactAvailability>(range_availability);
    setting.restart_requirement =
        static_cast<domain::settings_catalog::RestartRequirement>(restart_requirement);
    setting.force_attempt_confirmed = force_attempt == 1;
    setting.status =
        static_cast<application::SystemSettingsOperationStatus>(setting_status);
    fact.settings.push_back(std::move(setting));
  }
  std::uint32_t timeline_count{};
  if (!reader.u32(timeline_count) || timeline_count > 1'000) return false;
  fact.timeline.clear();
  fact.timeline.reserve(timeline_count);
  for (std::uint32_t index = 0; index < timeline_count; ++index) {
    application::SystemSettingsOperationTimelineEntry entry;
    std::uint8_t entry_status{};
    if (!reader.u32(entry.ordinal) || !reader.text(entry.stage) ||
        !reader.u8(entry_status) || !reader.text(entry.reason) ||
        entry_status > static_cast<std::uint8_t>(
                            application::SystemSettingsOperationStatus::blocked)) {
      return false;
    }
    entry.status = static_cast<application::SystemSettingsOperationStatus>(
        entry_status);
    fact.timeline.push_back(std::move(entry));
  }
  return true;
}

[[nodiscard]] domain::StateBytes encode(
    std::vector<application::SystemSettingsRecoveryRecord> const& records,
    application::SystemSettingsOperationHistory const& history) {
  Writer writer;
  writer.u32(k_format_version);
  writer.u32(static_cast<std::uint32_t>(records.size()));
  for (auto const& record : records) {
    writer.u64(record.record_id);
    writer.text(record.setting_id.value);
    writer.text(record.display_name);
    writer.u64(record.catalog_revision);
    writer.u8(static_cast<std::uint8_t>(record.setting));
    encode_value(writer, record.original_value);
    writer.u8(static_cast<std::uint8_t>(record.restart_requirement));
    writer.u8(static_cast<std::uint8_t>(record.status));
    writer.u8(static_cast<std::uint8_t>(record.operation));
  }
  writer.u32(static_cast<std::uint32_t>(history.facts.size()));
  for (auto const& fact : history.facts) {
    encode_operation_fact(writer, fact);
  }
  return std::move(writer).finish();
}

[[nodiscard]] bool decode(
    domain::StateBytes const& bytes,
    std::vector<application::SystemSettingsRecoveryRecord>& records,
    application::SystemSettingsOperationHistory& history) {
  Reader reader{bytes};
  std::uint32_t version{};
  std::uint32_t count{};
  if (!reader.u32(version) ||
      (version != 1 && version != 2 && version != k_format_version) ||
      !reader.u32(count) || count > 100'000) {
    return false;
  }
  records.clear();
  history.facts.clear();
  records.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    application::SystemSettingsRecoveryRecord record;
    std::uint8_t setting{};
    std::uint8_t restart{};
    std::uint8_t status{};
    if (!reader.u64(record.record_id) || !reader.text(record.setting_id.value) ||
        (version >= 2 && !reader.text(record.display_name)) ||
        !reader.u64(record.catalog_revision) || !reader.u8(setting) ||
        !decode_value(reader, record.original_value) || !reader.u8(restart) ||
        !reader.u8(status) ||
        setting > static_cast<std::uint8_t>(
                     application::ControlledSystemSetting::windows10_explorer) ||
        restart > static_cast<std::uint8_t>(
                      domain::settings_catalog::RestartRequirement::windows) ||
        status > static_cast<std::uint8_t>(
                     application::RecoveryRecordStatus::restore_failed)) {
      return false;
    }
    std::uint8_t operation{};
    if (version >= 2 &&
        (!reader.u8(operation) || operation > static_cast<std::uint8_t>(
                                           application::RecoveryRecordOperation::
                                               windows11_default))) {
      return false;
    }
    record.setting = static_cast<application::ControlledSystemSetting>(setting);
    record.restart_requirement =
        static_cast<domain::settings_catalog::RestartRequirement>(restart);
    record.status = static_cast<application::RecoveryRecordStatus>(status);
    record.operation =
        static_cast<application::RecoveryRecordOperation>(operation);
    records.push_back(std::move(record));
  }
  if (version >= 3) {
    std::uint32_t fact_count{};
    if (!reader.u32(fact_count) || fact_count > 100'000) {
      return false;
    }
    history.facts.reserve(fact_count);
    for (std::uint32_t index = 0; index < fact_count; ++index) {
      application::SystemSettingsOperationFact fact;
      if (!decode_operation_fact(reader, fact)) {
        return false;
      }
      history.facts.push_back(std::move(fact));
    }
  }
  return reader.done();
}

}  // namespace

SystemSettingsRecoveryStore::SystemSettingsRecoveryStore(
    application::DeviceStateStore& states) noexcept
    : states_(states) {}

application::RecoveryStorageRead SystemSettingsRecoveryStore::read() {
  auto inspected = states_.inspect(key());
  if (inspected.mode == application::StateReadMode::uninitialized) {
    loaded_ = true;
    return {.status = application::RecoveryStorageStatus::loaded};
  }
  if (inspected.mode == application::StateReadMode::recovered_previous ||
      inspected.mode == application::StateReadMode::read_only_future ||
      inspected.mode == application::StateReadMode::read_only_corrupt) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = inspected.error.empty()
                          ? "系统设置恢复记录处于只读状态"
                          : inspected.error};
  }
  if (inspected.mode == application::StateReadMode::busy ||
      inspected.mode == application::StateReadMode::failed) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = inspected.error.empty() ? "无法读取系统设置恢复记录"
                                               : inspected.error};
  }
  if (!inspected.snapshot.has_value()) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = inspected.error};
  }
  std::vector<application::SystemSettingsRecoveryRecord> records;
  application::SystemSettingsOperationHistory history;
  if (!decode(inspected.snapshot->state.value.payload, records, history)) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = "系统设置恢复记录格式无效"};
  }
  records_ = std::move(records);
  operation_history_ = std::move(history);
  revision_ = inspected.snapshot->revision;
  loaded_ = true;
  return {.status = application::RecoveryStorageStatus::loaded,
          .records = records_,
          .operation_history = operation_history_};
}

application::RecoveryStorageWrite SystemSettingsRecoveryStore::save(
    application::SystemSettingsRecoveryRecord record) {
  if (!loaded_ && read().status != application::RecoveryStorageStatus::loaded) {
    return {.detail = "系统设置恢复记录尚未加载"};
  }
  auto next_records = records_;
  auto found = std::ranges::find(
      next_records, record.record_id,
      &application::SystemSettingsRecoveryRecord::record_id);
  if (found == next_records.end()) {
    next_records.push_back(std::move(record));
  } else {
    *found = std::move(record);
  }
  auto state = domain::DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode(next_records, operation_history_)}};
  application::StateCommitResult committed;
  if (!revision_.has_value()) {
    committed = states_.initialize(key(), std::move(state));
  } else {
    committed = states_.commit(application::StateCommitRequest{
        .key = key(), .expected_revision = *revision_, .state = std::move(state)});
  }
  if (committed.status != application::StateCommitStatus::committed ||
      !committed.snapshot.has_value()) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = committed.error.empty() ? "恢复记录提交失败"
                                               : committed.error};
  }
  revision_ = committed.snapshot->revision;
  records_ = std::move(next_records);
  return {.status = application::RecoveryStorageStatus::committed};
}

application::RecoveryStorageWrite SystemSettingsRecoveryStore::append_operation_fact(
    application::SystemSettingsOperationFact fact) {
  if (!loaded_ && read().status != application::RecoveryStorageStatus::loaded) {
    return {.detail = "系统设置恢复记录尚未加载"};
  }
  auto next_history = operation_history_;
  next_history.facts.push_back(std::move(fact));
  auto state = domain::DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode(records_, next_history)}};
  application::StateCommitResult committed;
  if (!revision_.has_value()) {
    committed = states_.initialize(key(), std::move(state));
  } else {
    committed = states_.commit(application::StateCommitRequest{
        .key = key(), .expected_revision = *revision_, .state = std::move(state)});
  }
  if (committed.status != application::StateCommitStatus::committed ||
      !committed.snapshot.has_value()) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = committed.error.empty() ? "系统设置操作历史提交失败"
                                               : committed.error};
  }
  revision_ = committed.snapshot->revision;
  operation_history_ = std::move(next_history);
  return {.status = application::RecoveryStorageStatus::committed};
}

application::RecoveryStorageWrite SystemSettingsRecoveryStore::erase(
    std::uint64_t record_id) {
  if (!loaded_ && read().status != application::RecoveryStorageStatus::loaded) {
    return {.detail = "系统设置恢复记录尚未加载"};
  }
  auto next_records = records_;
  auto const found = std::ranges::find(
      next_records, record_id,
      &application::SystemSettingsRecoveryRecord::record_id);
  if (found == next_records.end()) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = "恢复记录不存在"};
  }
  next_records.erase(found);
  auto state = domain::DeviceState{
      .value = {.schema = 2,
                .minimum_reader = 1,
                .minimum_writer = 2,
                .payload = encode(next_records, operation_history_)}};
  application::StateCommitResult committed;
  if (!revision_.has_value()) {
    committed = states_.initialize(key(), std::move(state));
  } else {
    committed = states_.commit(application::StateCommitRequest{
        .key = key(), .expected_revision = *revision_, .state = std::move(state)});
  }
  if (committed.status != application::StateCommitStatus::committed ||
      !committed.snapshot.has_value()) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = committed.error.empty() ? "恢复记录删除提交失败"
                                               : committed.error};
  }
  revision_ = committed.snapshot->revision;
  records_ = std::move(next_records);
  return {.status = application::RecoveryStorageStatus::committed};
}

}  // namespace azzs::adapters::infrastructure
