#include "azzs/adapters/windows/bundled_catalog_resource_reader.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace azzs::adapters::windows {
namespace {

class ScopedHandle final {
 public:
  explicit ScopedHandle(HANDLE value) noexcept : value_(value) {}

  ~ScopedHandle() {
    if (value_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(value_);
    }
  }

  ScopedHandle(ScopedHandle const&) = delete;
  ScopedHandle& operator=(ScopedHandle const&) = delete;

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept {
    return value_ != INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] bool is_not_reparse_point(std::filesystem::path const& path) {
  auto const attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

[[nodiscard]] bool path_chain_has_no_reparse_points(
    std::filesystem::path const& path) {
  if (!path.is_absolute()) {
    return false;
  }

  auto current = path.root_path();
  if (current.empty() || !is_not_reparse_point(current)) {
    return false;
  }
  for (auto const& segment : path.relative_path()) {
    current /= segment;
    if (!is_not_reparse_point(current)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool relative_path_is_safe(
    std::filesystem::path const& relative_path) {
  if (relative_path.empty() || relative_path.is_absolute() ||
      relative_path.has_root_name() || relative_path.has_root_directory()) {
    return false;
  }
  for (auto const& segment : relative_path) {
    if (segment.empty() || segment == std::filesystem::path{L"."} ||
        segment == std::filesystem::path{L".."}) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool plain_regular_file(HANDLE file) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  FILE_STANDARD_INFO standard{};
  return ::GetFileType(file) == FILE_TYPE_DISK &&
         ::GetFileInformationByHandleEx(file, FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) &&
         ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                        sizeof(standard)) &&
         !standard.Directory &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

[[nodiscard]] bool plain_directory(HANDLE directory) {
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  return ::GetFileInformationByHandleEx(directory, FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
         (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

[[nodiscard]] std::optional<std::wstring> final_path(HANDLE handle) {
  auto const length = ::GetFinalPathNameByHandleW(
      handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (length == 0) {
    return std::nullopt;
  }
  std::wstring path(length, L'\0');
  auto const written = ::GetFinalPathNameByHandleW(
      handle, path.data(), length, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (written == 0 || written >= length) {
    return std::nullopt;
  }
  path.resize(written);
  return path;
}

[[nodiscard]] bool resource_is_within_open_root(
    HANDLE root, HANDLE resource,
    std::filesystem::path const& relative_path) {
  auto const root_path = final_path(root);
  auto const resource_path = final_path(resource);
  if (!root_path.has_value() || !resource_path.has_value()) {
    return false;
  }
  auto const expected_path =
      (std::filesystem::path{*root_path} / relative_path).wstring();
  return ::CompareStringOrdinal(
             expected_path.c_str(), static_cast<int>(expected_path.size()),
             resource_path->c_str(), static_cast<int>(resource_path->size()),
             TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool length_matches(HANDLE file, std::uintmax_t expected_bytes) {
  FILE_STANDARD_INFO standard{};
  return ::GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                        sizeof(standard)) &&
         !standard.Directory && standard.EndOfFile.QuadPart >= 0 &&
         static_cast<std::uintmax_t>(standard.EndOfFile.QuadPart) ==
             expected_bytes;
}

[[nodiscard]] bool bundled_catalog_resource_matches(
    std::string const& bytes,
    std::array<std::uint8_t, 32> const& expected_sha256) {
  HCRYPTPROV provider = 0;
  if (!::CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
    return false;
  }
  HCRYPTHASH hash = 0;
  if (!::CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    ::CryptReleaseContext(provider, 0);
    return false;
  }

  bool succeeded = true;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const remaining = bytes.size() - offset;
    auto const chunk = static_cast<DWORD>(std::min(
        remaining,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    if (!::CryptHashData(
            hash, reinterpret_cast<BYTE const*>(bytes.data() + offset),
            chunk, 0)) {
      succeeded = false;
      break;
    }
    offset += chunk;
  }

  std::array<std::uint8_t, 32> digest{};
  DWORD digest_bytes = static_cast<DWORD>(digest.size());
  if (succeeded &&
      !::CryptGetHashParam(hash, HP_HASHVAL,
                           reinterpret_cast<BYTE*>(digest.data()),
                           &digest_bytes, 0)) {
    succeeded = false;
  }
  ::CryptDestroyHash(hash);
  ::CryptReleaseContext(provider, 0);
  return succeeded && digest_bytes == digest.size() &&
         std::equal(digest.begin(), digest.end(), expected_sha256.begin());
}

[[nodiscard]] std::optional<std::string> read_exact_bytes(
    HANDLE file, std::uintmax_t expected_bytes) {
  if (expected_bytes == 0 ||
      expected_bytes > static_cast<std::uintmax_t>(
                           (std::numeric_limits<std::size_t>::max)())) {
    return std::nullopt;
  }

  std::string bytes(static_cast<std::size_t>(expected_bytes), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const remaining = bytes.size() - offset;
    auto const requested = static_cast<DWORD>(std::min(
        remaining,
        static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    if (!::ReadFile(file, bytes.data() + offset, requested, &read, nullptr) ||
        read == 0) {
      return std::nullopt;
    }
    offset += read;
  }
  return bytes;
}

}  // namespace

VerifiedBundledCatalogResource::VerifiedBundledCatalogResource(
    std::string bytes) noexcept
    : bytes_(std::move(bytes)) {}

std::string const& VerifiedBundledCatalogResource::bytes() const noexcept {
  return bytes_;
}

WindowsBundledCatalogResourceReader::WindowsBundledCatalogResourceReader(
    std::filesystem::path resource_root)
    : resource_root_(std::move(resource_root)) {}

BundledCatalogResourceRead WindowsBundledCatalogResourceReader::read(
    std::filesystem::path const& relative_path,
    BundledCatalogResourceExpectation const& expectation) const {
  if (!relative_path_is_safe(relative_path) ||
      !path_chain_has_no_reparse_points(resource_root_)) {
    return {.error = "bundled catalog resource path is unsafe"};
  }

  auto const resource_path = resource_root_ / relative_path;
  if (!path_chain_has_no_reparse_points(resource_path)) {
    return {.error = "bundled catalog resource path is unavailable"};
  }

  ScopedHandle root{::CreateFileW(
      resource_root_.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr)};
  if (!root.valid() || !plain_directory(root.get())) {
    return {.error = "bundled catalog resource root is unavailable"};
  }

  ScopedHandle file{::CreateFileW(
      resource_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr)};
  if (!file.valid() || !plain_regular_file(file.get()) ||
      !resource_is_within_open_root(root.get(), file.get(), relative_path) ||
      !length_matches(file.get(), expectation.byte_count)) {
    return {.error = "bundled catalog resource could not be opened safely"};
  }

  auto bytes = read_exact_bytes(file.get(), expectation.byte_count);
  if (!bytes.has_value() ||
      !bundled_catalog_resource_matches(*bytes, expectation.sha256)) {
    return {.error = "bundled catalog resource integrity check failed"};
  }
  return {.resource = VerifiedBundledCatalogResource{std::move(*bytes)}};
}

}  // namespace azzs::adapters::windows
