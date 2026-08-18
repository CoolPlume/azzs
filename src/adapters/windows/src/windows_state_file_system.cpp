#include "azzs/adapters/windows/windows_state_file_system.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <Aclapi.h>
#include <Sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace azzs::adapters::windows {
namespace {

struct HandleCloser final {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct LocalMemoryCloser final {
  void operator()(void* memory) const noexcept {
    if (memory != nullptr) {
      ::LocalFree(memory);
    }
  }
};

using SecurityDescriptor = std::unique_ptr<void, LocalMemoryCloser>;

struct GuardedStatePath final {
  std::filesystem::path path;
  PSECURITY_DESCRIPTOR security{};
  bool aggregate_missing{false};
  std::vector<UniqueHandle> directory_guards;
};

[[nodiscard]] SecurityDescriptor parse_security(std::wstring const& sddl) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
    return {};
  }
  return SecurityDescriptor{raw};
}

[[nodiscard]] bool equal_acl(PACL left, PACL right) {
  ACL_SIZE_INFORMATION left_size{};
  ACL_SIZE_INFORMATION right_size{};
  if (!::GetAclInformation(left, &left_size, sizeof(left_size),
                           AclSizeInformation) ||
      !::GetAclInformation(right, &right_size, sizeof(right_size),
                           AclSizeInformation) ||
      left_size.AceCount != right_size.AceCount) {
    return false;
  }
  for (DWORD index = 0; index < left_size.AceCount; ++index) {
    void* left_ace = nullptr;
    void* right_ace = nullptr;
    if (!::GetAce(left, index, &left_ace) ||
        !::GetAce(right, index, &right_ace)) {
      return false;
    }
    auto const* left_header = static_cast<ACE_HEADER const*>(left_ace);
    auto const* right_header = static_cast<ACE_HEADER const*>(right_ace);
    if (left_header->AceSize != right_header->AceSize ||
        std::memcmp(left_ace, right_ace, left_header->AceSize) != 0) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool security_matches(HANDLE directory,
                                    PSECURITY_DESCRIPTOR expected) {
  PSID expected_owner = nullptr;
  PACL expected_dacl = nullptr;
  BOOL owner_defaulted = FALSE;
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  if (!::GetSecurityDescriptorOwner(expected, &expected_owner,
                                    &owner_defaulted) ||
      expected_owner == nullptr ||
      !::GetSecurityDescriptorDacl(expected, &dacl_present, &expected_dacl,
                                   &dacl_defaulted) ||
      !dacl_present || expected_dacl == nullptr) {
    return false;
  }

  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  auto const status = ::GetSecurityInfo(
      directory, SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &raw);
  SecurityDescriptor actual{raw};
  if (status != ERROR_SUCCESS || raw == nullptr || owner == nullptr ||
      dacl == nullptr) {
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  return ::GetSecurityDescriptorControl(raw, &control, &revision) &&
         (control & SE_DACL_PROTECTED) != 0 &&
         ::EqualSid(owner, expected_owner) && equal_acl(dacl, expected_dacl);
}

[[nodiscard]] UniqueHandle open_plain_directory(
    std::filesystem::path const& path,
    PSECURITY_DESCRIPTOR expected_security) {
  UniqueHandle directory{::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (directory.get() == INVALID_HANDLE_VALUE) {
    return {};
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
    return {};
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    ::SetLastError(ERROR_REPARSE_TAG_INVALID);
    return {};
  }
  if (!security_matches(directory.get(), expected_security)) {
    ::SetLastError(ERROR_INVALID_SECURITY_DESCR);
    return {};
  }
  return directory;
}

[[nodiscard]] bool plain_state_file(HANDLE file) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  return ::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                        sizeof(attributes)) &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

class WindowsStateFileLock final : public application::StateFileLock {
 public:
  explicit WindowsStateFileLock(UniqueHandle handle) noexcept
      : handle_(std::move(handle)) {}

 private:
  UniqueHandle handle_;
};

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(
    std::string const& value) {
  if (value.empty()) {
    return std::wstring{};
  }
  auto const count = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) {
    return std::nullopt;
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            count) != count) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::string windows_error(DWORD code) {
  return "win32:" + std::to_string(code);
}

[[nodiscard]] application::StateIoResult io_failure(DWORD code) {
  return {.status = application::StateIoStatus::failed,
          .error = windows_error(code)};
}

[[nodiscard]] std::wstring_view slot_name(
    application::StateFileSlot slot) noexcept {
  using application::StateFileSlot;
  switch (slot) {
    case StateFileSlot::current:
      return L"authority.state";
    case StateFileSlot::previous:
      return L"previous.state";
    case StateFileSlot::candidate:
      return L"candidate.transaction";
    case StateFileSlot::previous_staging:
      return L"previous.transaction";
    case StateFileSlot::intent:
      return L"commit.intent";
    case StateFileSlot::checkpoint:
      return L"checkpoint.state";
    case StateFileSlot::checkpoint_staging:
      return L"checkpoint.transaction";
    case StateFileSlot::checkpoint_consumed:
      return L"checkpoint.consumed";
    case StateFileSlot::checkpoint_consumed_staging:
      return L"checkpoint-consumed.transaction";
    case StateFileSlot::corrupt_archive:
      return L"corrupt-evidence.archive";
    case StateFileSlot::corrupt_archive_staging:
      return L"corrupt-evidence.transaction";
    case StateFileSlot::corrupt_current:
      return L"corrupt-authority.state";
    case StateFileSlot::corrupt_previous:
      return L"corrupt-previous.state";
    case StateFileSlot::corrupt_candidate:
      return L"corrupt-candidate.transaction";
    case StateFileSlot::corrupt_intent:
      return L"corrupt-commit.intent";
    case StateFileSlot::corrupt_previous_staging:
      return L"corrupt-previous.transaction";
  }
  return L"invalid";
}

[[nodiscard]] std::uint64_t fnv1a(std::string_view value) noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
  for (unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] std::wstring hex(std::uint64_t value) {
  constexpr std::array digits{L'0', L'1', L'2', L'3', L'4', L'5', L'6',
                              L'7', L'8', L'9', L'a', L'b', L'c', L'd',
                              L'e', L'f'};
  std::wstring result(16, L'0');
  for (std::size_t index = 0; index < result.size(); ++index) {
    auto const shift = (result.size() - index - 1) * 4;
    result[index] = digits[(value >> shift) & 0x0fU];
  }
  return result;
}

}  // namespace

class WindowsStateFileSystem::Impl final {
 public:
  explicit Impl(DeviceDataEnvironment environment)
      : subject_(std::move(environment.subject_id)) {
    auto root = utf8_to_wide(environment.root_utf8);
    auto subject = utf8_to_wide(subject_);
    if (root.has_value() && subject.has_value()) {
      root_ = std::filesystem::path{*root};
      machine_directory_security_ = parse_security(
          L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
      subject_directory_security_ = parse_security(
          L"O:" + *subject +
          L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + *subject + L")");
      machine_file_security_ =
          parse_security(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)");
      subject_file_security_ = parse_security(
          L"O:" + *subject +
          L"D:P(A;;FA;;;SY)(A;;FA;;;" + *subject + L")");
      valid_ = machine_directory_security_.get() != nullptr &&
               subject_directory_security_.get() != nullptr &&
               machine_file_security_.get() != nullptr &&
               subject_file_security_.get() != nullptr;
    }
  }

  [[nodiscard]] std::optional<std::filesystem::path> aggregate_directory(
      domain::StateKey const& key) const {
    if (!valid_ || !key.valid()) {
      return std::nullopt;
    }
    auto aggregate = utf8_to_wide(key.aggregate.value);
    if (!aggregate.has_value()) {
      return std::nullopt;
    }
    if (key.partition == domain::StatePartition::device) {
      return root_ / L"device" / L"state" / *aggregate;
    }
    if (!key.subject.has_value() || key.subject->value != subject_) {
      return std::nullopt;
    }
    auto subject = utf8_to_wide(subject_);
    if (!subject.has_value()) {
      return std::nullopt;
    }
    return root_ / L"subjects" / *subject / L"state" / *aggregate;
  }

  [[nodiscard]] std::optional<GuardedStatePath> file(
      domain::StateKey const& key,
      application::StateFileSlot slot,
      bool create_directory) const {
    auto directory = aggregate_directory(key);
    if (!directory.has_value()) {
      return std::nullopt;
    }

    using SecuredPath =
        std::pair<std::filesystem::path, PSECURITY_DESCRIPTOR>;
    std::vector<SecuredPath> chain{
        SecuredPath{root_, machine_directory_security_.get()}};
    auto aggregate_security = machine_directory_security_.get();
    auto file_security = machine_file_security_.get();
    if (key.partition == domain::StatePartition::device) {
      chain.emplace_back(root_ / L"device",
                         machine_directory_security_.get());
      chain.emplace_back(root_ / L"device" / L"state",
                         machine_directory_security_.get());
    } else {
      auto subject = utf8_to_wide(subject_);
      if (!subject.has_value()) {
        return std::nullopt;
      }
      chain.emplace_back(root_ / L"subjects",
                         machine_directory_security_.get());
      chain.emplace_back(root_ / L"subjects" / *subject,
                         subject_directory_security_.get());
      chain.emplace_back(root_ / L"subjects" / *subject / L"state",
                         subject_directory_security_.get());
      aggregate_security = subject_directory_security_.get();
      file_security = subject_file_security_.get();
    }

    GuardedStatePath guarded{.path = *directory / slot_name(slot),
                             .security = file_security};
    guarded.directory_guards.reserve(chain.size() + 1);
    for (auto const& [component, security] : chain) {
      auto handle = open_plain_directory(component, security);
      if (handle.get() == nullptr) {
        return std::nullopt;
      }
      guarded.directory_guards.push_back(std::move(handle));
    }

    SECURITY_ATTRIBUTES attributes{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = aggregate_security,
        .bInheritHandle = FALSE,
    };
    if (create_directory &&
        !::CreateDirectoryW(directory->c_str(), &attributes)) {
      auto const error = ::GetLastError();
      if (error != ERROR_ALREADY_EXISTS) {
        return std::nullopt;
      }
    }
    auto aggregate = open_plain_directory(*directory, aggregate_security);
    if (aggregate.get() == nullptr) {
      auto const error = ::GetLastError();
      if (!create_directory &&
          (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
        guarded.aggregate_missing = true;
        return guarded;
      }
      return std::nullopt;
    }
    guarded.directory_guards.push_back(std::move(aggregate));
    return guarded;
  }

  [[nodiscard]] std::optional<GuardedStatePath> guarded_lock_file(
      domain::StateKey const& key) const {
    if (!aggregate_directory(key).has_value()) {
      return std::nullopt;
    }
    GuardedStatePath guarded{.path = lock_file(key),
                             .security = machine_file_security_.get()};
    for (auto const& component : {root_, root_ / L"locks"}) {
      auto handle = open_plain_directory(
          component, machine_directory_security_.get());
      if (handle.get() == nullptr) {
        return std::nullopt;
      }
      guarded.directory_guards.push_back(std::move(handle));
    }
    return guarded;
  }

  [[nodiscard]] std::filesystem::path lock_file(
      domain::StateKey const& key) const {
    auto identity = std::to_string(static_cast<unsigned>(key.partition)) + ":" +
                    key.aggregate.value + ":" +
                    (key.subject.has_value() ? key.subject->value : "device");
    return root_ / L"locks" / (L"state-" + hex(fnv1a(identity)) + L".lock");
  }

 private:
  friend class WindowsStateFileSystem;
  std::filesystem::path root_;
  std::string subject_;
  SecurityDescriptor machine_directory_security_;
  SecurityDescriptor subject_directory_security_;
  SecurityDescriptor machine_file_security_;
  SecurityDescriptor subject_file_security_;
  bool valid_{false};
};

WindowsStateFileSystem::WindowsStateFileSystem(
    DeviceDataEnvironment environment)
    : impl_(std::make_unique<Impl>(std::move(environment))) {}

WindowsStateFileSystem::~WindowsStateFileSystem() = default;
WindowsStateFileSystem::WindowsStateFileSystem(
    WindowsStateFileSystem&&) noexcept = default;
WindowsStateFileSystem& WindowsStateFileSystem::operator=(
    WindowsStateFileSystem&&) noexcept = default;

application::StateFileLockResult WindowsStateFileSystem::try_lock(
    domain::StateKey const& key) {
  auto path = impl_->guarded_lock_file(key);
  if (!path.has_value()) {
    return {.status = application::StateFileLockStatus::failed,
            .error = "invalid, missing, or unsafe state lock path"};
  }
  SECURITY_ATTRIBUTES lock_security{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = path->security,
      .bInheritHandle = FALSE,
  };
  auto raw = ::CreateFileW(path->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                           &lock_security, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    auto const error = ::GetLastError();
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return {.status = application::StateFileLockStatus::busy};
    }
    return {.status = application::StateFileLockStatus::failed,
            .error = windows_error(error)};
  }
  UniqueHandle lock_handle{raw};
  if (!plain_state_file(lock_handle.get()) ||
      !security_matches(lock_handle.get(), path->security)) {
    return {.status = application::StateFileLockStatus::failed,
            .error = "state lock path is a directory or reparse point"};
  }
  return {.status = application::StateFileLockStatus::acquired,
          .lock =
              std::make_unique<WindowsStateFileLock>(std::move(lock_handle))};
}

application::StateFileRead WindowsStateFileSystem::read(
    domain::StateKey const& key,
    application::StateFileSlot slot) {
  auto path = impl_->file(key, slot, false);
  if (!path.has_value()) {
    return {.status = application::StateIoStatus::failed,
            .error = "invalid, missing, or unsafe state storage path"};
  }
  if (path->aggregate_missing) {
    return {.status = application::StateIoStatus::not_found};
  }
  UniqueHandle file{::CreateFileW(
      path->path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  if (file.get() == INVALID_HANDLE_VALUE) {
    auto const error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return {.status = application::StateIoStatus::not_found};
    }
    return {.status = application::StateIoStatus::failed,
            .error = windows_error(error)};
  }
  if (!plain_state_file(file.get()) ||
      !security_matches(file.get(), path->security)) {
    return {.status = application::StateIoStatus::failed,
            .error = "state file is a directory or reparse point"};
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) >
          std::numeric_limits<std::size_t>::max()) {
    return {.status = application::StateIoStatus::failed,
            .error = windows_error(::GetLastError())};
  }
  domain::StateBytes bytes(static_cast<std::size_t>(size.QuadPart));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const remaining = bytes.size() - offset;
    auto const chunk = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD read_count = 0;
    if (!::ReadFile(file.get(), bytes.data() + offset, chunk, &read_count,
                    nullptr) ||
        read_count == 0) {
      return {.status = application::StateIoStatus::failed,
              .error = windows_error(::GetLastError())};
    }
    offset += read_count;
  }
  return {.status = application::StateIoStatus::succeeded,
          .bytes = std::move(bytes)};
}

application::StateIoResult WindowsStateFileSystem::write(
    domain::StateKey const& key,
    application::StateFileSlot slot,
    std::span<std::byte const> bytes) {
  auto path = impl_->file(key, slot, true);
  if (!path.has_value()) {
    return {.status = application::StateIoStatus::failed,
            .error = "invalid, missing, or unsafe state storage path"};
  }
  SECURITY_ATTRIBUTES file_security{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = path->security,
      .bInheritHandle = FALSE,
  };
  UniqueHandle file{::CreateFileW(path->path.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ, &file_security, OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr)};
  if (file.get() == INVALID_HANDLE_VALUE) {
    return io_failure(::GetLastError());
  }
  if (!plain_state_file(file.get()) ||
      !security_matches(file.get(), path->security)) {
    return {.status = application::StateIoStatus::failed,
            .error = "state file is a directory or reparse point"};
  }
  LARGE_INTEGER beginning{};
  if (!::SetFilePointerEx(file.get(), beginning, nullptr, FILE_BEGIN) ||
      !::SetEndOfFile(file.get())) {
    return io_failure(::GetLastError());
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const remaining = bytes.size() - offset;
    auto const chunk = static_cast<DWORD>(std::min<std::size_t>(
        remaining, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!::WriteFile(file.get(), bytes.data() + offset, chunk, &written,
                     nullptr) ||
        written == 0) {
      return io_failure(::GetLastError());
    }
    offset += written;
  }
  return {};
}

application::StateIoResult WindowsStateFileSystem::flush(
    domain::StateKey const& key,
    application::StateFileSlot slot) {
  auto path = impl_->file(key, slot, false);
  if (!path.has_value()) {
    return {.status = application::StateIoStatus::failed,
            .error = "invalid, missing, or unsafe state storage path"};
  }
  if (path->aggregate_missing) {
    return io_failure(ERROR_PATH_NOT_FOUND);
  }
  UniqueHandle file{::CreateFileW(path->path.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL |
                                      FILE_FLAG_OPEN_REPARSE_POINT,
                                  nullptr)};
  if (file.get() == INVALID_HANDLE_VALUE) {
    return io_failure(::GetLastError());
  }
  if (!plain_state_file(file.get()) ||
      !security_matches(file.get(), path->security)) {
    return {.status = application::StateIoStatus::failed,
            .error = "state file security or type is invalid"};
  }
  if (!::FlushFileBuffers(file.get())) {
    return io_failure(::GetLastError());
  }
  return {};
}

application::StateIoResult WindowsStateFileSystem::replace(
    domain::StateKey const& key,
    application::StateFileSlot source,
    application::StateFileSlot target) {
  auto source_path = impl_->file(key, source, false);
  auto target_path = impl_->file(key, target, true);
  if (!source_path.has_value() || !target_path.has_value() ||
      source_path->aggregate_missing) {
    return {.status = application::StateIoStatus::failed,
            .error = "invalid, missing, or unsafe state storage path"};
  }
  UniqueHandle source_file{::CreateFileW(
      source_path->path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  if (source_file.get() == INVALID_HANDLE_VALUE ||
      !plain_state_file(source_file.get()) ||
      !security_matches(source_file.get(), source_path->security)) {
    return {.status = application::StateIoStatus::failed,
            .error = "state replacement source security or type is invalid"};
  }
  UniqueHandle target_file{::CreateFileW(
      target_path->path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  if (target_file.get() == INVALID_HANDLE_VALUE) {
    auto const error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      return io_failure(error);
    }
  } else if (!plain_state_file(target_file.get()) ||
             !security_matches(target_file.get(), target_path->security)) {
    return {.status = application::StateIoStatus::failed,
            .error = "state replacement target security or type is invalid"};
  }
  // MoveFileExW cannot replace a target while its validation handle is open.
  target_file.reset();
  if (!::MoveFileExW(source_path->path.c_str(), target_path->path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return io_failure(::GetLastError());
  }
  return {};
}

application::StateIoResult WindowsStateFileSystem::remove(
    domain::StateKey const& key,
    application::StateFileSlot slot) {
  auto path = impl_->file(key, slot, false);
  if (!path.has_value()) {
    return {.status = application::StateIoStatus::failed,
            .error = "invalid, missing, or unsafe state storage path"};
  }
  if (path->aggregate_missing) {
    return {};
  }
  UniqueHandle file{::CreateFileW(
      path->path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  if (file.get() == INVALID_HANDLE_VALUE) {
    auto const error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return {};
    }
    return io_failure(error);
  }
  if (!plain_state_file(file.get()) ||
      !security_matches(file.get(), path->security)) {
    return {.status = application::StateIoStatus::failed,
            .error = "state file is a directory or reparse point"};
  }
  file.reset();
  if (!::DeleteFileW(path->path.c_str())) {
    return io_failure(::GetLastError());
  }
  return {};
}

application::StateIoResult WindowsStateFileSystem::flush_volume(
    domain::StateKey const& key) {
  auto const path = impl_->file(key, application::StateFileSlot::current,
                                false);
  if (!path.has_value()) {
    return {.status = application::StateIoStatus::failed,
            .error = "invalid, missing, or unsafe state storage path"};
  }
  if (path->aggregate_missing) {
    return io_failure(ERROR_PATH_NOT_FOUND);
  }
  // Windows has no portable directory fsync. Every state file is flushed before
  // replacement and MoveFileExW uses MOVEFILE_WRITE_THROUGH, which is the
  // platform durability barrier for the same-volume rename performed above.
  return {};
}

}  // namespace azzs::adapters::windows
