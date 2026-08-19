#include "azzs/testing/windows_device_data_environment_test_seam.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <Aclapi.h>
#include <Sddl.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace azzs::testing {
namespace {

using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;

struct HandleCloser final {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct SecurityTemplate final {
  explicit SecurityTemplate(PSECURITY_DESCRIPTOR raw)
      : descriptor(raw, &::LocalFree) {}

  LocalMemory descriptor;
  PSID owner{};
  PACL dacl{};
};

struct SecureDirectoryResult final {
  UniqueHandle handle;
  adapters::windows::DeviceDataEnvironmentError error{
      adapters::windows::DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle.get() != nullptr && handle.get() != INVALID_HANDLE_VALUE;
  }
};

struct AncestorResult final {
  std::vector<UniqueHandle> handles;
  adapters::windows::DeviceDataEnvironmentError error{
      adapters::windows::DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == adapters::windows::DeviceDataEnvironmentError::none;
  }
};

struct ExistingDirectoryResult final {
  UniqueHandle handle;
  adapters::windows::DeviceDataEnvironmentError error{
      adapters::windows::DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle.get() != nullptr && handle.get() != INVALID_HANDLE_VALUE;
  }
};

[[nodiscard]] adapters::windows::DeviceDataEnvironmentResult failure(
    adapters::windows::DeviceDataEnvironmentError error,
    DWORD raw_error,
    std::string detail) {
  return {.environment = std::nullopt,
          .error = error,
          .raw_error = raw_error,
          .detail = std::move(detail)};
}

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(
    std::string const& value) {
  auto const size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0);
  if (size <= 0) {
    return std::nullopt;
  }
  std::wstring result(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(),
                            size) != size) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<SecurityTemplate> parse_security_template(
    std::wstring const& sddl) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
    return std::nullopt;
  }
  SecurityTemplate result{raw};
  BOOL owner_defaulted = FALSE;
  BOOL dacl_present = FALSE;
  BOOL dacl_defaulted = FALSE;
  if (!::GetSecurityDescriptorOwner(result.descriptor.get(), &result.owner,
                                    &owner_defaulted) ||
      result.owner == nullptr ||
      !::GetSecurityDescriptorDacl(result.descriptor.get(), &dacl_present,
                                   &result.dacl, &dacl_defaulted) ||
      !dacl_present || result.dacl == nullptr) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] ExistingDirectoryResult open_plain_directory(
    std::filesystem::path const& path) {
  UniqueHandle directory{::CreateFileW(
      path.c_str(),
      FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (directory.get() == INVALID_HANDLE_VALUE) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    directory_creation_failed,
            .raw_error = ::GetLastError(),
            .detail = "test storage ancestor could not be opened safely"};
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    directory_creation_failed,
            .raw_error = ::GetLastError(),
            .detail = "test storage ancestor attributes could not be read"};
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    unsafe_storage_path,
            .raw_error = ERROR_REPARSE_TAG_INVALID,
            .detail = "test storage ancestor contains a reparse point"};
  }
  return {.handle = std::move(directory)};
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

[[nodiscard]] SecureDirectoryResult validate_secure_directory(
    std::filesystem::path const& path,
    SecurityTemplate const& expected) {
  UniqueHandle directory{::CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (directory.get() == INVALID_HANDLE_VALUE) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    directory_creation_failed,
            .raw_error = ::GetLastError(),
            .detail = "test managed directory could not be opened safely"};
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    directory_creation_failed,
            .raw_error = ::GetLastError(),
            .detail = "test managed directory attributes could not be read"};
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    unsafe_storage_path,
            .raw_error = ERROR_REPARSE_TAG_INVALID,
            .detail = "test managed storage path is not a plain directory"};
  }

  PSID owner = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
  auto const security_error = ::GetSecurityInfo(
      directory.get(), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &raw_descriptor);
  LocalMemory descriptor{raw_descriptor, &::LocalFree};
  if (security_error != ERROR_SUCCESS || raw_descriptor == nullptr ||
      owner == nullptr || dacl == nullptr) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    access_control_failed,
            .raw_error = security_error,
            .detail = "test managed directory security could not be read"};
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!::GetSecurityDescriptorControl(raw_descriptor, &control, &revision) ||
      (control & SE_DACL_PROTECTED) == 0 ||
      !::EqualSid(owner, expected.owner) || !equal_acl(dacl, expected.dacl)) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    unsafe_storage_path,
            .raw_error = ERROR_INVALID_SECURITY_DESCR,
            .detail =
                "test managed directory owner or protected DACL does not match the contract"};
  }
  return {.handle = std::move(directory)};
}

[[nodiscard]] SecureDirectoryResult ensure_secure_directory(
    std::filesystem::path const& path,
    SecurityTemplate const& security) {
  SECURITY_ATTRIBUTES attributes{
      .nLength = sizeof(SECURITY_ATTRIBUTES),
      .lpSecurityDescriptor = security.descriptor.get(),
      .bInheritHandle = FALSE,
  };
  if (!::CreateDirectoryW(path.c_str(), &attributes)) {
    auto const error = ::GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      return {.error =
                  adapters::windows::DeviceDataEnvironmentError::
                      directory_creation_failed,
              .raw_error = error,
              .detail = "test managed directory could not be created"};
    }
  }
  return validate_secure_directory(path, security);
}

[[nodiscard]] AncestorResult hold_safe_ancestors(
    std::filesystem::path const& path) {
  auto current = path.root_path();
  if (current.empty()) {
    return {.error =
                adapters::windows::DeviceDataEnvironmentError::
                    unsupported_storage_location,
            .raw_error = ERROR_INVALID_NAME,
            .detail = "test managed storage path is not absolute"};
  }

  std::vector<std::filesystem::path> paths{current};
  for (auto const& component : path.relative_path()) {
    if (component == L".") {
      continue;
    }
    current /= component;
    paths.push_back(current);
  }
  AncestorResult result;
  for (auto const& candidate : paths) {
    auto directory = open_plain_directory(candidate);
    if (!directory) {
      return {.error = directory.error,
              .raw_error = directory.raw_error,
              .detail = std::move(directory.detail)};
    }
    result.handles.push_back(std::move(directory.handle));
  }
  return result;
}

[[nodiscard]] adapters::windows::DeviceDataEnvironmentResult
resolve_test_subject(WindowsDeviceDataIdentityEvidence const& evidence) {
  using Error = adapters::windows::DeviceDataEnvironmentError;
  auto const wts_error = [&]() -> DWORD {
    if (evidence.wts_user_name.has_value() &&
        !evidence.wts_user_name->empty() &&
        (!evidence.wts_domain_name.has_value() ||
         evidence.wts_domain_name->empty())) {
      auto const domain_error =
          evidence.wts_domain_raw_error == ERROR_SUCCESS
              ? evidence.wts_raw_error
              : evidence.wts_domain_raw_error;
      return domain_error == ERROR_SUCCESS ? ERROR_NONE_MAPPED : domain_error;
    }
    return evidence.wts_raw_error;
  }();
  if (!evidence.process_sid.has_value()) {
    auto const error = wts_error != ERROR_SUCCESS
                           ? wts_error
                           : evidence.desktop_shell_raw_error;
    return failure(Error::interactive_subject_unavailable,
                   error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error,
                   "test process SID is unavailable");
  }

  auto process_wide = utf8_to_wide(*evidence.process_sid);
  PSID process_raw = nullptr;
  if (!process_wide.has_value() ||
      !::ConvertStringSidToSidW(process_wide->c_str(), &process_raw)) {
    return failure(Error::interactive_subject_unavailable, ::GetLastError(),
                   "test process SID is invalid");
  }
  LocalMemory process{process_raw, &::LocalFree};

  auto check_candidate = [&](std::string const& candidate)
      -> adapters::windows::DeviceDataEnvironmentResult {
    auto candidate_wide = utf8_to_wide(candidate);
    PSID candidate_raw = nullptr;
    if (!candidate_wide.has_value() ||
        !::ConvertStringSidToSidW(candidate_wide->c_str(), &candidate_raw)) {
      return failure(Error::interactive_subject_unavailable, ::GetLastError(),
                     "test session SID is invalid");
    }
    LocalMemory candidate_memory{candidate_raw, &::LocalFree};
    if (!::EqualSid(process.get(), candidate_raw)) {
      return failure(Error::alternate_credentials_not_supported,
                     ERROR_ACCESS_DENIED,
                     "test session identity differs from the process token");
    }
    return {.environment = adapters::windows::DeviceDataEnvironment{
                .subject_id = candidate}};
  };

  if (evidence.wts_session_sid.has_value()) {
    return check_candidate(*evidence.wts_session_sid);
  }
  if (evidence.wts_user_name.has_value() &&
      !evidence.wts_user_name->empty() &&
      (!evidence.wts_domain_name.has_value() ||
       evidence.wts_domain_name->empty()) &&
      wts_error == ERROR_NONE_MAPPED) {
    return failure(Error::interactive_subject_unavailable, wts_error,
                   "an unqualified WTS username is not resolved");
  }
  if (evidence.desktop_shell_sid.has_value() &&
      evidence.desktop_shell_session_matches) {
    return check_candidate(*evidence.desktop_shell_sid);
  }
  auto const error = wts_error != ERROR_SUCCESS
                         ? wts_error
                         : evidence.desktop_shell_raw_error;
  return failure(Error::interactive_subject_unavailable,
                 error == ERROR_SUCCESS ? ERROR_NOT_FOUND : error,
                 "test interactive subject is unavailable");
}

[[nodiscard]] adapters::windows::DeviceDataEnvironmentResult
prepare_test_root(WindowsDeviceDataTestOptions options) {
  using Error = adapters::windows::DeviceDataEnvironmentError;
  auto wide_root = utf8_to_wide(options.root_override_utf8);
  if (!wide_root.has_value() || wide_root->empty() ||
      wide_root->starts_with(L"\\\\")) {
    return failure(Error::unsupported_storage_location, ERROR_BAD_NETPATH,
                   "test root must be a local fixed path");
  }
  std::filesystem::path root{*wide_root};
  wchar_t volume[MAX_PATH]{};
  if (!::GetVolumePathNameW(root.c_str(), volume, MAX_PATH) ||
      ::GetDriveTypeW(volume) != DRIVE_FIXED) {
    return failure(Error::unsupported_storage_location, ERROR_NOT_SUPPORTED,
                   "test root must be on a local fixed volume");
  }
  auto ancestors = hold_safe_ancestors(root.parent_path());
  if (!ancestors) {
    return failure(ancestors.error, ancestors.raw_error,
                   std::move(ancestors.detail));
  }
  std::vector<UniqueHandle> directory_guards = std::move(ancestors.handles);

  std::string subject_id;
  if (options.subject_override.has_value()) {
    auto subject_wide = utf8_to_wide(*options.subject_override);
    PSID subject_raw = nullptr;
    if (!subject_wide.has_value() ||
        !::ConvertStringSidToSidW(subject_wide->c_str(), &subject_raw)) {
      return failure(Error::interactive_subject_unavailable, ::GetLastError(),
                     "test subject override is not a valid SID");
    }
    LocalMemory subject_memory{subject_raw, &::LocalFree};
    subject_id = *options.subject_override;
  } else {
    auto subject = resolve_test_subject(options.test_identity_evidence);
    if (!subject) {
      return subject;
    }
    subject_id = subject.environment->subject_id;
  }

  auto machine_security = parse_security_template(
      L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
  auto subject_wide = utf8_to_wide(subject_id);
  if (!subject_wide.has_value()) {
    return failure(Error::access_control_failed, ERROR_NO_UNICODE_TRANSLATION,
                   "test subject SID could not be encoded");
  }
  auto subject_security = parse_security_template(
      L"O:" + *subject_wide + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" +
      *subject_wide + L")");
  if (!machine_security.has_value() || !subject_security.has_value()) {
    return failure(Error::access_control_failed, ::GetLastError(),
                   "test security descriptor could not be prepared");
  }
  auto ensure = [&](std::filesystem::path const& path,
                    SecurityTemplate const& security)
      -> std::optional<adapters::windows::DeviceDataEnvironmentResult> {
    auto prepared = ensure_secure_directory(path, security);
    if (!prepared) {
      return failure(prepared.error, prepared.raw_error,
                     std::move(prepared.detail));
    }
    directory_guards.push_back(std::move(prepared.handle));
    return std::nullopt;
  };

  auto const device = root / L"device";
  auto const state = device / L"state";
  auto const locks = root / L"locks";
  auto const subjects = root / L"subjects";
  for (auto const& path : {root, device, state, locks, subjects}) {
    if (auto failed = ensure(path, *machine_security); failed.has_value()) {
      return std::move(*failed);
    }
  }
  auto const subject_root =
      subjects / std::filesystem::path{subject_id};
  for (auto const& path : {subject_root, subject_root / L"state",
                           subject_root / L"logs", subject_root / L"exports"}) {
    if (auto failed = ensure(path, *subject_security); failed.has_value()) {
      return std::move(*failed);
    }
  }
  return {.environment = adapters::windows::DeviceDataEnvironment{
              .root_utf8 = options.root_override_utf8,
              .subject_id = subject_id}};
}

}  // namespace

WindowsDeviceDataSubjectResolution
resolve_windows_device_data_subject_for_test(
    WindowsDeviceDataIdentityEvidence evidence) {
  auto result = resolve_test_subject(evidence);
  return {.subject_id = result.environment.has_value()
                            ? std::optional<std::string>{
                                  result.environment->subject_id}
                            : std::nullopt,
          .error = result.error,
          .raw_error = result.raw_error};
}

adapters::windows::DeviceDataEnvironmentResult
prepare_windows_device_data_for_test(WindowsDeviceDataTestOptions options) {
  return prepare_test_root(std::move(options));
}

}  // namespace azzs::testing
