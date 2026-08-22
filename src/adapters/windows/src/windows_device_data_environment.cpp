#include "azzs/adapters/windows/windows_device_data_environment.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <Aclapi.h>
#include <KnownFolders.h>
#include <ShlObj.h>
#include <Sddl.h>
#include <WtsApi32.h>

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace azzs::adapters::windows {
namespace {

using LocalMemory = std::unique_ptr<void, decltype(&::LocalFree)>;
using WtsMemory = std::unique_ptr<void, decltype(&::WTSFreeMemory)>;
using KnownFolderMemory = std::unique_ptr<wchar_t, decltype(&::CoTaskMemFree)>;

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
  DeviceDataEnvironmentError error{DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle.get() != nullptr && handle.get() != INVALID_HANDLE_VALUE;
  }
};

struct AncestorResult final {
  std::vector<UniqueHandle> handles;
  DeviceDataEnvironmentError error{DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return error == DeviceDataEnvironmentError::none;
  }
};

struct ExistingDirectoryResult final {
  UniqueHandle handle;
  DeviceDataEnvironmentError error{DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
  std::string detail;

  [[nodiscard]] explicit operator bool() const noexcept {
    return handle.get() != nullptr && handle.get() != INVALID_HANDLE_VALUE;
  }
};

[[nodiscard]] DeviceDataEnvironmentResult failure(
    DeviceDataEnvironmentError error,
    std::uint32_t raw_error,
    std::string detail) {
  return DeviceDataEnvironmentResult{
      .environment = std::nullopt,
      .error = error,
      .raw_error = raw_error,
      .detail = std::move(detail),
  };
}

[[nodiscard]] std::optional<std::wstring> utf8_to_wide(
    std::string const& value) {
  if (value.empty()) {
    return std::wstring{};
  }
  auto const size = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
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

[[nodiscard]] std::optional<std::string> wide_to_utf8(
    std::wstring const& value) {
  if (value.empty()) {
    return std::string{};
  }
  auto const size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                          value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0, nullptr, nullptr);
  if (size <= 0) {
    return std::nullopt;
  }
  std::string result(static_cast<std::size_t>(size), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size,
                            nullptr, nullptr) != size) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<std::wstring> sid_to_string(PSID sid) {
  wchar_t* raw = nullptr;
  if (!::ConvertSidToStringSidW(sid, &raw)) {
    return std::nullopt;
  }
  LocalMemory memory{raw, &::LocalFree};
  return std::wstring{raw};
}

[[nodiscard]] std::optional<std::vector<std::byte>> copy_sid(PSID source,
                                                               DWORD& error) {
  error = ERROR_SUCCESS;
  if (source == nullptr || !::IsValidSid(source)) {
    error = ERROR_INVALID_SID;
    return std::nullopt;
  }
  auto const sid_size = ::GetLengthSid(source);
  if (sid_size == 0) {
    error = ERROR_INVALID_SID;
    return std::nullopt;
  }
  std::vector<std::byte> sid(sid_size);
  if (!::CopySid(sid_size, sid.data(), source)) {
    error = ::GetLastError();
    return std::nullopt;
  }
  return sid;
}

[[nodiscard]] std::optional<std::vector<std::byte>> token_user_sid(
    HANDLE token,
    DWORD& error) {
  error = ERROR_SUCCESS;
  DWORD needed = 0;
  ::SetLastError(ERROR_SUCCESS);
  ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  auto const size_error = ::GetLastError();
  if (needed == 0 || size_error != ERROR_INSUFFICIENT_BUFFER) {
    error = size_error;
    return std::nullopt;
  }
  std::vector<std::byte> buffer(needed);
  if (!::GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
    error = ::GetLastError();
    return std::nullopt;
  }
  auto const* user = reinterpret_cast<TOKEN_USER const*>(buffer.data());
  return copy_sid(user->User.Sid, error);
}

[[nodiscard]] std::optional<std::wstring> query_session_text(
    DWORD session_id,
    WTS_INFO_CLASS info_class,
    DWORD& error) {
  error = ERROR_SUCCESS;
  wchar_t* raw = nullptr;
  DWORD bytes = 0;
  if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session_id,
                                     info_class, &raw, &bytes)) {
    error = ::GetLastError();
    return std::nullopt;
  }
  WtsMemory memory{raw, &::WTSFreeMemory};
  if (raw == nullptr || bytes <= sizeof(wchar_t)) {
    error = ERROR_NONE_MAPPED;
    return std::nullopt;
  }
  return std::wstring{raw};
}

[[nodiscard]] std::optional<std::vector<std::byte>> account_sid(
    std::wstring const& account,
    DWORD& error) {
  error = ERROR_SUCCESS;
  DWORD sid_size = 0;
  DWORD domain_size = 0;
  SID_NAME_USE use{};
  ::SetLastError(ERROR_SUCCESS);
  ::LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_size, nullptr,
                       &domain_size, &use);
  auto const size_error = ::GetLastError();
  if (sid_size == 0 || size_error != ERROR_INSUFFICIENT_BUFFER) {
    error = size_error;
    return std::nullopt;
  }
  std::vector<std::byte> sid(sid_size);
  std::wstring domain(domain_size, L'\0');
  if (!::LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sid_size,
                            domain.data(), &domain_size, &use)) {
    error = ::GetLastError();
    return std::nullopt;
  }
  return sid;
}

struct SubjectResult final {
  std::optional<std::wstring> sid;
  DeviceDataEnvironmentError error{DeviceDataEnvironmentError::none};
  DWORD raw_error{ERROR_SUCCESS};
};

struct SubjectEvidence final {
  std::optional<std::vector<std::byte>> process_sid;
  std::optional<std::vector<std::byte>> wts_session_sid;
  std::optional<std::vector<std::byte>> desktop_shell_sid;
  DWORD process_raw_error{ERROR_SUCCESS};
  DWORD session_raw_error{ERROR_SUCCESS};
  DWORD wts_raw_error{ERROR_SUCCESS};
  DWORD desktop_shell_raw_error{ERROR_SUCCESS};
  bool wts_unqualified_user{false};
  bool wts_rpc_transport_failure{false};
};

[[nodiscard]] constexpr bool is_wts_rpc_transport_failure(
    DWORD error) noexcept {
  return error == RPC_S_SERVER_UNAVAILABLE ||
         error == RPC_S_NOT_LISTENING ||
         error == RPC_S_SERVER_TOO_BUSY || error == RPC_S_CALL_FAILED ||
         error == RPC_S_CALL_FAILED_DNE;
}

[[nodiscard]] std::optional<std::vector<std::byte>> wts_account_sid(
    std::optional<std::wstring> const& user,
    DWORD user_error,
    std::optional<std::wstring> const& domain,
    DWORD domain_error,
    DWORD& error) {
  if (!user.has_value() || user->empty()) {
    error = user_error == ERROR_SUCCESS ? ERROR_NONE_MAPPED : user_error;
    return std::nullopt;
  }
  if (!domain.has_value() || domain->empty()) {
    error = domain_error == ERROR_SUCCESS ? ERROR_NONE_MAPPED : domain_error;
    return std::nullopt;
  }
  return account_sid(*domain + L"\\" + *user, error);
}

[[nodiscard]] std::optional<std::vector<std::byte>> wts_session_sid(
    DWORD session_id,
    DWORD& error,
    bool& unqualified_user,
    bool& rpc_transport_failure) {
  unqualified_user = false;
  rpc_transport_failure = false;
  DWORD user_error = ERROR_SUCCESS;
  auto user = query_session_text(session_id, WTSUserName, user_error);
  if (!user.has_value() || user->empty()) {
    rpc_transport_failure = is_wts_rpc_transport_failure(user_error);
    return wts_account_sid(user, user_error, std::nullopt, ERROR_SUCCESS,
                           error);
  }
  DWORD domain_error = ERROR_SUCCESS;
  auto domain = query_session_text(session_id, WTSDomainName, domain_error);
  if (!domain.has_value() || domain->empty()) {
    rpc_transport_failure = is_wts_rpc_transport_failure(domain_error);
  }
  if ((!domain.has_value() || domain->empty()) &&
      domain_error == ERROR_NONE_MAPPED) {
    // A username without a domain is ambiguous. Do not use a shell process as
    // an alternate identity source after WTS has returned partial identity.
    unqualified_user = true;
  }
  return wts_account_sid(user, user_error, domain, domain_error, error);
}

[[nodiscard]] std::optional<std::vector<std::byte>> desktop_shell_sid(
    DWORD expected_session_id,
    DWORD& error) {
  error = ERROR_SUCCESS;
  auto const shell = ::GetShellWindow();
  if (shell == nullptr) {
    error = ERROR_NOT_FOUND;
    return std::nullopt;
  }
  DWORD shell_process_id = 0;
  if (::GetWindowThreadProcessId(shell, &shell_process_id) == 0 ||
      shell_process_id == 0) {
    error = ::GetLastError();
    if (error == ERROR_SUCCESS) {
      error = ERROR_NOT_FOUND;
    }
    return std::nullopt;
  }
  DWORD shell_session_id = 0;
  if (!::ProcessIdToSessionId(shell_process_id, &shell_session_id)) {
    error = ::GetLastError();
    return std::nullopt;
  }
  if (shell_session_id != expected_session_id) {
    error = ERROR_NOT_FOUND;
    return std::nullopt;
  }
  HANDLE raw_process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                     shell_process_id);
  if (raw_process == nullptr) {
    error = ::GetLastError();
    return std::nullopt;
  }
  UniqueHandle process{raw_process};
  HANDLE raw_token = nullptr;
  if (!::OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token)) {
    error = ::GetLastError();
    return std::nullopt;
  }
  UniqueHandle token{raw_token};
  return token_user_sid(token.get(), error);
}

[[nodiscard]] SubjectEvidence collect_subject_evidence() {
  SubjectEvidence evidence;
  HANDLE raw_token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
    evidence.process_raw_error = ::GetLastError();
    return evidence;
  }
  UniqueHandle token{raw_token};
  evidence.process_sid = token_user_sid(token.get(), evidence.process_raw_error);
  if (!evidence.process_sid.has_value()) {
    return evidence;
  }

  DWORD session_id = 0;
  if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session_id)) {
    evidence.session_raw_error = ::GetLastError();
    return evidence;
  }
  evidence.wts_session_sid =
      wts_session_sid(session_id, evidence.wts_raw_error,
                      evidence.wts_unqualified_user,
                      evidence.wts_rpc_transport_failure);
  if (evidence.wts_session_sid.has_value()) {
    return evidence;
  }
  evidence.desktop_shell_sid =
      desktop_shell_sid(session_id, evidence.desktop_shell_raw_error);
  return evidence;
}

[[nodiscard]] DWORD unavailable_subject_error(SubjectEvidence const& evidence) {
  // WTS is the authoritative interactive-session source. When its RPC/query
  // path and the shell fallback both fail, retain the WTS failure for callers.
  if (evidence.wts_raw_error != ERROR_SUCCESS) {
    return evidence.wts_raw_error;
  }
  if (evidence.desktop_shell_raw_error != ERROR_SUCCESS) {
    return evidence.desktop_shell_raw_error;
  }
  if (evidence.session_raw_error != ERROR_SUCCESS) {
    return evidence.session_raw_error;
  }
  if (evidence.process_raw_error != ERROR_SUCCESS) {
    return evidence.process_raw_error;
  }
  return ERROR_NOT_FOUND;
}

[[nodiscard]] SubjectResult resolve_subject_evidence(
    SubjectEvidence const& evidence) {
  if (!evidence.process_sid.has_value()) {
    return {.error = DeviceDataEnvironmentError::interactive_subject_unavailable,
            .raw_error = unavailable_subject_error(evidence)};
  }

  auto resolve_matching_sid = [&](std::vector<std::byte> const& candidate)
      -> SubjectResult {
    auto const process_sid = const_cast<std::byte*>(evidence.process_sid->data());
    auto const candidate_sid = const_cast<std::byte*>(candidate.data());
    if (!::EqualSid(process_sid, candidate_sid)) {
      return {.error =
                  DeviceDataEnvironmentError::alternate_credentials_not_supported,
              .raw_error = ERROR_ACCESS_DENIED};
    }
    auto sid = sid_to_string(candidate_sid);
    if (!sid.has_value()) {
      return {.error = DeviceDataEnvironmentError::interactive_subject_unavailable,
              .raw_error = ::GetLastError()};
    }
    return {.sid = std::move(sid)};
  };

  if (evidence.wts_session_sid.has_value()) {
    return resolve_matching_sid(*evidence.wts_session_sid);
  }
  if (evidence.wts_unqualified_user) {
    return {.error = DeviceDataEnvironmentError::interactive_subject_unavailable,
            .raw_error = evidence.wts_raw_error == ERROR_SUCCESS
                             ? ERROR_NONE_MAPPED
                             : evidence.wts_raw_error};
  }
  if (evidence.wts_rpc_transport_failure &&
      evidence.desktop_shell_sid.has_value()) {
    return resolve_matching_sid(*evidence.desktop_shell_sid);
  }
  return {.error = DeviceDataEnvironmentError::interactive_subject_unavailable,
          .raw_error = unavailable_subject_error(evidence)};
}

[[nodiscard]] SubjectResult resolve_interactive_subject() {
  return resolve_subject_evidence(collect_subject_evidence());
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
      path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
  if (directory.get() == INVALID_HANDLE_VALUE) {
    auto const error = ::GetLastError();
    return {.error = DeviceDataEnvironmentError::directory_creation_failed,
            .raw_error = error,
            .detail = "storage ancestor could not be opened safely"};
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
    auto const error = ::GetLastError();
    return {.error = DeviceDataEnvironmentError::directory_creation_failed,
            .raw_error = error,
            .detail = "storage ancestor attributes could not be read"};
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return {.error = DeviceDataEnvironmentError::unsafe_storage_path,
            .raw_error = ERROR_REPARSE_TAG_INVALID,
            .detail = "storage ancestor contains a reparse point"};
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
    auto const error = ::GetLastError();
    return {.error = DeviceDataEnvironmentError::directory_creation_failed,
            .raw_error = error,
            .detail = "managed directory could not be opened safely"};
  }
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!::GetFileInformationByHandleEx(directory.get(), FileAttributeTagInfo,
                                      &attributes, sizeof(attributes))) {
    auto const error = ::GetLastError();
    return {.error = DeviceDataEnvironmentError::directory_creation_failed,
            .raw_error = error,
            .detail = "managed directory attributes could not be read"};
  }
  if ((attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return {.error = DeviceDataEnvironmentError::unsafe_storage_path,
            .raw_error = ERROR_REPARSE_TAG_INVALID,
            .detail = "managed storage path is not a plain directory"};
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
    return {.error = DeviceDataEnvironmentError::access_control_failed,
            .raw_error = security_error,
            .detail = "managed directory security could not be read"};
  }
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  if (!::GetSecurityDescriptorControl(raw_descriptor, &control, &revision) ||
      (control & SE_DACL_PROTECTED) == 0 ||
      !::EqualSid(owner, expected.owner) || !equal_acl(dacl, expected.dacl)) {
    return {.error = DeviceDataEnvironmentError::unsafe_storage_path,
            .raw_error = ERROR_INVALID_SECURITY_DESCR,
            .detail =
                "managed directory owner or protected DACL does not match the contract"};
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
      return {.error = DeviceDataEnvironmentError::directory_creation_failed,
              .raw_error = error,
              .detail = "managed directory could not be created"};
    }
  }
  return validate_secure_directory(path, security);
}

[[nodiscard]] AncestorResult hold_safe_ancestors(
    std::filesystem::path const& path) {
  AncestorResult result;
  auto current = path.root_path();
  if (current.empty()) {
    return {.error = DeviceDataEnvironmentError::unsupported_storage_location,
            .raw_error = ERROR_INVALID_NAME,
            .detail = "managed storage path is not absolute"};
  }

  std::vector<std::filesystem::path> paths{current};
  for (auto const& component : path.relative_path()) {
    if (component == L".") {
      continue;
    }
    current /= component;
    paths.push_back(current);
  }
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

[[nodiscard]] std::optional<std::filesystem::path> canonical_fixed_path(
    std::filesystem::path const& input,
    DWORD& error) {
  auto const native = input.native();
  if (native.empty() || native.starts_with(L"\\\\") ||
      !input.has_root_name() || !input.has_root_directory()) {
    error = ERROR_BAD_NETPATH;
    return std::nullopt;
  }
  auto const required = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (required == 0) {
    error = ::GetLastError();
    return std::nullopt;
  }
  std::vector<wchar_t> buffer(required);
  auto const copied = ::GetFullPathNameW(input.c_str(), required, buffer.data(),
                                         nullptr);
  if (copied == 0 || copied >= required) {
    error = copied == 0 ? ::GetLastError() : ERROR_INSUFFICIENT_BUFFER;
    return std::nullopt;
  }
  std::filesystem::path canonical{buffer.data()};
  if (canonical.native().starts_with(L"\\\\")) {
    error = ERROR_BAD_NETPATH;
    return std::nullopt;
  }
  auto volume_probe = canonical;
  while (::GetFileAttributesW(volume_probe.c_str()) ==
         INVALID_FILE_ATTRIBUTES) {
    auto const parent = volume_probe.parent_path();
    if (parent.empty() || parent == volume_probe) {
      error = ::GetLastError();
      return std::nullopt;
    }
    volume_probe = parent;
  }
  std::vector<wchar_t> volume(32'768, L'\0');
  if (!::GetVolumePathNameW(volume_probe.c_str(), volume.data(),
                            static_cast<DWORD>(volume.size()))) {
    error = ::GetLastError();
    return std::nullopt;
  }
  if (::GetDriveTypeW(volume.data()) != DRIVE_FIXED) {
    error = ERROR_NOT_SUPPORTED;
    return std::nullopt;
  }
  error = ERROR_SUCCESS;
  return canonical;
}

[[nodiscard]] bool is_d_drive_path(std::filesystem::path const& path) {
  auto const root_name = path.root_name().wstring();
  return root_name.size() == 2 &&
         (root_name[0] == L'D' || root_name[0] == L'd') &&
         root_name[1] == L':';
}

[[nodiscard]] std::optional<std::filesystem::path> program_data_base(
    DWORD& error) {
  error = ERROR_SUCCESS;
  wchar_t* raw = nullptr;
  auto const status =
      ::SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DEFAULT, nullptr, &raw);
  KnownFolderMemory memory{raw, &::CoTaskMemFree};
  if (FAILED(status) || raw == nullptr) {
    error = FAILED(status) ? static_cast<DWORD>(status) : ERROR_NOT_FOUND;
    return std::nullopt;
  }
  return std::filesystem::path{raw};
}

[[nodiscard]] DeviceDataEnvironmentResult prepare_device_data_impl(
    std::optional<std::string> const& diagnostic_root_utf8) {
  std::filesystem::path root;
  std::filesystem::path existing_anchor;
  std::vector<std::filesystem::path> owned_machine_prefixes;
  DWORD path_error = ERROR_SUCCESS;
  if (diagnostic_root_utf8.has_value()) {
    auto wide = utf8_to_wide(*diagnostic_root_utf8);
    if (!wide.has_value() || wide->empty()) {
      return failure(DeviceDataEnvironmentError::unsupported_storage_location,
                     ERROR_INVALID_NAME,
                     "diagnostic device data root is not valid UTF-8");
    }
    auto const candidate = std::filesystem::path{*wide};
    if (!is_d_drive_path(candidate)) {
      return failure(DeviceDataEnvironmentError::unsupported_storage_location,
                     ERROR_INVALID_DRIVE,
                     "diagnostic device data root must be on drive D:");
    }
    auto canonical = canonical_fixed_path(candidate, path_error);
    if (!canonical.has_value()) {
      return failure(DeviceDataEnvironmentError::unsupported_storage_location,
                     path_error,
                     "diagnostic device data root must be on a local fixed volume");
    }
    if (!is_d_drive_path(*canonical) || *canonical == canonical->root_path()) {
      return failure(DeviceDataEnvironmentError::unsupported_storage_location,
                     ERROR_INVALID_NAME,
                     "diagnostic device data root must be below drive D: root");
    }
    root = std::move(*canonical);
    existing_anchor = root.parent_path();
    owned_machine_prefixes.push_back(root);
  } else {
    auto base = program_data_base(path_error);
    if (!base.has_value()) {
      return failure(DeviceDataEnvironmentError::program_data_unavailable,
                     path_error, "ProgramData known folder is unavailable");
    }
    auto canonical = canonical_fixed_path(*base, path_error);
    if (!canonical.has_value()) {
      return failure(DeviceDataEnvironmentError::unsupported_storage_location,
                     path_error,
                     "ProgramData must resolve to a local fixed volume");
    }
    existing_anchor = *canonical;
    auto const vendor = existing_anchor / L"Azzs";
    auto const product = vendor / L"WindowsInitialSetupWorkbench";
    root = product / L"device-data-v1";
    owned_machine_prefixes = {vendor, product, root};
  }

  auto subject = resolve_interactive_subject();
  if (!subject.sid.has_value()) {
    return failure(subject.error, subject.raw_error,
                   "interactive state subject could not be established");
  }

  auto const subject_sid = *subject.sid;

  auto machine_security = parse_security_template(
      L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
  auto subject_security = parse_security_template(
      L"O:" + subject_sid + L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;" +
      subject_sid + L")");
  if (!machine_security.has_value() || !subject_security.has_value()) {
    return failure(DeviceDataEnvironmentError::access_control_failed,
                   ::GetLastError(),
                   "device data security descriptor could not be prepared");
  }

  auto ancestors = hold_safe_ancestors(existing_anchor);
  if (!ancestors) {
    return failure(ancestors.error, ancestors.raw_error,
                   std::move(ancestors.detail));
  }
  std::vector<UniqueHandle> directory_guards = std::move(ancestors.handles);

  auto ensure = [&](std::filesystem::path const& path,
                    SecurityTemplate const& security)
      -> std::optional<DeviceDataEnvironmentResult> {
    auto prepared = ensure_secure_directory(path, security);
    if (!prepared) {
      return failure(prepared.error, prepared.raw_error,
                     std::move(prepared.detail));
    }
    directory_guards.push_back(std::move(prepared.handle));
    return std::nullopt;
  };

  for (auto const& path : owned_machine_prefixes) {
    if (auto failed = ensure(path, *machine_security); failed.has_value()) {
      return std::move(*failed);
    }
  }

  auto const device = root / L"device";
  auto const device_state = device / L"state";
  auto const locks = root / L"locks";
  auto const subjects = root / L"subjects";
  for (auto const& path : {device, device_state, locks, subjects}) {
    if (auto failed = ensure(path, *machine_security); failed.has_value()) {
      return std::move(*failed);
    }
  }

  auto const subject_path = subjects / subject_sid;
  auto const subject_state = subject_path / L"state";
  auto const logs = subject_path / L"logs";
  auto const exports = subject_path / L"exports";
  for (auto const& path : {subject_path, subject_state, logs, exports}) {
    if (auto failed = ensure(path, *subject_security); failed.has_value()) {
      return std::move(*failed);
    }
  }

  auto root_utf8 = wide_to_utf8(root.wstring());
  auto subject_utf8 = wide_to_utf8(subject_sid);
  if (!root_utf8.has_value() || !subject_utf8.has_value()) {
    return failure(DeviceDataEnvironmentError::directory_creation_failed,
                   ERROR_NO_UNICODE_TRANSLATION,
                   "device data path could not be encoded as UTF-8");
  }
  return DeviceDataEnvironmentResult{
      .environment = DeviceDataEnvironment{
          .root_utf8 = std::move(*root_utf8),
          .subject_id = std::move(*subject_utf8),
      },
  };
}

}  // namespace

DeviceDataEnvironmentResult WindowsDeviceDataEnvironment::prepare(
    DeviceDataEnvironmentOptions options) {
  return prepare_device_data_impl(options.diagnostic_root_utf8);
}

}  // namespace azzs::adapters::windows
