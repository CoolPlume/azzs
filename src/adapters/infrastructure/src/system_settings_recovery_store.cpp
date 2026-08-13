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

constexpr std::uint32_t k_format_version = 2;

[[nodiscard]] domain::StateKey key() {
  return domain::StateKey::machine(domain::AggregateId{"system-settings-recovery"});
}

class Writer final {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
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

[[nodiscard]] domain::StateBytes encode(
    std::vector<application::SystemSettingsRecoveryRecord> const& records) {
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
  return std::move(writer).finish();
}

[[nodiscard]] bool decode(
    domain::StateBytes const& bytes,
    std::vector<application::SystemSettingsRecoveryRecord>& records) {
  Reader reader{bytes};
  std::uint32_t version{};
  std::uint32_t count{};
  if (!reader.u32(version) || (version != 1 && version != k_format_version) ||
      !reader.u32(count) || count > 100'000) {
    return false;
  }
  records.clear();
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
  if (!decode(inspected.snapshot->state.value.payload, records)) {
    return {.status = application::RecoveryStorageStatus::failed,
            .detail = "系统设置恢复记录格式无效"};
  }
  records_ = std::move(records);
  revision_ = inspected.snapshot->revision;
  loaded_ = true;
  return {.status = application::RecoveryStorageStatus::loaded,
          .records = records_};
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
                .payload = encode(next_records)}};
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
                .payload = encode(next_records)}};
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
