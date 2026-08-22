#include "azzs/adapters/infrastructure/local_package_cache_storage.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace azzs::adapters::infrastructure {
namespace {

namespace cache = application::offline_package_cache;
namespace cache_domain = domain::offline_package_cache;

constexpr std::string_view kPackagesDirectory{"packages"};
constexpr std::string_view kPartialSuffix{".partial"};
constexpr std::string_view kPayloadSuffix{".payload"};
constexpr std::string_view kMarkerSuffix{".complete"};
constexpr std::string_view kMarkerTemporarySuffix{".complete.tmp"};
constexpr std::string_view kLockSuffix{".lock"};
constexpr std::string_view kMarkerMagic{"AZZSPKG1"};
constexpr std::size_t kStableKeyLength = 16;
constexpr std::uint64_t kMaximumPayloadReadBytes =
    2ULL * 1024ULL * 1024ULL * 1024ULL;

[[nodiscard]] bool stable_key(std::string_view key) noexcept {
  return key.size() == kStableKeyLength &&
         std::ranges::all_of(key, [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] std::filesystem::path path_for(
    std::filesystem::path const& directory, std::string_view key,
    std::string_view suffix) {
  return directory / std::string{kPackagesDirectory} /
         (std::string{key} + std::string{suffix});
}

[[nodiscard]] std::filesystem::path package_directory(
    std::filesystem::path const& directory) {
  return directory / std::string{kPackagesDirectory};
}

[[nodiscard]] bool path_is_directory(std::filesystem::path const& path,
                                     std::error_code& error) {
  auto const status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status)) {
    return false;
  }
  return std::filesystem::is_directory(status);
}

[[nodiscard]] std::optional<std::string> file_name_key(
    std::filesystem::path const& path, std::string_view suffix) {
  auto const name = path.filename().string();
  if (!name.ends_with(suffix)) {
    return std::nullopt;
  }
  auto const key = std::string_view{name}.substr(0, name.size() - suffix.size());
  if (!stable_key(key)) {
    return std::nullopt;
  }
  return std::string{key};
}

[[nodiscard]] bool parse_signed(std::string_view text, std::int64_t& value) {
  auto const parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t& value) {
  auto const parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

[[nodiscard]] std::optional<cache_domain::CacheArchitecture> architecture(
    std::string_view value) noexcept {
  if (value == "x64") {
    return cache_domain::CacheArchitecture::x64;
  }
  if (value == "arm64") {
    return cache_domain::CacheArchitecture::arm64;
  }
  if (value == "independent") {
    return cache_domain::CacheArchitecture::architecture_independent;
  }
  if (value == "x86") {
    return cache_domain::CacheArchitecture::x86;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<cache::CompletedCacheEntry> read_marker(
    std::filesystem::path const& marker) {
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(marker, status_error);
  if (status_error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return std::nullopt;
  }
  std::ifstream input(marker, std::ios::binary);
  if (!input) {
    return std::nullopt;
  }
  std::array<std::string, 7> lines;
  for (auto& line : lines) {
    if (!std::getline(input, line)) {
      return std::nullopt;
    }
  }
  std::string extra;
  if (std::getline(input, extra) || lines[0] != kMarkerMagic) {
    return std::nullopt;
  }
  auto const parsed_architecture = architecture(lines[3]);
  std::uint64_t byte_count{};
  std::int64_t completed_at{};
  if (!parsed_architecture.has_value() || !parse_unsigned(lines[5], byte_count) ||
      !parse_signed(lines[6], completed_at)) {
    return std::nullopt;
  }
  cache::CompletedCacheEntry entry{
      .identity = {.software_id = std::move(lines[1]),
                   .version = std::move(lines[2]),
                   .architecture = *parsed_architecture,
                   .source_identity = std::move(lines[4])},
      .byte_count = byte_count,
      .completed_at = application::WallClockTime{
          std::chrono::milliseconds{completed_at}},
  };
  return entry.identity.valid() ? std::optional{std::move(entry)}
                                : std::nullopt;
}

[[nodiscard]] bool write_marker(std::filesystem::path const& marker,
                                cache::CompletedCacheEntry const& entry) {
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(marker, status_error);
  if ((status_error && status_error != std::errc::no_such_file_or_directory) ||
      (std::filesystem::exists(status) &&
       (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)))) {
    return false;
  }
  std::ofstream output(marker, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output << kMarkerMagic << '\n'
         << entry.identity.software_id << '\n'
         << entry.identity.version << '\n'
         << cache_domain::to_string(entry.identity.architecture) << '\n'
         << entry.identity.source_identity << '\n'
         << entry.byte_count << '\n'
         << entry.completed_at.time_since_epoch().count() << '\n';
  output.flush();
  return static_cast<bool>(output);
}

class AssetLock final {
 public:
  AssetLock() = default;
  ~AssetLock() { release(); }

  AssetLock(AssetLock const&) = delete;
  AssetLock& operator=(AssetLock const&) = delete;
  AssetLock(AssetLock&& other) noexcept { *this = std::move(other); }
  AssetLock& operator=(AssetLock&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    release();
#ifdef _WIN32
    handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
#else
    descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    return *this;
  }

  [[nodiscard]] static std::optional<AssetLock> try_acquire(
      std::filesystem::path const& path, bool& busy, std::string& detail) {
    busy = false;
    detail.clear();
#ifdef _WIN32
    HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_HIDDEN, nullptr);
    auto const create_error = ::GetLastError();
    auto const created = handle != INVALID_HANDLE_VALUE;
    if (handle == INVALID_HANDLE_VALUE &&
        (create_error == ERROR_FILE_EXISTS ||
         create_error == ERROR_ALREADY_EXISTS)) {
      handle = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_HIDDEN |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
                             nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        auto const open_error = ::GetLastError();
        busy = open_error == ERROR_SHARING_VIOLATION ||
               open_error == ERROR_LOCK_VIOLATION;
        detail = "controlled cache lock open failed (" +
                 std::to_string(open_error) + ")";
        return std::nullopt;
      }
    } else if (handle == INVALID_HANDLE_VALUE) {
      detail = "controlled cache lock create failed (" +
               std::to_string(create_error) + ")";
      return std::nullopt;
    }
    if (created) {
      return AssetLock{handle};
    }
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!::GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
      detail = "controlled cache lock file is unsafe";
      ::CloseHandle(handle);
      return std::nullopt;
    }
    return AssetLock{handle};
#else
    auto const descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW,
                                   S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      detail = "controlled cache lock open failed (" +
               std::to_string(errno) + ")";
      return std::nullopt;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      auto const lock_error = errno;
      busy = lock_error == EACCES || lock_error == EAGAIN;
      detail = "controlled cache lock acquire failed (" +
               std::to_string(lock_error) + ")";
      if (descriptor >= 0) {
        ::close(descriptor);
      }
      return std::nullopt;
    }
    return AssetLock{descriptor};
#endif
  }

  [[nodiscard]] bool acquired() const noexcept {
#ifdef _WIN32
    return handle_ != INVALID_HANDLE_VALUE;
#else
    return descriptor_ >= 0;
#endif
  }

 private:
#ifdef _WIN32
  explicit AssetLock(HANDLE handle) noexcept : handle_(handle) {}
  void release() noexcept {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }
  HANDLE handle_{INVALID_HANDLE_VALUE};
#else
  explicit AssetLock(int descriptor) noexcept : descriptor_(descriptor) {}
  void release() noexcept {
    if (descriptor_ >= 0) {
      ::flock(descriptor_, LOCK_UN);
      ::close(descriptor_);
      descriptor_ = -1;
    }
  }
  int descriptor_{-1};
#endif
};

[[nodiscard]] bool remove_file(std::filesystem::path const& path,
                               std::error_code& error) {
  error.clear();
  auto const status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    return true;
  }
  if (error) {
    return false;
  }
  if (std::filesystem::is_directory(status)) {
    error = std::make_error_code(std::errc::is_a_directory);
    return false;
  }
  if (std::filesystem::is_symlink(status)) {
    error = std::make_error_code(std::errc::operation_not_permitted);
    return false;
  }
  if (status.type() == std::filesystem::file_type::not_found) {
    return true;
  }
  auto const removed = std::filesystem::remove(path, error);
  return !error && (removed || !std::filesystem::exists(path, error));
}

[[nodiscard]] bool completed_pair_is_valid(
    std::filesystem::path const& directory, std::string_view key) {
  auto entry = read_marker(path_for(directory, key, kMarkerSuffix));
  if (!entry.has_value()) {
    return false;
  }
  std::error_code error;
  auto const payload = path_for(directory, key, kPayloadSuffix);
  auto const status = std::filesystem::symlink_status(payload, error);
  return !error && std::filesystem::is_regular_file(status) &&
         std::filesystem::file_size(payload, error) == entry->byte_count &&
         !error;
}

}  // namespace

class LocalPackageCacheStorage::Impl final {
 public:
  explicit Impl(std::vector<ControlledPackageCacheRootConfiguration> roots) {
    for (auto& root : roots) {
      if (!root.root.valid() || root.directory.empty()) {
        continue;
      }
      roots_.insert_or_assign(root.root.id, std::move(root));
    }
  }

  [[nodiscard]] ControlledPackageCacheRootConfiguration const* configuration(
      cache::ControlledCacheRoot const& root) const noexcept {
    if (!root.valid()) {
      return nullptr;
    }
    auto const found = roots_.find(root.id);
    if (found == roots_.end() || found->second.root != root) {
      return nullptr;
    }
    return std::addressof(found->second);
  }

  [[nodiscard]] std::optional<std::filesystem::path> usable_directory(
      cache::ControlledCacheRoot const& root, std::string& detail) const {
    auto const* configuration_value = configuration(root);
    if (configuration_value == nullptr) {
      detail = "cache root is not registered by the host";
      return std::nullopt;
    }
    std::error_code error;
    auto const& directory = configuration_value->directory;
    auto exists = std::filesystem::exists(directory, error);
    if (!exists && !error && configuration_value->create_if_missing) {
      std::filesystem::create_directories(directory, error);
      exists = !error && std::filesystem::exists(directory, error);
    }
    if (error || !exists || !path_is_directory(directory, error)) {
      detail = "controlled cache root is unavailable or unsafe";
      return std::nullopt;
    }
    auto packages = package_directory(directory);
    if (!std::filesystem::exists(packages, error) && !error) {
      std::filesystem::create_directories(packages, error);
    }
    if (error || !path_is_directory(packages, error)) {
      detail = "controlled cache package directory is unavailable or unsafe";
      return std::nullopt;
    }
    return directory;
  }

  [[nodiscard]] cache::CacheRootObservation observe(
      cache::ControlledCacheRoot const& root) const {
    std::string detail;
    auto directory = usable_directory(root, detail);
    if (!directory.has_value()) {
      return {.detail = std::move(detail)};
    }
    std::error_code error;
    auto const information = std::filesystem::space(*directory, error);
    return {.available = true,
            .free_bytes = error ? std::nullopt
                                : std::optional{information.available},
            .detail = error ? "controlled cache root free space is unavailable"
                            : std::string{}};
  }

 private:
  std::map<std::string, ControlledPackageCacheRootConfiguration, std::less<>>
      roots_;
};

class LocalWriteSession final : public cache::PackageCacheWriteSession {
 public:
  LocalWriteSession(std::filesystem::path directory, cache::CacheAsset asset,
                    AssetLock lock)
      : directory_(std::move(directory)),
        asset_(std::move(asset)),
        lock_(std::move(lock)) {}

  ~LocalWriteSession() override { static_cast<void>(abandon()); }

  [[nodiscard]] std::uint64_t received_bytes() const noexcept override {
    std::error_code error;
    auto const size = std::filesystem::file_size(
        path_for(directory_, asset_.identity.stable_key(), kPartialSuffix), error);
    return error ? 0 : size;
  }

  [[nodiscard]] cache::CacheWriteResult append(
      std::span<std::byte const> bytes) override {
    if (closed_ || !lock_.acquired()) {
      return {.detail = "controlled cache write session is closed"};
    }
    auto const partial =
        path_for(directory_, asset_.identity.stable_key(), kPartialSuffix);
    std::error_code status_error;
    auto const status = std::filesystem::symlink_status(partial, status_error);
    if ((status_error && status_error != std::errc::no_such_file_or_directory) ||
        (std::filesystem::exists(status) &&
         (std::filesystem::is_symlink(status) ||
          !std::filesystem::is_regular_file(status)))) {
      return {.detail = "controlled cache temporary file is unsafe"};
    }
    std::ofstream output(partial, std::ios::binary | std::ios::app);
    if (!output) {
      return {.detail = "controlled cache temporary file cannot be written"};
    }
    output.write(reinterpret_cast<char const*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output ? cache::CacheWriteResult{.code = cache::CacheWriteCode::succeeded}
                  : cache::CacheWriteResult{.detail =
                                                 "controlled cache temporary file write failed"};
  }

  [[nodiscard]] cache::CacheWriteResult complete(
      application::WallClockTime completed_at) override {
    if (closed_ || !lock_.acquired()) {
      return {.detail = "controlled cache write session is closed"};
    }
    auto const key = asset_.identity.stable_key();
    auto const partial = path_for(directory_, key, kPartialSuffix);
    auto const payload = path_for(directory_, key, kPayloadSuffix);
    auto const marker_temporary =
        path_for(directory_, key, kMarkerTemporarySuffix);
    auto const marker = path_for(directory_, key, kMarkerSuffix);
    std::error_code error;
    auto const partial_status = std::filesystem::symlink_status(partial, error);
    if (error || std::filesystem::is_symlink(partial_status) ||
        !std::filesystem::is_regular_file(partial_status)) {
      return {.detail = "controlled cache temporary file is absent"};
    }
    auto const size = std::filesystem::file_size(partial, error);
    if (error) {
      return {.detail = "controlled cache temporary file size is unavailable"};
    }
    if (!remove_file(payload, error)) {
      return {.detail = "stale controlled cache payload cannot be removed"};
    }
    std::filesystem::rename(partial, payload, error);
    if (error) {
      return {.detail = "controlled cache payload could not be finalized"};
    }
    cache::CompletedCacheEntry entry{
        .identity = asset_.identity,
        .byte_count = size,
        .completed_at = completed_at,
    };
    if (!write_marker(marker_temporary, entry)) {
      return {.detail = "controlled cache completion marker could not be written"};
    }
    if (remove_file(marker, error)) {
      std::filesystem::rename(marker_temporary, marker, error);
    }
    if (error) {
      return {.detail = "controlled cache completion marker could not be finalized"};
    }
    closed_ = true;
    return {.code = cache::CacheWriteCode::succeeded};
  }

  [[nodiscard]] cache::CacheWriteResult abandon() override {
    if (closed_) {
      return {.code = cache::CacheWriteCode::succeeded};
    }
    std::error_code error;
    static_cast<void>(remove_file(
        path_for(directory_, asset_.identity.stable_key(), kPartialSuffix), error));
    if (!error) {
      static_cast<void>(remove_file(
          path_for(directory_, asset_.identity.stable_key(),
                  kMarkerTemporarySuffix),
          error));
    }
    if (!error) {
      static_cast<void>(remove_file(
          path_for(directory_, asset_.identity.stable_key(), kPayloadSuffix),
          error));
    }
    closed_ = true;
    return error ? cache::CacheWriteResult{
                       .detail = "controlled cache temporary file cleanup failed"}
                 : cache::CacheWriteResult{.code = cache::CacheWriteCode::succeeded};
  }

 private:
  std::filesystem::path directory_;
  cache::CacheAsset asset_;
  AssetLock lock_;
  bool closed_{false};
};

LocalPackageCacheStorage::LocalPackageCacheStorage(
    std::vector<ControlledPackageCacheRootConfiguration> roots)
    : impl_(std::make_unique<Impl>(std::move(roots))) {}

LocalPackageCacheStorage::~LocalPackageCacheStorage() = default;
LocalPackageCacheStorage::LocalPackageCacheStorage(
    LocalPackageCacheStorage&&) noexcept = default;
LocalPackageCacheStorage& LocalPackageCacheStorage::operator=(
    LocalPackageCacheStorage&&) noexcept = default;

cache::CacheRootObservation LocalPackageCacheStorage::observe_root(
    cache::ControlledCacheRoot const& root) {
  return impl_->observe(root);
}

cache::CompletedCacheRead LocalPackageCacheStorage::read_completed(
    cache::ControlledCacheRoot const& root,
    cache::CacheAssetIdentity const& identity) {
  auto observation = impl_->observe(root);
  if (!observation.available) {
    return {.code = cache::CompletedCacheReadCode::root_unavailable,
            .detail = std::move(observation.detail)};
  }
  if (!identity.valid()) {
    return {.code = cache::CompletedCacheReadCode::absent};
  }
  auto const* configuration = impl_->configuration(root);
  if (configuration == nullptr) {
    return {.code = cache::CompletedCacheReadCode::root_unavailable,
            .detail = "cache root is not registered by the host"};
  }
  auto const marker = path_for(configuration->directory, identity.stable_key(),
                               kMarkerSuffix);
  auto entry = read_marker(marker);
  if (!entry.has_value() || entry->identity != identity) {
    return {.code = cache::CompletedCacheReadCode::absent};
  }
  std::error_code error;
  auto const payload = path_for(configuration->directory, identity.stable_key(),
                                kPayloadSuffix);
  auto const payload_status = std::filesystem::symlink_status(payload, error);
  if (std::filesystem::is_symlink(payload_status) ||
      !std::filesystem::is_regular_file(payload_status) || error ||
      std::filesystem::file_size(payload, error) != entry->byte_count || error) {
    return {.code = cache::CompletedCacheReadCode::absent};
  }
  return {.code = cache::CompletedCacheReadCode::found, .entry = std::move(entry)};
}

cache::CompletedCachePayloadRead LocalPackageCacheStorage::read_completed_payload(
    cache::ControlledCacheRoot const& root,
    cache::CacheAssetIdentity const& identity) {
  auto observation = impl_->observe(root);
  if (!observation.available) {
    return {.code = cache::CompletedCachePayloadReadCode::root_unavailable,
            .detail = std::move(observation.detail)};
  }
  if (!identity.valid()) {
    return {.code = cache::CompletedCachePayloadReadCode::absent};
  }
  auto const completed = read_completed(root, identity);
  if (completed.code == cache::CompletedCacheReadCode::root_unavailable) {
    return {.code = cache::CompletedCachePayloadReadCode::root_unavailable,
            .detail = std::move(completed.detail)};
  }
  if (completed.code != cache::CompletedCacheReadCode::found ||
      !completed.entry.has_value()) {
    return {.code = completed.code == cache::CompletedCacheReadCode::failed
                         ? cache::CompletedCachePayloadReadCode::failed
                         : cache::CompletedCachePayloadReadCode::absent,
            .detail = std::move(completed.detail)};
  }
  if (completed.entry->byte_count > kMaximumPayloadReadBytes ||
      completed.entry->byte_count >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return {.code = cache::CompletedCachePayloadReadCode::failed,
            .detail = "controlled cache payload exceeds the read limit"};
  }
  auto const* configuration = impl_->configuration(root);
  if (configuration == nullptr) {
    return {.code = cache::CompletedCachePayloadReadCode::root_unavailable,
            .detail = "cache root is not registered by the host"};
  }
  auto const payload = path_for(configuration->directory, identity.stable_key(),
                                kPayloadSuffix);
  std::error_code status_error;
  auto const status = std::filesystem::symlink_status(payload, status_error);
  if (status_error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return {.code = cache::CompletedCachePayloadReadCode::absent,
            .detail = "controlled cache payload is absent or unsafe"};
  }
  std::ifstream input(payload, std::ios::binary);
  if (!input) {
    return {.code = cache::CompletedCachePayloadReadCode::failed,
            .detail = "controlled cache payload cannot be opened"};
  }
  auto const byte_count = static_cast<std::size_t>(completed.entry->byte_count);
  std::vector<std::byte> bytes(byte_count);
  if (byte_count != 0) {
    input.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()) ||
        input.bad()) {
      return {.code = cache::CompletedCachePayloadReadCode::failed,
              .detail = "controlled cache payload read was incomplete"};
    }
  }
  char trailing_byte{};
  input.read(&trailing_byte, 1);
  if (input.gcount() != 0) {
    return {.code = cache::CompletedCachePayloadReadCode::failed,
            .detail = "controlled cache payload has trailing bytes"};
  }
  if (input.bad() || !input.eof()) {
    return {.code = cache::CompletedCachePayloadReadCode::failed,
            .detail = "controlled cache payload read failed"};
  }
  return {.code = cache::CompletedCachePayloadReadCode::found,
          .bytes = std::move(bytes)};
}

cache::CompletedCacheList LocalPackageCacheStorage::list_completed(
    cache::ControlledCacheRoot const& root) {
  auto observation = impl_->observe(root);
  if (!observation.available) {
    return {.code = cache::CompletedCacheReadCode::root_unavailable,
            .detail = std::move(observation.detail)};
  }
  auto const* configuration = impl_->configuration(root);
  if (configuration == nullptr) {
    return {.code = cache::CompletedCacheReadCode::root_unavailable,
            .detail = "cache root is not registered by the host"};
  }
  cache::CompletedCacheList result{.code = cache::CompletedCacheReadCode::found};
  std::error_code error;
  auto const directory = package_directory(configuration->directory);
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    auto const key = file_name_key(iterator->path(), kMarkerSuffix);
    if (!key.has_value()) {
      continue;
    }
    auto entry = read_marker(iterator->path());
    if (!entry.has_value() || entry->identity.stable_key() != *key) {
      continue;
    }
    auto const payload = path_for(configuration->directory, *key, kPayloadSuffix);
    std::error_code payload_error;
    if (!std::filesystem::is_regular_file(payload, payload_error) ||
        payload_error || std::filesystem::file_size(payload, payload_error) !=
                             entry->byte_count ||
        payload_error) {
      continue;
    }
    result.entries.push_back(std::move(*entry));
  }
  if (error) {
    result.code = cache::CompletedCacheReadCode::failed;
    result.entries.clear();
    result.detail = "controlled cache directory enumeration failed";
  }
  return result;
}

cache::CacheWriteBegin LocalPackageCacheStorage::begin_write(
    cache::ControlledCacheRoot const& root, cache::CacheAsset const& asset) {
  if (!asset.valid() || !asset.cacheable()) {
    return {.detail = "asset is not a cacheable controlled package"};
  }
  auto observation = impl_->observe(root);
  if (!observation.available) {
    return {.code = cache::CacheWriteBeginCode::root_unavailable,
            .detail = std::move(observation.detail)};
  }
  if (!observation.free_bytes.has_value()) {
    return {.detail = "controlled cache root free space is unavailable"};
  }
  if (asset.expected_bytes.has_value() &&
      *observation.free_bytes < *asset.expected_bytes) {
    return {.code = cache::CacheWriteBeginCode::insufficient_space,
            .detail = "controlled cache root has insufficient free space"};
  }
  auto const* configuration = impl_->configuration(root);
  if (configuration == nullptr) {
    return {.code = cache::CacheWriteBeginCode::root_unavailable,
            .detail = "cache root is not registered by the host"};
  }
  auto const key = asset.identity.stable_key();
  bool lock_busy = false;
  std::string lock_detail;
  auto lock = AssetLock::try_acquire(
      path_for(configuration->directory, key, kLockSuffix), lock_busy,
      lock_detail);
  if (!lock.has_value()) {
    return {.code = lock_busy ? cache::CacheWriteBeginCode::busy
                              : cache::CacheWriteBeginCode::failed,
            .detail = lock_detail.empty()
                          ? "controlled cache asset is locked by another session"
                          : std::move(lock_detail)};
  }
  std::error_code error;
  static_cast<void>(remove_file(
      path_for(configuration->directory, key, kPartialSuffix), error));
  if (!error) {
    static_cast<void>(remove_file(
        path_for(configuration->directory, key, kMarkerSuffix), error));
  }
  if (!error) {
    static_cast<void>(remove_file(
        path_for(configuration->directory, key, kPayloadSuffix), error));
  }
  if (!error) {
    static_cast<void>(remove_file(
        path_for(configuration->directory, key, kMarkerTemporarySuffix), error));
  }
  if (error) {
    return {.detail = "stale controlled cache temporary state cannot be cleared"};
  }
  return {.code = cache::CacheWriteBeginCode::acquired,
          .session = std::make_unique<LocalWriteSession>(
              configuration->directory, asset, std::move(*lock))};
}

cache::CacheStorageCleanupResult LocalPackageCacheStorage::clean_orphaned_partials(
    cache::ControlledCacheRoot const& root) {
  auto observation = impl_->observe(root);
  if (!observation.available) {
    return {.code = cache::CacheStorageCleanupCode::root_unavailable,
            .detail = std::move(observation.detail)};
  }
  auto const* configuration = impl_->configuration(root);
  if (configuration == nullptr) {
    return {.code = cache::CacheStorageCleanupCode::root_unavailable,
            .detail = "cache root is not registered by the host"};
  }
  std::size_t removed{};
  std::error_code error;
  auto const directory = package_directory(configuration->directory);
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    auto key = file_name_key(iterator->path(), kPartialSuffix);
    if (!key.has_value()) {
      key = file_name_key(iterator->path(), kMarkerTemporarySuffix);
    }
    if (!key.has_value()) {
      key = file_name_key(iterator->path(), kPayloadSuffix);
    }
    if (!key.has_value()) {
      continue;
    }
    bool lock_busy = false;
    std::string lock_detail;
    auto lock = AssetLock::try_acquire(
        path_for(configuration->directory, *key, kLockSuffix), lock_busy,
        lock_detail);
    if (!lock.has_value()) {
      continue;
    }
    std::error_code remove_error;
    auto const partial = path_for(configuration->directory, *key, kPartialSuffix);
    auto const marker_temporary =
        path_for(configuration->directory, *key, kMarkerTemporarySuffix);
    auto const payload = path_for(configuration->directory, *key, kPayloadSuffix);
    std::error_code exists_error;
    auto const had_orphaned_state =
        std::filesystem::exists(partial, exists_error) ||
        std::filesystem::exists(marker_temporary, exists_error) ||
        std::filesystem::exists(payload, exists_error);
    if (exists_error) {
      return {.detail = "orphaned controlled cache state inspection failed"};
    }
    if (completed_pair_is_valid(configuration->directory, *key)) {
      continue;
    }
    if (!remove_file(partial, remove_error) ||
        !remove_file(marker_temporary, remove_error) ||
        !remove_file(payload, remove_error)) {
      if (remove_error) {
        return {.detail = "orphaned controlled cache partial cleanup failed"};
      }
    }
    if (remove_error) {
      return {.detail = "orphaned controlled cache partial cleanup failed"};
    }
    if (had_orphaned_state) {
      ++removed;
    }
  }
  if (error) {
    return {.detail = "controlled cache directory enumeration failed"};
  }
  return {.code = cache::CacheStorageCleanupCode::succeeded,
          .removed_partial_count = removed};
}

cache::CacheStorageRemovalResult LocalPackageCacheStorage::remove_completed(
    cache::ControlledCacheRoot const& root,
    cache::CacheAssetIdentity const& identity) {
  auto observation = impl_->observe(root);
  if (!observation.available) {
    return {.code = cache::CacheStorageRemovalCode::root_unavailable,
            .detail = std::move(observation.detail)};
  }
  if (!identity.valid()) {
    return {.code = cache::CacheStorageRemovalCode::absent};
  }
  auto const* configuration = impl_->configuration(root);
  if (configuration == nullptr) {
    return {.code = cache::CacheStorageRemovalCode::root_unavailable,
            .detail = "cache root is not registered by the host"};
  }
  auto const key = identity.stable_key();
  bool lock_busy = false;
  std::string lock_detail;
  auto lock = AssetLock::try_acquire(
      path_for(configuration->directory, key, kLockSuffix), lock_busy,
      lock_detail);
  if (!lock.has_value()) {
    return {.code = cache::CacheStorageRemovalCode::failed,
            .detail = lock_detail.empty() ? "controlled cache asset is busy"
                                           : std::move(lock_detail)};
  }
  auto existing = read_completed(root, identity);
  if (existing.code == cache::CompletedCacheReadCode::root_unavailable) {
    return {.code = cache::CacheStorageRemovalCode::root_unavailable,
            .detail = std::move(existing.detail)};
  }
  if (existing.code != cache::CompletedCacheReadCode::found) {
    return {.code = cache::CacheStorageRemovalCode::absent};
  }
  std::error_code error;
  if (!remove_file(path_for(configuration->directory, key, kMarkerSuffix), error) ||
      !remove_file(path_for(configuration->directory, key, kPayloadSuffix), error) ||
      error) {
    return {.code = cache::CacheStorageRemovalCode::failed,
            .detail = "controlled cache completed entry cleanup failed"};
  }
  return {.code = cache::CacheStorageRemovalCode::removed};
}

cache::ControlledDownloadResult UnavailableControlledPackageDownloader::transfer(
    cache::ControlledDownloadRequest const& request) {
  if (!request.asset.valid() || !request.cache_root.valid()) {
    return {.code = cache::ControlledDownloadCode::failed,
            .detail = "controlled package download request is invalid"};
  }
  return {.code = cache::ControlledDownloadCode::network_unavailable,
          .detail = "controlled package download is not configured"};
}

}  // namespace azzs::adapters::infrastructure
