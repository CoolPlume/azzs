#include "azzs/adapters/infrastructure/local_file_log_storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <Aclapi.h>
#include <Sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace azzs::adapters::infrastructure {
namespace {

[[nodiscard]] std::filesystem::path path_from_utf8(std::string const& value) {
  auto const* begin = reinterpret_cast<char8_t const*>(value.data());
  return std::filesystem::path{std::u8string{begin, begin + value.size()}};
}

[[nodiscard]] std::string path_to_utf8(std::filesystem::path const& value) {
  auto const bytes = value.u8string();
  return {reinterpret_cast<char const*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string padded_sequence(std::uint64_t value) {
  auto result = std::to_string(value);
  if (result.size() < 6) {
    result.insert(result.begin(), 6 - result.size(), '0');
  }
  return result;
}

[[nodiscard]] bool valid_subject(std::string const& value) noexcept {
  if (value.empty() || value.size() > 184) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char byte) {
    return (byte >= 'A' && byte <= 'Z') ||
           (byte >= 'a' && byte <= 'z') ||
           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
  });
}

#ifndef _WIN32
[[nodiscard]] std::optional<std::string> read_all(
    std::filesystem::path const& path,
    std::string& error) {
  std::error_code exists_error;
  if (!std::filesystem::exists(path, exists_error)) {
    if (exists_error) {
      error = "log existence check failed: " + exists_error.message();
      return std::nullopt;
    }
    return std::string{};
  }
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    error = "log read open failed";
    return std::nullopt;
  }
  stream.seekg(0, std::ios::end);
  auto const end = stream.tellg();
  if (end < 0) {
    error = "log size query failed";
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(end), '\0');
  stream.seekg(0, std::ios::beg);
  if (!bytes.empty() &&
      !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
    error = "log read failed";
    return std::nullopt;
  }
  return bytes;
}
#endif

#ifdef _WIN32

struct HandleCloser final {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
    }
  }
};

using NativeHandle = std::unique_ptr<void, HandleCloser>;

struct LocalMemoryCloser final {
  void operator()(void* memory) const noexcept {
    if (memory != nullptr) {
      ::LocalFree(memory);
    }
  }
};

using SecurityDescriptor = std::unique_ptr<void, LocalMemoryCloser>;

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

[[nodiscard]] bool security_matches(HANDLE handle,
                                    PSECURITY_DESCRIPTOR expected) {
  PSID expected_owner = nullptr;
  PACL expected_dacl = nullptr;
  BOOL owner_defaulted = FALSE;
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  if (expected == nullptr ||
      !::GetSecurityDescriptorOwner(expected, &expected_owner,
                                    &owner_defaulted) ||
      expected_owner == nullptr ||
      !::GetSecurityDescriptorDacl(expected, &dacl_present, &expected_dacl,
                                   &dacl_defaulted) ||
      !dacl_present || expected_dacl == nullptr) {
    ::SetLastError(ERROR_INVALID_SECURITY_DESCR);
    return false;
  }

  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw = nullptr;
  auto const status = ::GetSecurityInfo(
      handle, SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &raw);
  SecurityDescriptor actual{raw};
  if (status != ERROR_SUCCESS || raw == nullptr || owner == nullptr ||
      dacl == nullptr) {
    ::SetLastError(status == ERROR_SUCCESS ? ERROR_INVALID_SECURITY_DESCR
                                           : status);
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!::GetSecurityDescriptorControl(raw, &control, &revision) ||
      (control & SE_DACL_PROTECTED) == 0 ||
      !::EqualSid(owner, expected_owner) || !equal_acl(dacl, expected_dacl)) {
    ::SetLastError(ERROR_INVALID_SECURITY_DESCR);
    return false;
  }
  return true;
}

[[nodiscard]] bool plain_secure_file(HANDLE file,
                                     PSECURITY_DESCRIPTOR expected) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes,
                                      sizeof(attributes))) {
    return false;
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    ::SetLastError(ERROR_REPARSE_TAG_INVALID);
    return false;
  }
  return security_matches(file, expected);
}

[[nodiscard]] NativeHandle open_secure_file(
    std::filesystem::path const& path,
    DWORD access,
    DWORD sharing,
    DWORD creation,
    DWORD attributes,
    PSECURITY_DESCRIPTOR security) {
  SECURITY_ATTRIBUTES security_attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = security,
      .bInheritHandle = FALSE,
  };
  NativeHandle file{::CreateFileW(
      path.c_str(), access | READ_CONTROL | FILE_READ_ATTRIBUTES, sharing,
      &security_attributes, creation,
      attributes | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (file.get() == INVALID_HANDLE_VALUE) {
    return {};
  }
  if (!plain_secure_file(file.get(), security)) {
    auto const error = ::GetLastError();
    file.reset();
    ::SetLastError(error);
    return {};
  }
  return file;
}

[[nodiscard]] NativeHandle open_secure_directory(
    std::filesystem::path const& path,
    PSECURITY_DESCRIPTOR security) {
  NativeHandle directory{::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (directory.get() == INVALID_HANDLE_VALUE) {
    return {};
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
    auto const error = ::GetLastError();
    directory.reset();
    ::SetLastError(error);
    return {};
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    directory.reset();
    ::SetLastError(ERROR_REPARSE_TAG_INVALID);
    return {};
  }
  if (!security_matches(directory.get(), security)) {
    auto const error = ::GetLastError();
    directory.reset();
    ::SetLastError(error);
    return {};
  }
  return directory;
}

[[nodiscard]] std::optional<std::string> read_all_secure(
    std::filesystem::path const& path,
    PSECURITY_DESCRIPTOR security,
    std::string& error) {
  auto file = open_secure_file(path, GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, security);
  if (file.get() == nullptr) {
    auto const code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return std::string{};
    }
    error = "log read open or security validation failed: win32:" +
            std::to_string(code);
    return std::nullopt;
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file.get(), &size)) {
    error = "log size query failed: win32:" +
            std::to_string(::GetLastError());
    return std::nullopt;
  }
  if (size.QuadPart < 0 ||
      static_cast<unsigned long long>(size.QuadPart) >
          std::numeric_limits<std::size_t>::max()) {
    error = "log size query failed: win32:" +
            std::to_string(ERROR_FILE_TOO_LARGE);
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD read = 0;
    if (!::ReadFile(file.get(), bytes.data() + offset, amount, &read, nullptr)) {
      error = "log read failed: win32:" +
              std::to_string(::GetLastError());
      return std::nullopt;
    }
    if (read == 0) {
      error = "log read failed: win32:" + std::to_string(ERROR_HANDLE_EOF);
      return std::nullopt;
    }
    offset += read;
  }
  return bytes;
}

class FileLock final {
 public:
  FileLock(std::filesystem::path const& path,
           PSECURITY_DESCRIPTOR security) {
    handle_ = open_secure_file(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN, security);
    if (handle_.get() == nullptr) {
      error_ = "log lock open or security validation failed: win32:" +
               std::to_string(::GetLastError());
      return;
    }
    OVERLAPPED overlapped{};
    if (!::LockFileEx(handle_.get(), LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD,
                      MAXDWORD, &overlapped)) {
      error_ = "log lock acquire failed: win32:" +
               std::to_string(::GetLastError());
      handle_.reset();
      return;
    }
    locked_ = true;
  }

  ~FileLock() {
    if (locked_) {
      OVERLAPPED overlapped{};
      ::UnlockFileEx(handle_.get(), 0, MAXDWORD, MAXDWORD, &overlapped);
    }
  }

  [[nodiscard]] bool acquired() const noexcept { return locked_; }
  [[nodiscard]] std::string const& error() const noexcept { return error_; }

 private:
  NativeHandle handle_;
  bool locked_{false};
  std::string error_;
};

[[nodiscard]] LogStorageWriteResult write_and_flush(
    std::filesystem::path const& path,
    std::string const& bytes,
    PSECURITY_DESCRIPTOR security) {
  auto file = open_secure_file(path, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, security);
  if (file.get() == nullptr) {
    return {.error = "log transaction open or security validation failed: win32:" +
                     std::to_string(::GetLastError())};
  }
  LARGE_INTEGER start{};
  if (!::SetFilePointerEx(file.get(), start, nullptr, FILE_BEGIN) ||
      !::SetEndOfFile(file.get())) {
    return {.error = "log transaction truncate failed: win32:" +
                     std::to_string(::GetLastError())};
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const amount = static_cast<DWORD>(std::min<std::size_t>(
        bytes.size() - offset, std::numeric_limits<DWORD>::max()));
    DWORD written = 0;
    if (!::WriteFile(file.get(), bytes.data() + offset, amount, &written,
                     nullptr)) {
      return {.error = "log transaction write failed: win32:" +
                       std::to_string(::GetLastError())};
    }
    if (written == 0) {
      return {.error = "log transaction write failed: win32:" +
                       std::to_string(ERROR_WRITE_FAULT)};
    }
    offset += written;
  }
  if (!::FlushFileBuffers(file.get())) {
    return {.error = "log transaction flush failed: win32:" +
                     std::to_string(::GetLastError())};
  }
  return {.committed = true, .verified = true};
}

[[nodiscard]] LogStorageWriteResult atomic_replace(
    std::filesystem::path const& source,
    std::filesystem::path const& target,
    PSECURITY_DESCRIPTOR security) {
  auto source_guard = open_secure_file(
      source, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, security);
  if (source_guard.get() == nullptr) {
    return {.error = "log transaction source validation failed: win32:" +
                     std::to_string(::GetLastError())};
  }
  auto target_guard = open_secure_file(
      target, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, security);
  if (target_guard.get() == nullptr) {
    auto const target_error = ::GetLastError();
    if (target_error != ERROR_FILE_NOT_FOUND &&
        target_error != ERROR_PATH_NOT_FOUND) {
      return {.error = "log transaction target validation failed: win32:" +
                       std::to_string(target_error)};
    }
  }
  // MoveFileExW cannot replace a target while its validation handle is open.
  target_guard.reset();
  if (!::MoveFileExW(source.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    return {.error = "log transaction replace failed: win32:" +
                     std::to_string(::GetLastError())};
  }
  auto published = open_secure_file(
      target, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, security);
  if (published.get() == nullptr) {
    return {.committed = true,
            .verified = false,
            .error = "log transaction publication validation failed: win32:" +
                     std::to_string(::GetLastError())};
  }
  return {.committed = true, .verified = true};
}

#else

struct DescriptorCloser final {
  void operator()(int* descriptor) const noexcept {
    if (descriptor != nullptr) {
      if (*descriptor >= 0) {
        ::close(*descriptor);
      }
      delete descriptor;
    }
  }
};

using NativeHandle = std::unique_ptr<int, DescriptorCloser>;

class FileLock final {
 public:
  explicit FileLock(std::filesystem::path const& path)
      : descriptor_(new int{::open(path.c_str(), O_CREAT | O_RDWR, 0600)}) {
    if (*descriptor_ < 0) {
      error_ = "log lock open failed: errno:" + std::to_string(errno);
      return;
    }
    if (::flock(*descriptor_, LOCK_EX) != 0) {
      error_ = "log lock acquire failed: errno:" + std::to_string(errno);
      return;
    }
    locked_ = true;
  }

  ~FileLock() {
    if (locked_) {
      ::flock(*descriptor_, LOCK_UN);
    }
  }

  [[nodiscard]] bool acquired() const noexcept { return locked_; }
  [[nodiscard]] std::string const& error() const noexcept { return error_; }

 private:
  NativeHandle descriptor_;
  bool locked_{false};
  std::string error_;
};

[[nodiscard]] LogStorageWriteResult write_and_flush(
    std::filesystem::path const& path,
    std::string const& bytes) {
  auto descriptor = ::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
  if (descriptor < 0) {
    return {.error = "log transaction open failed: errno:" +
                     std::to_string(errno)};
  }
  NativeHandle file{new int{descriptor}};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const written =
        ::write(*file, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return {.error = "log transaction write failed: errno:" +
                       std::to_string(errno)};
    }
    if (written == 0) {
      return {.error = "log transaction write made no progress"};
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(*file) != 0) {
    return {.error = "log transaction flush failed: errno:" +
                     std::to_string(errno)};
  }
  return {.committed = true, .verified = true};
}

[[nodiscard]] LogStorageWriteResult atomic_replace(
    std::filesystem::path const& source,
    std::filesystem::path const& target) {
  if (::rename(source.c_str(), target.c_str()) != 0) {
    return {.error = "log transaction replace failed: errno:" +
                     std::to_string(errno)};
  }
  auto descriptor = ::open(target.parent_path().c_str(), O_RDONLY);
  if (descriptor < 0) {
    return {.committed = true,
            .verified = false,
            .error = "log directory open failed after replace: errno:" +
                     std::to_string(errno)};
  }
  NativeHandle directory{new int{descriptor}};
  if (::fsync(*directory) != 0) {
    return {.committed = true,
            .verified = false,
            .error = "log directory flush failed after replace: errno:" +
                     std::to_string(errno)};
  }
  return {.committed = true, .verified = true};
}

#endif

}  // namespace

class LocalFileLogStorage::Impl final {
 public:
  Impl(std::string device_root_utf8, std::string subject_id)
      : subject_(std::move(subject_id)) {
    if (!device_root_utf8.empty() && valid_subject(subject_)) {
      root_ = path_from_utf8(device_root_utf8);
      log_directory_ = root_ / "subjects" / path_from_utf8(subject_) / "logs";
      export_directory_ =
          root_ / "subjects" / path_from_utf8(subject_) / "exports";
      lock_directory_ = root_ / "locks";
      log_path_ = log_directory_ / "execution.log";
      candidate_path_ = log_directory_ / "execution.transaction";
      export_candidate_path_ = export_directory_ / "diagnostic.transaction";
      lock_path_ = lock_directory_ / ("execution-log-" + subject_ + ".lock");
#ifdef _WIN32
      std::wstring const subject_wide{subject_.begin(), subject_.end()};
      machine_directory_security_ = parse_security(
          L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
      subject_directory_security_ = parse_security(
          L"O:" + subject_wide +
          L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" + subject_wide + L")");
      machine_file_security_ =
          parse_security(L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)");
      subject_file_security_ = parse_security(
          L"O:" + subject_wide + L"D:P(A;;FA;;;SY)(A;;FA;;;" +
          subject_wide + L")");
      valid_ = machine_directory_security_.get() != nullptr &&
               subject_directory_security_.get() != nullptr &&
               machine_file_security_.get() != nullptr &&
               subject_file_security_.get() != nullptr;
#else
      valid_ = true;
#endif
    }
  }

  std::filesystem::path root_;
  std::filesystem::path log_directory_;
  std::filesystem::path export_directory_;
  std::filesystem::path lock_directory_;
  std::filesystem::path log_path_;
  std::filesystem::path candidate_path_;
  std::filesystem::path export_candidate_path_;
  std::filesystem::path lock_path_;
  std::string subject_;
#ifdef _WIN32
  SecurityDescriptor machine_directory_security_;
  SecurityDescriptor subject_directory_security_;
  SecurityDescriptor machine_file_security_;
  SecurityDescriptor subject_file_security_;
#endif
  bool valid_{false};
};

namespace {

class LocalFileLogTransaction final : public LogStorageTransaction {
 public:
  explicit LocalFileLogTransaction(LocalFileLogStorage::Impl& owner)
      : owner_(owner) {
    if (!owner_.valid_) {
      error_ = "invalid local log storage root or subject";
      return;
    }
#ifdef _WIN32
    using SecuredDirectory =
        std::pair<std::filesystem::path, PSECURITY_DESCRIPTOR>;
    std::vector<SecuredDirectory> const directories{
        {owner_.root_, owner_.machine_directory_security_.get()},
        {owner_.lock_directory_, owner_.machine_directory_security_.get()},
        {owner_.root_ / "subjects",
         owner_.machine_directory_security_.get()},
        {owner_.log_directory_.parent_path(),
         owner_.subject_directory_security_.get()},
        {owner_.log_directory_, owner_.subject_directory_security_.get()},
        {owner_.export_directory_, owner_.subject_directory_security_.get()},
    };
    directory_guards_.reserve(directories.size());
    for (auto const& [path, security] : directories) {
      auto directory = open_secure_directory(path, security);
      if (directory.get() == nullptr) {
        error_ = "log directory security validation failed: win32:" +
                 std::to_string(::GetLastError());
        return;
      }
      directory_guards_.push_back(std::move(directory));
    }
    lock_ = std::make_unique<FileLock>(
        owner_.lock_path_, owner_.machine_file_security_.get());
#else
    std::error_code directory_error;
    std::filesystem::create_directories(owner_.log_directory_, directory_error);
    if (!directory_error) {
      std::filesystem::create_directories(owner_.lock_directory_,
                                          directory_error);
    }
    if (!directory_error) {
      std::filesystem::create_directories(owner_.export_directory_,
                                          directory_error);
    }
    if (directory_error) {
      error_ = "log directory creation failed: " + directory_error.message();
      return;
    }
    lock_ = std::make_unique<FileLock>(owner_.lock_path_);
#endif
    if (!lock_->acquired()) {
      error_ = lock_->error();
      return;
    }
#ifdef _WIN32
    auto bytes = read_all_secure(owner_.log_path_,
                                 owner_.subject_file_security_.get(), error_);
#else
    auto bytes = read_all(owner_.log_path_, error_);
#endif
    if (bytes.has_value()) {
      bytes_ = std::move(*bytes);
    }
  }

  [[nodiscard]] std::string_view bytes() const noexcept override {
    return bytes_;
  }

  [[nodiscard]] std::string_view read_error() const noexcept override {
    return error_;
  }

  [[nodiscard]] LogStorageWriteResult replace(std::string bytes) override {
    if (!error_.empty()) {
      return {.error = error_};
    }
#ifdef _WIN32
    auto result = write_and_flush(owner_.candidate_path_, bytes,
                                  owner_.subject_file_security_.get());
#else
    auto result = write_and_flush(owner_.candidate_path_, bytes);
#endif
    if (!result.committed) {
      return result;
    }
#ifdef _WIN32
    result = atomic_replace(owner_.candidate_path_, owner_.log_path_,
                            owner_.subject_file_security_.get());
#else
    result = atomic_replace(owner_.candidate_path_, owner_.log_path_);
#endif
    if (!result.committed) {
      return result;
    }
    auto const publication_verified = result.verified;
    auto warning = std::move(result.error);
    std::string read_error;
#ifdef _WIN32
    auto persisted = read_all_secure(owner_.log_path_,
                                     owner_.subject_file_security_.get(),
                                     read_error);
#else
    auto persisted = read_all(owner_.log_path_, read_error);
#endif
    if (!persisted.has_value() || *persisted != bytes) {
      bytes_ = std::move(bytes);
      return {.committed = true,
              .verified = false,
              .error = read_error.empty()
                           ? "log reread validation failed after publication"
                           : std::move(read_error)};
    }
    bytes_ = std::move(bytes);
    return {.committed = true,
            .verified = publication_verified,
            .error = std::move(warning)};
  }

  [[nodiscard]] LogStorageExportResult write_diagnostic(
      std::string bytes) override {
    if (!error_.empty()) {
      return {.error = error_};
    }
    std::filesystem::path target;
    for (std::uint64_t sequence = 1; sequence <= 999'999; ++sequence) {
      auto candidate = owner_.export_directory_ /
                       ("diagnostic-" + padded_sequence(sequence) +
                        ".azzsdiag");
      std::error_code exists_error;
      auto const exists = std::filesystem::exists(candidate, exists_error);
      if (exists_error) {
        return {.error = "diagnostic export existence check failed: " +
                         exists_error.message()};
      }
      if (!exists) {
        target = std::move(candidate);
        break;
      }
    }
    if (target.empty()) {
      return {.error = "diagnostic export sequence is exhausted"};
    }

#ifdef _WIN32
    auto result = write_and_flush(owner_.export_candidate_path_, bytes,
                                  owner_.subject_file_security_.get());
#else
    auto result = write_and_flush(owner_.export_candidate_path_, bytes);
#endif
    if (!result.committed) {
      return {.error = std::move(result.error)};
    }
#ifdef _WIN32
    result = atomic_replace(owner_.export_candidate_path_, target,
                            owner_.subject_file_security_.get());
#else
    result = atomic_replace(owner_.export_candidate_path_, target);
#endif
    if (!result.committed) {
      return {.error = std::move(result.error)};
    }
    auto const publication_verified = result.verified;
    auto warning = std::move(result.error);
    std::string read_error;
#ifdef _WIN32
    auto persisted = read_all_secure(target,
                                     owner_.subject_file_security_.get(),
                                     read_error);
#else
    auto persisted = read_all(target, read_error);
#endif
    if (!persisted.has_value() || *persisted != bytes) {
      return {.written = true,
              .verified = false,
              .file_name = path_to_utf8(target),
              .error = read_error.empty()
                           ? "diagnostic export reread validation failed after publication"
                           : std::move(read_error)};
    }
    return {.written = true,
            .verified = publication_verified,
            .file_name = path_to_utf8(target),
            .error = std::move(warning)};
  }

 private:
  LocalFileLogStorage::Impl& owner_;
#ifdef _WIN32
  std::vector<NativeHandle> directory_guards_;
#endif
  std::unique_ptr<FileLock> lock_;
  std::string bytes_;
  std::string error_;
};

}  // namespace

LocalFileLogStorage::LocalFileLogStorage(std::string device_root_utf8,
                                         std::string subject_id)
    : impl_(std::make_unique<Impl>(std::move(device_root_utf8),
                                  std::move(subject_id))) {}

LocalFileLogStorage::~LocalFileLogStorage() = default;
LocalFileLogStorage::LocalFileLogStorage(LocalFileLogStorage&&) noexcept =
    default;
LocalFileLogStorage& LocalFileLogStorage::operator=(
    LocalFileLogStorage&&) noexcept = default;

std::unique_ptr<LogStorageTransaction>
LocalFileLogStorage::begin_transaction() {
  return std::make_unique<LocalFileLogTransaction>(*impl_);
}

}  // namespace azzs::adapters::infrastructure
