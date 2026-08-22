#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include "azzs/adapters/infrastructure/software_catalog_file.hpp"
#include "azzs/adapters/windows/bundled_catalog_resource_reader.hpp"

namespace {

using azzs::adapters::infrastructure::LocalSoftwareCatalogFileReader;
using azzs::adapters::windows::BundledCatalogResourceExpectation;
using azzs::adapters::windows::WindowsBundledCatalogResourceReader;

constexpr std::array<std::uint8_t, 32> kAbcSha256{
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
constexpr BundledCatalogResourceExpectation kAbcExpectation{
    .byte_count = 3,
    .sha256 = kAbcSha256,
};

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "bundled catalog resource contract failed: " << message
              << '\n';
  }
  return condition;
}

[[nodiscard]] bool write_file(std::filesystem::path const& path,
                              std::string_view bytes) {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(output);
}

struct SymbolicLinkCreation final {
  bool created{false};
  DWORD unprivileged_error{ERROR_SUCCESS};
  DWORD fallback_error{ERROR_SUCCESS};
};

[[nodiscard]] SymbolicLinkCreation create_symbolic_link(
    std::filesystem::path const& link, std::filesystem::path const& target,
    DWORD directory_flag) {
  auto created = ::CreateSymbolicLinkW(
      link.c_str(), target.c_str(),
      directory_flag | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
  auto const unprivileged_error =
      created ? DWORD{ERROR_SUCCESS} : ::GetLastError();
  auto fallback_error = DWORD{ERROR_SUCCESS};
  if (!created) {
    created = ::CreateSymbolicLinkW(link.c_str(), target.c_str(),
                                    directory_flag);
    fallback_error = created ? DWORD{ERROR_SUCCESS} : ::GetLastError();
  }
  return {.created = created != FALSE,
          .unprivileged_error = unprivileged_error,
          .fallback_error = fallback_error};
}

[[nodiscard]] bool expect_symbolic_link_creation(
    SymbolicLinkCreation const& creation, char const* message) {
  if (creation.created) {
    return true;
  }
  std::cerr << "bundled catalog resource contract failed: " << message
            << " (CreateSymbolicLinkW ALLOW_UNPRIVILEGED_CREATE GetLastError="
            << creation.unprivileged_error
            << "; fallback GetLastError=" << creation.fallback_error << ")\n";
  return false;
}

struct JunctionReparseBuffer final {
  DWORD reparse_tag;
  WORD reparse_data_length;
  WORD reserved;
  WORD substitute_name_offset;
  WORD substitute_name_length;
  WORD print_name_offset;
  WORD print_name_length;
  wchar_t path_buffer[1];
};

static_assert(offsetof(JunctionReparseBuffer, path_buffer) == 16);

struct DirectoryJunctionCreation final {
  bool created{false};
  DWORD error{ERROR_SUCCESS};
};

// Mount-point junctions are real directory reparse points but do not require
// SeCreateSymbolicLinkPrivilege, unlike directory symbolic links.
[[nodiscard]] DirectoryJunctionCreation create_directory_junction(
    std::filesystem::path const& link, std::filesystem::path const& target) {
  if (!target.is_absolute()) {
    return {.error = ERROR_INVALID_NAME};
  }
  if (!::CreateDirectoryW(link.c_str(), nullptr)) {
    return {.error = ::GetLastError()};
  }

  auto const substitute_name = std::wstring{L"\\??\\"} + target.native();
  auto const print_name = target.native();
  auto const substitute_bytes = substitute_name.size() * sizeof(wchar_t);
  auto const print_bytes = print_name.size() * sizeof(wchar_t);
  auto const path_bytes = substitute_bytes +
                          sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
  auto const buffer_bytes = offsetof(JunctionReparseBuffer, path_buffer) +
                            path_bytes;
  auto const reparse_data_length = sizeof(WORD) * 4 + path_bytes;
  if (reparse_data_length >
          static_cast<std::size_t>((std::numeric_limits<WORD>::max)()) ||
      buffer_bytes > MAXIMUM_REPARSE_DATA_BUFFER_SIZE) {
    ::RemoveDirectoryW(link.c_str());
    return {.error = ERROR_BUFFER_OVERFLOW};
  }
  alignas(JunctionReparseBuffer)
      std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
  auto* buffer = reinterpret_cast<JunctionReparseBuffer*>(storage.data());
  buffer->reparse_tag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer->reparse_data_length = static_cast<WORD>(reparse_data_length);
  buffer->reserved = 0;
  buffer->substitute_name_offset = 0;
  buffer->substitute_name_length = static_cast<WORD>(substitute_bytes);
  buffer->print_name_offset =
      static_cast<WORD>(substitute_bytes + sizeof(wchar_t));
  buffer->print_name_length = static_cast<WORD>(print_bytes);
  auto* path = buffer->path_buffer;
  std::copy(substitute_name.begin(), substitute_name.end(), path);
  path[substitute_name.size()] = L'\0';
  auto* print = path + substitute_name.size() + 1;
  std::copy(print_name.begin(), print_name.end(), print);
  print[print_name.size()] = L'\0';

  auto const directory = ::CreateFileW(
      link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (directory == INVALID_HANDLE_VALUE) {
    auto const error = ::GetLastError();
    ::RemoveDirectoryW(link.c_str());
    return {.error = error};
  }
  DWORD written{};
  auto const created = ::DeviceIoControl(
      directory, FSCTL_SET_REPARSE_POINT, buffer,
      static_cast<DWORD>(buffer_bytes), nullptr, 0, &written, nullptr);
  auto const error = created ? DWORD{ERROR_SUCCESS} : ::GetLastError();
  ::CloseHandle(directory);
  if (!created) {
    ::RemoveDirectoryW(link.c_str());
  }
  return {.created = created != FALSE, .error = error};
}

[[nodiscard]] bool expect_directory_junction_creation(
    DirectoryJunctionCreation const& creation, char const* message) {
  if (creation.created) {
    return true;
  }
  std::cerr << "bundled catalog resource contract failed: " << message
            << " (directory junction GetLastError=" << creation.error << ")\n";
  return false;
}

class Fixture final {
 public:
  Fixture()
      : root_(std::filesystem::temp_directory_path() /
              (L"azzs-bundled-catalog-contract-" +
               std::to_wstring(::GetCurrentProcessId()))) {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_ / L"catalog", error);
    ready_ = !error;
  }

  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  Fixture(Fixture const&) = delete;
  Fixture& operator=(Fixture const&) = delete;

  [[nodiscard]] bool ready() const noexcept { return ready_; }
  [[nodiscard]] std::filesystem::path const& root() const noexcept {
    return root_;
  }
  [[nodiscard]] std::filesystem::path catalog_file() const {
    return root_ / L"catalog" / L"software-catalog.toml";
  }

 private:
  std::filesystem::path root_;
  bool ready_{false};
};

[[nodiscard]] bool missing_resource_is_rejected() {
  Fixture fixture;
  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto const read =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);
  return expect(fixture.ready(), "the controlled resource root must be created") &&
         expect(!read, "a missing bundled catalog resource must be rejected");
}

[[nodiscard]] bool digest_mismatch_is_rejected() {
  Fixture fixture;
  auto const written = write_file(fixture.catalog_file(), "abd");
  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto const read =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);
  return expect(fixture.ready() && written,
                "the digest mismatch fixture must be writable") &&
         expect(!read, "a digest mismatch must reject the resource bytes");
}

[[nodiscard]] bool byte_count_mismatch_is_rejected() {
  Fixture fixture;
  auto const written = write_file(fixture.catalog_file(), "abcx");
  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto const read =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);
  return expect(fixture.ready() && written,
                "the byte-count mismatch fixture must be writable") &&
         expect(!read,
                "a resource with bytes outside the locked length must be rejected");
}

[[nodiscard]] bool unsafe_relative_path_is_rejected() {
  Fixture fixture;
  auto const written = write_file(fixture.catalog_file(), "abc");
  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto const absolute =
      reader.read(fixture.catalog_file(), kAbcExpectation);
  auto const parent = reader.read(L"../outside.toml", kAbcExpectation);
  auto const dot =
      reader.read(L"catalog/./software-catalog.toml", kAbcExpectation);
  auto const empty = reader.read(std::filesystem::path{}, kAbcExpectation);

  return expect(fixture.ready() && written,
                "the unsafe path fixture must be writable") &&
         expect(!absolute, "an absolute resource path must be rejected") &&
         expect(!parent, "a parent traversal resource path must be rejected") &&
         expect(!dot, "a dot-segment resource path must be rejected") &&
         expect(!empty, "an empty resource path must be rejected");
}

[[nodiscard]] bool reparse_point_is_rejected() {
  Fixture fixture;
  auto const target = fixture.root() / L"outside.toml";
  auto const written = write_file(target, "abc");
  auto const link = fixture.catalog_file();
  auto const creation =
      create_symbolic_link(link, target, /*directory_flag=*/0);

  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto const read =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);
  bool passed = expect(fixture.ready() && written,
                       "the reparse fixture target must be writable") &&
                expect_symbolic_link_creation(
                    creation, "the reparse contract requires a file symbolic link");
  if (creation.created) {
    passed &= expect(!read,
                     "a bundled catalog reparse point must be rejected");
    passed &= expect(::DeleteFileW(link.c_str()) != FALSE,
                     "the file symbolic-link fixture must be removed");
  }
  return passed;
}

[[nodiscard]] bool nested_reparse_point_is_rejected() {
  Fixture fixture;
  auto const catalog = fixture.root() / L"catalog";
  auto const target = fixture.root() / L"alternate-catalog";
  std::error_code error;
  std::filesystem::remove_all(catalog, error);
  std::filesystem::create_directories(target, error);
  auto const written = !error &&
                       write_file(target / L"software-catalog.toml", "abc");
  auto const creation = create_directory_junction(catalog, target);

  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto const read =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);
  bool passed = expect(fixture.ready() && written,
                       "the nested reparse fixture target must be writable") &&
                expect_directory_junction_creation(
                    creation, "the reparse contract requires a directory junction");
  if (creation.created) {
    passed &= expect(!read,
                     "a reparse point in the catalog directory must be rejected");
    passed &= expect(::RemoveDirectoryW(catalog.c_str()) != FALSE,
                     "the directory junction fixture must be removed");
  }
  return passed;
}

[[nodiscard]] bool verified_snapshot_survives_path_replacement() {
  Fixture fixture;
  auto const path = fixture.catalog_file();
  auto const written = write_file(path, "abc");
  WindowsBundledCatalogResourceReader reader{fixture.root()};
  auto snapshot =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);

  std::error_code removal_error;
  std::filesystem::remove(path, removal_error);
  auto const replacement_written = !removal_error && write_file(path, "abd");
  auto const reopened =
      reader.read("catalog/software-catalog.toml", kAbcExpectation);
  auto catalog_reader = snapshot
                            ? LocalSoftwareCatalogFileReader::from_verified_built_in(
                                  snapshot.resource->bytes())
                            : LocalSoftwareCatalogFileReader{};
  auto const consumed = catalog_reader.read_built_in();

  return expect(fixture.ready() && written && snapshot,
                "the initial bundled catalog bytes must be accepted") &&
         expect(replacement_written,
                "the fixture must replace the path after snapshot validation") &&
         expect(snapshot.resource->bytes() == "abc",
                "the verified resource must retain owned bytes after replacement") &&
         expect(!reopened,
                "a later path read must reject replacement bytes") &&
         expect(consumed.succeeded && consumed.bytes == "abc",
                "the software catalog consumer must use the verified snapshot");
}

}  // namespace

int main() {
  bool passed = true;
  passed &= missing_resource_is_rejected();
  passed &= digest_mismatch_is_rejected();
  passed &= byte_count_mismatch_is_rejected();
  passed &= unsafe_relative_path_is_rejected();
  passed &= reparse_point_is_rejected();
  passed &= nested_reparse_point_is_rejected();
  passed &= verified_snapshot_survives_path_replacement();
  if (!passed) {
    return EXIT_FAILURE;
  }
  std::cout << "bundled catalog resource contract passed\n";
  return EXIT_SUCCESS;
}
