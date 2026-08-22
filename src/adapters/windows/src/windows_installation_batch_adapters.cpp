#include "azzs/adapters/windows/windows_installation_batch_adapters.hpp"
#include "azzs/adapters/windows/windows_controlled_acquisition.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <filesystem>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace azzs::adapters::windows {
namespace {

namespace batch = application::installation_batch;
namespace cache = application::offline_package_cache;
namespace cache_domain = domain::offline_package_cache;

enum class ControlledInstallerKind {
  exe,
  msi,
  archive,
};

struct ControlledInstallerRegistration final {
  std::string_view profile_id;
  std::string_view software_id;
  ControlledInstallerKind kind;
};

// This table is the complete project-owned launch surface.  No catalog text,
// cache filename, source URL, or user input can select an executable or add
// installer arguments.
constexpr std::array k_controlled_installer_registrations{
    ControlledInstallerRegistration{"qq-windows-v1", "qq", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"sogou-input-defaults-v1", "sogou-input", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"game-cheats-manager-windows-v1", "game-cheats-manager", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"office-tool-plus-windows-v1", "office-tool-plus", ControlledInstallerKind::archive},
    ControlledInstallerRegistration{"internet-download-manager-windows-v1", "internet-download-manager", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"java-runtime-windows-v1", "java-runtime", ControlledInstallerKind::msi},
    ControlledInstallerRegistration{"dotnet-runtime-windows-v1", "dotnet-runtime", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"directx-runtime-windows-v1", "directx-runtime", ControlledInstallerKind::exe},
    ControlledInstallerRegistration{"powershell-7-windows-v1", "powershell-7", ControlledInstallerKind::msi},
};

[[nodiscard]] std::optional<ControlledInstallerKind> controlled_installer_kind(
    domain::installation_batch::FrozenExecutionProfile const& profile,
    std::string_view software_id) noexcept {
  auto const found = std::ranges::find_if(
      k_controlled_installer_registrations,
      [&](ControlledInstallerRegistration const& registration) {
        return registration.profile_id == profile.profile_id &&
               registration.software_id == software_id;
      });
  if (found == k_controlled_installer_registrations.end()) {
    return std::nullopt;
  }
  return found->kind;
}

[[nodiscard]] bool profile_registered(
    domain::installation_batch::FrozenExecutionProfile const& profile,
    std::string_view software_id) {
  if (!profile.valid()) {
    return false;
  }
  auto const profiles = domain::software_catalog::initial_controlled_install_profiles();
  auto const found = std::ranges::find(profiles, profile.profile_id,
                                       &domain::software_catalog::ControlledInstallProfile::id);
  return found != profiles.end() && found->software_id == software_id &&
         found->execution_kind == profile.executor &&
         found->execution == profile.execution &&
         found->execution == domain::software_catalog::WindowsExecutionReadiness::project_executor_registered &&
         found->completion_boundary == profile.completion_boundary &&
         found->post_install_behavior == profile.post_install &&
         found->restart_verification == profile.restart &&
         found->result_detection == profile.result_detection &&
         found->interaction_scope == profile.interaction_scope &&
         std::ranges::find(found->baselines, profile.baseline) != found->baselines.end();
}

[[nodiscard]] bool profile_registered(
    domain::installation_batch::FrozenExecutionProfile const& profile) {
  auto const profiles = domain::software_catalog::initial_controlled_install_profiles();
  auto const found = std::ranges::find(profiles, profile.profile_id,
                                       &domain::software_catalog::ControlledInstallProfile::id);
  return found != profiles.end() && profile_registered(profile, found->software_id);
}

[[nodiscard]] bool has_msi_signature(std::span<std::byte const> bytes) noexcept {
  static constexpr std::array<std::byte, 8> k_signature{
      std::byte{0xD0}, std::byte{0xCF}, std::byte{0x11}, std::byte{0xE0},
      std::byte{0xA1}, std::byte{0xB1}, std::byte{0x1A}, std::byte{0xE1}};
  return bytes.size() >= k_signature.size() &&
         std::ranges::equal(bytes.first(k_signature.size()), k_signature);
}

[[nodiscard]] std::optional<std::uint16_t> pe_machine(
    std::span<std::byte const> bytes) noexcept {
  if (bytes.size() < 64U || bytes[0] != std::byte{'M'} ||
      bytes[1] != std::byte{'Z'}) {
    return std::nullopt;
  }
  auto read32 = [&](std::size_t offset) -> std::optional<std::uint32_t> {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1U])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3U])) << 24U);
  };
  auto const pe_offset = read32(0x3cU);
  if (!pe_offset.has_value() || *pe_offset > bytes.size() ||
      bytes.size() - *pe_offset < 6U ||
      bytes[*pe_offset] != std::byte{'P'} ||
      bytes[*pe_offset + 1U] != std::byte{'E'} ||
      bytes[*pe_offset + 2U] != std::byte{0} ||
      bytes[*pe_offset + 3U] != std::byte{0}) {
    return std::nullopt;
  }
  auto const offset = static_cast<std::size_t>(*pe_offset) + 4U;
  return static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1U]) << 8U);
}

class DeflateBitReader final {
 public:
  explicit DeflateBitReader(std::span<std::byte const> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool read(std::uint32_t count, std::uint32_t& value) noexcept {
    if (count > 24U || bit_offset_ > bytes_.size() * 8U ||
        count > bytes_.size() * 8U - bit_offset_) {
      return false;
    }
    value = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
      auto const absolute = bit_offset_ + index;
      auto const byte = std::to_integer<unsigned char>(bytes_[absolute / 8U]);
      value |= static_cast<std::uint32_t>((byte >> (absolute % 8U)) & 1U) << index;
    }
    bit_offset_ += count;
    return true;
  }

  void align_byte() noexcept { bit_offset_ = (bit_offset_ + 7U) & ~std::size_t{7U}; }

  [[nodiscard]] std::size_t bit_offset() const noexcept { return bit_offset_; }

  [[nodiscard]] std::size_t remaining_bits() const noexcept {
    const auto total_bits = bytes_.size() * 8U;
    return bit_offset_ > total_bits ? 0U : total_bits - bit_offset_;
  }

  [[nodiscard]] bool remaining_bits_are_zero() const noexcept {
    auto copy = *this;
    std::uint32_t value{};
    while (copy.remaining_bits() != 0U) {
      const auto count = static_cast<std::uint32_t>(
          std::min<std::size_t>(copy.remaining_bits(), 24U));
      if (!copy.read(count, value) || value != 0U) {
        return false;
      }
    }
    return true;
  }

 private:
  std::span<std::byte const> bytes_;
  std::size_t bit_offset_{};
};

struct DeflateHuffman final {
  struct Code final {
    std::uint16_t value{};
    std::uint8_t length{};
    std::uint16_t symbol{};
  };
  std::array<Code, 320> codes{};
  std::size_t count{};
  std::uint8_t max_length{};

  [[nodiscard]] bool build(std::span<std::uint8_t const> lengths,
                           bool allow_empty = false,
                           bool allow_single_incomplete = false) noexcept {
    std::array<std::uint16_t, 16> frequencies{};
    std::size_t nonzero{};
    for (auto length : lengths) {
      if (length > 15U) {
        return false;
      }
      if (length != 0U) {
        ++frequencies[length];
        max_length = std::max(max_length, length);
        ++nonzero;
      }
    }
    if (max_length == 0U) {
      return allow_empty;
    }
    // RFC 1951 code-space accounting catches both malformed
    // oversubscription and incomplete trees. The decoder only accepts the
    // one-bit, one-symbol exception where that form is permitted.
    int left = 1;
    for (std::uint8_t length = 1U; length <= 15U; ++length) {
      left = (left << 1) - frequencies[length];
      if (left < 0) {
        return false;
      }
    }
    if (left != 0 &&
        !(allow_single_incomplete && nonzero == 1U &&
          frequencies[1U] == 1U)) {
      return false;
    }
    std::array<std::uint16_t, 16> next{};
    std::uint16_t code = 0;
    for (std::uint8_t length = 1; length <= 15U; ++length) {
      code = static_cast<std::uint16_t>((code + frequencies[length - 1U]) << 1U);
      next[length] = code;
    }
    count = 0;
    for (std::uint16_t symbol = 0; symbol < lengths.size(); ++symbol) {
      auto const length = lengths[symbol];
      if (length == 0U) {
        continue;
      }
      if (count >= codes.size()) {
        return false;
      }
      auto canonical = next[length]++;
      std::uint16_t reversed = 0;
      for (std::uint8_t bit = 0; bit < length; ++bit) {
        reversed = static_cast<std::uint16_t>((reversed << 1U) | (canonical & 1U));
        canonical = static_cast<std::uint16_t>(canonical >> 1U);
      }
      codes[count++] = Code{reversed, length, symbol};
    }
    return count != 0U;
  }

  [[nodiscard]] bool contains(std::uint16_t symbol) const noexcept {
    return std::ranges::any_of(codes.begin(), codes.begin() + count,
                               [symbol](Code const& code) {
                                 return code.symbol == symbol;
                               });
  }

  [[nodiscard]] bool decode(DeflateBitReader& reader, std::uint16_t& symbol) const noexcept {
    std::uint16_t bits = 0;
    for (std::uint8_t length = 1; length <= max_length; ++length) {
      std::uint32_t bit{};
      if (!reader.read(1U, bit)) {
        return false;
      }
      bits = static_cast<std::uint16_t>(bits | (bit << (length - 1U)));
      for (std::size_t index = 0; index < count; ++index) {
        auto const& candidate = codes[index];
        if (candidate.length == length && candidate.value == bits) {
          symbol = candidate.symbol;
          return true;
        }
      }
    }
    return false;
  }
};

[[nodiscard]] bool inflate_raw_deflate(
    std::span<std::byte const> compressed, std::uint64_t expected_size,
    std::vector<std::byte>& output) {
  constexpr std::array<std::uint16_t, 29> kLengthBase{
      3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
  constexpr std::array<std::uint8_t, 29> kLengthExtra{
      0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  constexpr std::array<std::uint16_t, 30> kDistanceBase{
      1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
      6145, 8193, 12289, 16385, 24577};
  constexpr std::array<std::uint8_t, 30> kDistanceExtra{
      0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
      6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

  if (expected_size > 512ULL * 1024ULL * 1024ULL) {
    return false;
  }
  DeflateBitReader reader{compressed};
  output.clear();
  output.reserve(static_cast<std::size_t>(expected_size));
  bool final = false;
  while (!final) {
    std::uint32_t value{};
    if (!reader.read(1U, value)) {
      return false;
    }
    final = value != 0U;
    if (!reader.read(2U, value)) {
      return false;
    }
    auto const block_type = value;
    if (block_type == 0U) {
      reader.align_byte();
      std::uint32_t length{}, complement{};
      if (!reader.read(16U, length) || !reader.read(16U, complement) ||
          (length ^ 0xffffU) != complement ||
          length > expected_size - std::min<std::uint64_t>(expected_size, output.size())) {
        return false;
      }
      for (std::uint32_t index = 0; index < length; ++index) {
        if (!reader.read(8U, value)) {
          return false;
        }
        output.push_back(std::byte{static_cast<unsigned char>(value)});
      }
      continue;
    }
    if (block_type != 1U && block_type != 2U) {
      return false;
    }

    std::array<std::uint8_t, 288> literal_lengths{};
    std::array<std::uint8_t, 32> distance_lengths{};
    if (block_type == 1U) {
      for (std::size_t index = 0; index <= 143U; ++index) literal_lengths[index] = 8;
      for (std::size_t index = 144U; index <= 255U; ++index) literal_lengths[index] = 9;
      for (std::size_t index = 256U; index <= 279U; ++index) literal_lengths[index] = 7;
      for (std::size_t index = 280U; index < literal_lengths.size(); ++index) literal_lengths[index] = 8;
      distance_lengths.fill(5);
    } else {
      std::uint32_t hlit{}, hdist{}, hclen{};
      if (!reader.read(5U, hlit) || !reader.read(5U, hdist) || !reader.read(4U, hclen)) return false;
      hlit += 257U; hdist += 1U; hclen += 4U;
      if (hlit > literal_lengths.size() || hdist > distance_lengths.size()) return false;
      constexpr std::array<std::uint8_t, 19> order{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      std::array<std::uint8_t, 19> code_lengths{};
      for (std::uint32_t index = 0; index < hclen; ++index) {
        if (!reader.read(3U, value)) return false;
        code_lengths[order[index]] = static_cast<std::uint8_t>(value);
      }
      DeflateHuffman code_tree;
      if (!code_tree.build(code_lengths, false, true)) return false;
      std::array<std::uint8_t, 320> all_lengths{};
      auto const total = hlit + hdist;
      std::size_t index = 0;
      while (index < total) {
        std::uint16_t symbol{};
        if (!code_tree.decode(reader, symbol)) return false;
        if (symbol <= 15U) {
          all_lengths[index++] = static_cast<std::uint8_t>(symbol);
        } else if (symbol == 16U) {
          if (index == 0U) return false;
          if (!reader.read(2U, value)) return false;
          auto const repeat = value + 3U;
          if (index + repeat > total) return false;
          std::fill_n(all_lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat, all_lengths[index - 1U]);
          index += repeat;
        } else if (symbol == 17U || symbol == 18U) {
          auto const extra = symbol == 17U ? 3U : 7U;
          auto const base = symbol == 17U ? 3U : 11U;
          if (!reader.read(extra, value)) return false;
          auto const repeat = value + base;
          if (index + repeat > total) return false;
          std::fill_n(all_lengths.begin() + static_cast<std::ptrdiff_t>(index), repeat, 0U);
          index += repeat;
        } else {
          return false;
        }
      }
      std::copy_n(all_lengths.begin(), hlit, literal_lengths.begin());
      std::copy_n(all_lengths.begin() + static_cast<std::ptrdiff_t>(hlit), hdist, distance_lengths.begin());
      if (literal_lengths[286U] != 0U || literal_lengths[287U] != 0U ||
          distance_lengths[30U] != 0U || distance_lengths[31U] != 0U) {
        return false;
      }
    }
    DeflateHuffman literal_tree;
    DeflateHuffman distance_tree;
    if (!literal_tree.build(literal_lengths, false, true) ||
        !literal_tree.contains(256U) ||
        !distance_tree.build(distance_lengths, true, true)) {
      return false;
    }
    for (;;) {
      std::uint16_t symbol{};
      if (!literal_tree.decode(reader, symbol)) return false;
      if (symbol < 256U) {
        if (output.size() >= expected_size) return false;
        output.push_back(std::byte{static_cast<unsigned char>(symbol)});
      } else if (symbol == 256U) {
        break;
      } else if (symbol >= 257U && symbol <= 285U) {
        auto const length_index = symbol - 257U;
        std::uint32_t extra{};
        if (!reader.read(kLengthExtra[length_index], extra)) return false;
        auto const length = static_cast<std::size_t>(kLengthBase[length_index] + extra);
        std::uint16_t distance_symbol{};
        if (!distance_tree.decode(reader, distance_symbol) || distance_symbol >= 30U) return false;
        if (!reader.read(kDistanceExtra[distance_symbol], extra)) return false;
        auto const distance = static_cast<std::size_t>(kDistanceBase[distance_symbol] + extra);
        if (distance == 0U || distance > output.size() || length > expected_size - output.size()) return false;
        auto const start = output.size() - distance;
        for (std::size_t index = 0; index < length; ++index) output.push_back(output[start + (index % distance)]);
      } else {
        return false;
      }
    }
  }
  // A raw Deflate member may end in at most seven zero padding bits.  Any
  // additional byte or non-zero tail would otherwise permit hidden data after
  // the authenticated stream.
  return output.size() == expected_size && reader.remaining_bits() <= 7U &&
         reader.remaining_bits_are_zero();
}

[[nodiscard]] std::uint32_t crc32(std::span<std::byte const> bytes) noexcept {
  std::uint32_t crc = 0xffffffffU;
  for (auto byte : bytes) {
    crc ^= std::to_integer<unsigned char>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

[[nodiscard]] std::optional<std::string> sha256_hex(
    std::span<std::byte const> bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(
          &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return std::nullopt;
  }
  DWORD object_length{};
  DWORD result_length{};
  if (!BCRYPT_SUCCESS(::BCryptGetProperty(
          algorithm, BCRYPT_OBJECT_LENGTH,
          reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
          &result_length, 0)) || object_length == 0U) {
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return std::nullopt;
  }
  std::vector<std::byte> object(object_length);
  BCRYPT_HASH_HANDLE hash{};
  if (!BCRYPT_SUCCESS(::BCryptCreateHash(
          algorithm, &hash, reinterpret_cast<PUCHAR>(object.data()), object_length,
          nullptr, 0, 0))) {
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    return std::nullopt;
  }
  auto const update = bytes.empty() ||
                      BCRYPT_SUCCESS(::BCryptHashData(
                          hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
                          static_cast<ULONG>(bytes.size()), 0));
  std::array<std::byte, 32> digest{};
  auto const finish = update && BCRYPT_SUCCESS(::BCryptFinishHash(
                                      hash, reinterpret_cast<PUCHAR>(digest.data()),
                                      static_cast<ULONG>(digest.size()), 0));
  ::BCryptDestroyHash(hash);
  ::BCryptCloseAlgorithmProvider(algorithm, 0);
  if (!finish) return std::nullopt;
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(64U);
  for (auto byte : digest) {
    auto const value = std::to_integer<unsigned char>(byte);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

[[nodiscard]] std::optional<std::string> normalize_sha256(
    std::string_view value) {
  if (value.size() != 64U) {
    return std::nullopt;
  }
  std::string normalized;
  normalized.reserve(value.size());
  for (unsigned char character : value) {
    if (character >= '0' && character <= '9') {
      normalized.push_back(static_cast<char>(character));
    } else if (character >= 'a' && character <= 'f') {
      normalized.push_back(static_cast<char>(character));
    } else if (character >= 'A' && character <= 'F') {
      normalized.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      return std::nullopt;
    }
  }
  return normalized;
}

struct ParsedArchiveMember final {
  std::string path;
  std::vector<std::byte> bytes;
  std::uint16_t method{};
  std::uint64_t compressed_size{};
  std::uint64_t uncompressed_size{};
  std::uint16_t machine{};
};

struct ParsedArchive final {
  WindowsArchiveValidationCode code{WindowsArchiveValidationCode::malformed};
  std::vector<ParsedArchiveMember> members;
  std::string detail;
};

[[nodiscard]] std::optional<std::uint16_t> archive_u16(
    std::span<std::byte const> bytes, std::size_t offset) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < 2U) return std::nullopt;
  return static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset])) |
         static_cast<std::uint16_t>(std::to_integer<unsigned char>(bytes[offset + 1U]) << 8U);
}

[[nodiscard]] std::optional<std::uint32_t> archive_u32(
    std::span<std::byte const> bytes, std::size_t offset) noexcept {
  if (offset > bytes.size() || bytes.size() - offset < 4U) return std::nullopt;
  return static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset])) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 1U])) << 8U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 2U])) << 16U) |
         (static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + 3U])) << 24U);
}

[[nodiscard]] bool safe_archive_path(std::string_view path) noexcept {
  if (path.empty() || path.front() == '/' || path.front() == '\\' ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  if (path.back() == '/' &&
      (path.size() == 1U || path[path.size() - 2U] == '/')) {
    return false;
  }
  // A single trailing slash is the ZIP directory marker. Empty segments
  // anywhere else would make separator normalization ambiguous.
  auto const segment_end = path.back() == '/' ? path.size() - 1U : path.size();
  if (segment_end == 0U) {
    return false;
  }
  std::size_t start = 0U;
  while (start < segment_end) {
    auto const end = path.find('/', start);
    auto const bounded_end = end == std::string_view::npos
                                 ? segment_end
                                 : std::min(end, segment_end);
    auto const segment = path.substr(start, bounded_end - start);
    if (segment.empty() || segment == "." || segment == ".." ||
        segment.find(':') != std::string_view::npos) {
      return false;
    }
    if (end == std::string_view::npos || end >= segment_end) {
      break;
    }
    start = end + 1U;
  }
  return true;
}

[[nodiscard]] std::uint16_t expected_machine(
    cache_domain::CacheArchitecture architecture) noexcept {
  switch (architecture) {
    case cache_domain::CacheArchitecture::x64: return 0x8664U;
    case cache_domain::CacheArchitecture::x86: return 0x014cU;
    case cache_domain::CacheArchitecture::arm64: return 0xaa64U;
    case cache_domain::CacheArchitecture::architecture_independent:
    case cache_domain::CacheArchitecture::unknown: return 0U;
  }
  return 0U;
}

[[nodiscard]] ParsedArchive parse_controlled_archive_impl(
    std::span<std::byte const> bytes, std::span<std::string const> allowed,
    cache_domain::CacheArchitecture architecture) {
  constexpr std::uint16_t kMaximumMembers = 4096U;
  constexpr std::uint64_t kMaximumMemberBytes = 512ULL * 1024ULL * 1024ULL;
  constexpr std::uint64_t kMaximumUncompressedBytes =
      512ULL * 1024ULL * 1024ULL;
  ParsedArchive result;
  if (allowed.empty()) {
    result.code = WindowsArchiveValidationCode::member_mismatch;
    result.detail = "controlled archive whitelist is empty";
    return result;
  }
  if (allowed.size() > kMaximumMembers) {
    result.code = WindowsArchiveValidationCode::member_mismatch;
    result.detail = "controlled archive whitelist exceeds the member budget";
    return result;
  }
  std::vector<bool> matched(allowed.size(), false);
  for (std::size_t index = 0U; index < allowed.size(); ++index) {
    if (!safe_archive_path(allowed[index]) || allowed[index].back() == '/' ||
        std::ranges::find(allowed.begin(), allowed.begin() + index,
                          allowed[index]) != allowed.begin() + index) {
      result.code = WindowsArchiveValidationCode::member_mismatch;
      result.detail =
          "controlled archive whitelist contains an unsafe or duplicate member";
      return result;
    }
  }
  if (bytes.size() < 22U) {
    result.detail = "controlled archive is shorter than the ZIP end record";
    return result;
  }
  auto const search_begin = bytes.size() > 22U + 65535U ? bytes.size() - 22U - 65535U : 0U;
  std::optional<std::size_t> eocd;
  for (std::size_t offset = bytes.size() - 22U;; --offset) {
    if (archive_u32(bytes, offset) == 0x06054b50U) { eocd = offset; break; }
    if (offset == search_begin) break;
  }
  if (!eocd.has_value()) { result.detail = "ZIP end record is missing"; return result; }
  auto disk = archive_u16(bytes, *eocd + 4U);
  auto cd_disk = archive_u16(bytes, *eocd + 6U);
  auto disk_count = archive_u16(bytes, *eocd + 8U);
  auto total_count = archive_u16(bytes, *eocd + 10U);
  auto cd_size = archive_u32(bytes, *eocd + 12U);
  auto cd_offset = archive_u32(bytes, *eocd + 16U);
  auto comment = archive_u16(bytes, *eocd + 20U);
  if (!disk.has_value() || !cd_disk.has_value() || !disk_count.has_value() ||
      !total_count.has_value() || !cd_size.has_value() ||
      !cd_offset.has_value() || !comment.has_value() ||
      *disk != 0U || *cd_disk != 0U || *disk_count != *total_count ||
      *comment != bytes.size() - (*eocd + 22U) || *total_count == 0U ||
      *total_count == 0xffffU || *total_count > kMaximumMembers ||
      *cd_size == 0xffffffffU || *cd_offset == 0xffffffffU ||
      *cd_offset > bytes.size() || *cd_size > bytes.size() - *cd_offset ||
      *eocd != static_cast<std::size_t>(*cd_offset) + *cd_size) {
    result.detail = "ZIP end record has unsupported or out-of-range fields";
    return result;
  }
  std::size_t cursor = *cd_offset;
  std::uint64_t total_uncompressed{};
  std::vector<std::string> names;
  names.reserve(*total_count);
  std::vector<std::pair<std::size_t, std::size_t>> local_ranges;
  local_ranges.reserve(*total_count);
  result.members.reserve(allowed.size());
  for (std::uint16_t entry_index = 0U; entry_index < *total_count;
       ++entry_index) {
    if (cursor > bytes.size() || bytes.size() - cursor < 46U ||
        archive_u32(bytes, cursor) != 0x02014b50U) {
      result.detail = "ZIP central directory entry is malformed"; return result;
    }
    auto flags = archive_u16(bytes, cursor + 8U);
    auto method = archive_u16(bytes, cursor + 10U);
    auto disk_start = archive_u16(bytes, cursor + 34U);
    auto expected_crc = archive_u32(bytes, cursor + 16U);
    auto compressed_size = archive_u32(bytes, cursor + 20U);
    auto uncompressed_size = archive_u32(bytes, cursor + 24U);
    auto name_size = archive_u16(bytes, cursor + 28U);
    auto extra_size = archive_u16(bytes, cursor + 30U);
    auto comment_size = archive_u16(bytes, cursor + 32U);
    auto local_offset = archive_u32(bytes, cursor + 42U);
    if (!flags.has_value() || !method.has_value() || !disk_start.has_value() ||
        !expected_crc.has_value() || !compressed_size.has_value() ||
        !uncompressed_size.has_value() || !name_size.has_value() ||
        !extra_size.has_value() || !comment_size.has_value() ||
        !local_offset.has_value() || ((*flags & ~0x0800U) != 0U) ||
        *disk_start != 0U ||
        (*method != 0U && *method != 8U) ||
        *compressed_size == 0xffffffffU || *uncompressed_size == 0xffffffffU ||
        *local_offset == 0xffffffffU ||
        *compressed_size > kMaximumMemberBytes ||
        *uncompressed_size > kMaximumMemberBytes || *name_size == 0U ||
        *name_size > bytes.size() - (cursor + 46U) ||
        *extra_size > bytes.size() - (cursor + 46U + *name_size) ||
        *comment_size > bytes.size() - (cursor + 46U + *name_size + *extra_size)) {
      result.detail = "ZIP entry uses unsupported flags, method, or size"; return result;
    }
    auto const name_start = cursor + 46U;
    std::string name(reinterpret_cast<char const*>(bytes.data() + name_start), *name_size);
    auto const entry_end = name_start + *name_size + *extra_size + *comment_size;
    if (entry_end > static_cast<std::size_t>(*eocd) || !safe_archive_path(name) ||
        std::ranges::find(names, name) != names.end()) {
      result.detail = "ZIP central directory contains an unsafe or duplicate member";
      return result;
    }
    names.push_back(name);
    auto const is_directory = !name.empty() && name.back() == '/';
    auto allowed_it = std::ranges::find(allowed, name);
    auto const is_allowed = allowed_it != allowed.end();
    if (is_directory && is_allowed) {
      result.code = WindowsArchiveValidationCode::member_mismatch;
      result.detail = "ZIP directory marker cannot be a whitelist member";
      return result;
    }
    if (!is_directory && !is_allowed) {
      result.code = WindowsArchiveValidationCode::member_mismatch;
      result.detail = "ZIP central directory contains an unreviewed file";
      return result;
    }
    if (is_allowed && matched[static_cast<std::size_t>(allowed_it - allowed.begin())]) {
      result.code = WindowsArchiveValidationCode::member_mismatch;
      result.detail = "ZIP whitelist member is duplicated";
      return result;
    }
    if (is_directory && (*method != 0U || *compressed_size != 0U ||
                         *uncompressed_size != 0U)) {
      result.detail = "ZIP directory marker carries file data";
      return result;
    }
    if (*method == 0U && *compressed_size != *uncompressed_size) {
      result.detail = "stored ZIP member has inconsistent sizes";
      return result;
    }
    if (total_uncompressed > kMaximumUncompressedBytes - *uncompressed_size) {
      result.detail = "ZIP total uncompressed size exceeds the safety budget";
      return result;
    }
    total_uncompressed += *uncompressed_size;
    if (*local_offset >= *cd_offset || *local_offset > bytes.size() ||
        bytes.size() - *local_offset < 30U ||
        archive_u32(bytes, *local_offset) != 0x04034b50U) {
      result.detail = "ZIP local file header is malformed"; return result;
    }
    auto local_flags = archive_u16(bytes, *local_offset + 6U);
    auto local_method = archive_u16(bytes, *local_offset + 8U);
    auto local_crc = archive_u32(bytes, *local_offset + 14U);
    auto local_compressed_size = archive_u32(bytes, *local_offset + 18U);
    auto local_uncompressed_size = archive_u32(bytes, *local_offset + 22U);
    auto local_name_size = archive_u16(bytes, *local_offset + 26U);
    auto local_extra_size = archive_u16(bytes, *local_offset + 28U);
    if (!local_flags.has_value() || !local_method.has_value() ||
        !local_crc.has_value() || !local_compressed_size.has_value() ||
        !local_uncompressed_size.has_value() || !local_name_size.has_value() ||
        !local_extra_size.has_value() || *local_flags != *flags ||
        *local_method != *method || *local_crc != *expected_crc ||
        *local_compressed_size != *compressed_size ||
        *local_uncompressed_size != *uncompressed_size ||
        *local_name_size != *name_size ||
        *local_extra_size != *extra_size ||
        *local_name_size > bytes.size() - (*local_offset + 30U) ||
        *local_extra_size > bytes.size() - (*local_offset + 30U + *local_name_size)) {
      result.detail = "ZIP local header does not match its central entry"; return result;
    }
    auto const local_name_start = static_cast<std::size_t>(*local_offset) + 30U;
    if (!std::ranges::equal(bytes.subspan(name_start, *name_size),
                            bytes.subspan(local_name_start, *name_size))) {
      result.detail = "ZIP local header filename differs from its central entry";
      return result;
    }
    auto const data_start = local_name_start + *local_name_size + *local_extra_size;
    if (data_start > *cd_offset || *compressed_size > bytes.size() - data_start ||
        *compressed_size > *cd_offset - data_start) {
      result.detail = "ZIP member data exceeds the local or central directory boundary";
      return result;
    }
    auto const data_end = data_start + *compressed_size;
    for (auto const& range : local_ranges) {
      if (static_cast<std::size_t>(*local_offset) < range.second &&
          range.first < data_end) {
        result.detail = "ZIP local member ranges overlap";
        return result;
      }
    }
    local_ranges.emplace_back(static_cast<std::size_t>(*local_offset), data_end);

    if (is_directory) {
      if (*expected_crc != 0U) {
        result.detail = "ZIP directory marker has a non-empty CRC";
        return result;
      }
    } else {
      std::vector<std::byte> member;
      auto const compressed = bytes.subspan(data_start, *compressed_size);
      if (*method == 0U) {
        member.assign(compressed.begin(), compressed.end());
      } else if (!inflate_raw_deflate(compressed, *uncompressed_size, member)) {
        result.detail = "ZIP Deflate member could not be decoded";
        return result;
      }
      if (member.size() != *uncompressed_size || crc32(member) != *expected_crc) {
        result.detail = "ZIP member CRC or size verification failed";
        return result;
      }
      if (is_allowed) {
        matched[static_cast<std::size_t>(allowed_it - allowed.begin())] = true;
        auto const machine = pe_machine(member);
        if (!machine.has_value() || expected_machine(architecture) == 0U ||
            *machine != expected_machine(architecture)) {
          result.code = WindowsArchiveValidationCode::architecture_mismatch;
          result.detail =
              "ZIP member is not a PE image for the selected architecture";
          return result;
        }
        result.members.push_back(ParsedArchiveMember{
            std::move(name), std::move(member), *method, *compressed_size,
            *uncompressed_size, *machine});
      }
    }
    cursor = entry_end;
  }
  if (cursor != static_cast<std::size_t>(*cd_offset) + *cd_size ||
      std::ranges::any_of(matched, [](bool value) { return !value; })) {
    result.code = WindowsArchiveValidationCode::member_mismatch;
    result.detail = "ZIP central directory contains missing or trailing members";
    return result;
  }
  result.code = WindowsArchiveValidationCode::valid;
  result.detail = "controlled ZIP archive passed structural, member, CRC, and PE checks";
  return result;
}

[[nodiscard]] ParsedArchive parse_controlled_archive(
    std::span<std::byte const> bytes, std::span<std::string const> allowed,
    cache_domain::CacheArchitecture architecture) noexcept {
  try {
    return parse_controlled_archive_impl(bytes, allowed, architecture);
  } catch (...) {
    ParsedArchive result;
    result.code = WindowsArchiveValidationCode::malformed;
    result.detail = "controlled archive validation failed closed";
    return result;
  }
}

[[nodiscard]] std::optional<std::filesystem::path> controlled_payload_path(
    std::span<std::byte const> bytes,
    std::wstring_view extension) {
  wchar_t temp_buffer[MAX_PATH]{};
  auto const length = ::GetTempPathW(static_cast<DWORD>(std::size(temp_buffer)),
                                     temp_buffer);
  if (length == 0 || length >= std::size(temp_buffer)) {
    return std::nullopt;
  }
  auto const root = std::filesystem::path{temp_buffer} / L"azzs-controlled-install";
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return std::nullopt;
  }
  auto const directory_attributes = ::GetFileAttributesW(root.c_str());
  if (directory_attributes == INVALID_FILE_ATTRIBUTES ||
      (directory_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (directory_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return std::nullopt;
  }

  GUID guid{};
  if (FAILED(::CoCreateGuid(&guid))) {
    return std::nullopt;
  }
  wchar_t guid_text[64]{};
  if (::StringFromGUID2(guid, guid_text, static_cast<int>(std::size(guid_text))) <= 0) {
    return std::nullopt;
  }
  auto const operation_directory = root / guid_text;
  if (!std::filesystem::create_directory(operation_directory, error) || error) {
    return std::nullopt;
  }
  auto const operation_attributes =
      ::GetFileAttributesW(operation_directory.c_str());
  if (operation_attributes == INVALID_FILE_ATTRIBUTES ||
      (operation_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (operation_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    std::filesystem::remove(operation_directory, error);
    return std::nullopt;
  }
  auto const path = operation_directory /
                    (std::wstring{guid_text} + std::wstring{extension});
  auto const native = path.wstring();
  HANDLE file = ::CreateFileW(
      native.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    std::filesystem::remove(operation_directory, error);
    return std::nullopt;
  }
  bool written = true;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    auto const remaining = bytes.size() - offset;
    auto const chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, MAXDWORD));
    DWORD written_bytes{};
    if (!::WriteFile(file, bytes.data() + offset, chunk, &written_bytes, nullptr) ||
        written_bytes != chunk) {
      written = false;
      break;
    }
    offset += written_bytes;
  }
  if (written && !::FlushFileBuffers(file)) {
    written = false;
  }
  ::CloseHandle(file);
  if (!written) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    std::filesystem::remove(operation_directory, remove_error);
    return std::nullopt;
  }
  return path;
}

[[nodiscard]] std::wstring quote_argument(std::wstring_view value) {
  std::wstring quoted;
  quoted.reserve(value.size() + 2U);
  quoted.push_back(L'"');
  quoted.append(value);
  quoted.push_back(L'"');
  return quoted;
}

[[nodiscard]] std::optional<std::wstring> system_msiexec_path() {
  wchar_t directory[MAX_PATH]{};
  auto const length = ::GetSystemDirectoryW(directory, static_cast<UINT>(std::size(directory)));
  if (length == 0 || length >= std::size(directory)) {
    return std::nullopt;
  }
  return std::wstring{directory, length} + L"\\msiexec.exe";
}

[[nodiscard]] bool valid_verification_phase(
    batch::InstallVerificationPhase phase) noexcept {
  switch (phase) {
    case batch::InstallVerificationPhase::before_launch:
    case batch::InstallVerificationPhase::after_process_exit:
    case batch::InstallVerificationPhase::recovery_read_only:
    case batch::InstallVerificationPhase::after_restart_read_only:
      return true;
  }
  return false;
}

[[nodiscard]] batch::InstallationDownloadObservation download_failure(
    std::string detail) {
  return {.code = batch::InstallationDownloadCode::failed,
          .detail = std::move(detail)};
}

[[nodiscard]] bool cached_asset_is_present(
    cache::OfflinePackageCacheService& service,
    domain::offline_package_cache::CacheAssetIdentity const& identity) {
  auto const snapshot = service.snapshot();
  return std::ranges::any_of(snapshot.items, [&identity](auto const& item) {
    return item.asset.identity == identity && item.cache_present;
  });
}

[[nodiscard]] bool frozen_cache_root_is_selected(
    cache::OfflinePackageCacheService& service,
    domain::offline_package_cache::ControlledCacheRoot const& root) {
  return service.snapshot().selected_root == root;
}

[[nodiscard]] char const* stage_for(
    batch::InstallationFactKind const kind) noexcept {
  switch (kind) {
    case batch::InstallationFactKind::batch_created:
      return "batch-created";
    case batch::InstallationFactKind::state_persisted:
      return "state-persisted";
    case batch::InstallationFactKind::launch_requested:
      return "launch-requested";
    case batch::InstallationFactKind::verification_observed:
      return "verification-observed";
    case batch::InstallationFactKind::batch_paused:
      return "batch-paused";
    case batch::InstallationFactKind::download_paused:
      return "download-paused";
    case batch::InstallationFactKind::download_resumed:
      return "download-resumed";
    case batch::InstallationFactKind::normal_stop_requested:
      return "normal-stop-requested";
    case batch::InstallationFactKind::forced_termination_confirmation_requested:
      return "forced-termination-confirmation-requested";
    case batch::InstallationFactKind::forced_termination_confirmation_cancelled:
      return "forced-termination-confirmation-cancelled";
    case batch::InstallationFactKind::forced_termination_observed:
      return "forced-termination-observed";
    case batch::InstallationFactKind::normal_close_requested:
      return "normal-close-requested";
    case batch::InstallationFactKind::recovery_continued:
      return "recovery-continued";
    case batch::InstallationFactKind::recovery_observed:
      return "recovery-observed";
    case batch::InstallationFactKind::coverage_gap:
      return "coverage-gap";
  }
  return "unknown";
}

}  // namespace

bool windows_controlled_pe_machine_matches(
    domain::offline_package_cache::CacheArchitecture expected_architecture,
    std::uint16_t actual_machine) noexcept {
  switch (expected_architecture) {
    case domain::offline_package_cache::CacheArchitecture::x64:
      return actual_machine == 0x8664U;
    case domain::offline_package_cache::CacheArchitecture::x86:
      return actual_machine == 0x014cU;
    case domain::offline_package_cache::CacheArchitecture::arm64:
      return actual_machine == 0xaa64U;
    case domain::offline_package_cache::CacheArchitecture::architecture_independent:
      // The package may contain either native Windows width, but only the
      // reviewed PE machine set is accepted.  This is not an arbitrary
      // machine wildcard and deliberately excludes ARM32/unknown values.
      return actual_machine == 0x014cU || actual_machine == 0x8664U ||
             actual_machine == 0xaa64U;
    case domain::offline_package_cache::CacheArchitecture::unknown:
      return false;
  }
  return false;
}

WindowsArchiveInspection inspect_windows_controlled_archive(
    std::span<std::byte const> archive,
    std::span<std::string const> allowed_members,
    domain::offline_package_cache::CacheArchitecture expected_architecture) {
  auto parsed = parse_controlled_archive(archive, allowed_members,
                                         expected_architecture);
  WindowsArchiveInspection inspection{.code = parsed.code,
                                      .detail = std::move(parsed.detail)};
  for (auto const& member : parsed.members) {
    inspection.members.push_back({.path = member.path,
                                  .compression_method = member.method,
                                  .compressed_size = member.compressed_size,
                                  .uncompressed_size = member.uncompressed_size,
                                  .pe_machine = member.machine});
  }
  return inspection;
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::advance(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return {.code = batch::InstallationDownloadCode::source_invalid,
            .detail = "the frozen batch cache root is unavailable"};
  }

  auto const result = cache_.download(target.cache_asset.identity);
  switch (result.code) {
    case cache::CacheActionCode::completed:
    case cache::CacheActionCode::already_available:
      return {.code = batch::InstallationDownloadCode::cached_ready,
              .detail = "the controlled batch cache has a completed artifact"};
    case cache::CacheActionCode::paused:
      return {.code = batch::InstallationDownloadCode::paused,
              .detail = "the controlled batch cache transfer is paused"};
    case cache::CacheActionCode::waiting_for_network:
      return {.code = batch::InstallationDownloadCode::waiting_network,
              .detail = "the controlled batch cache requires a network"};
    case cache::CacheActionCode::unsupported:
    case cache::CacheActionCode::asset_not_current:
      return {.code = batch::InstallationDownloadCode::source_invalid,
              .detail = "the frozen batch asset is unavailable from its controlled cache"};
    case cache::CacheActionCode::restarted_from_zero:
    case cache::CacheActionCode::location_unavailable:
    case cache::CacheActionCode::insufficient_space:
    case cache::CacheActionCode::space_unknown:
    case cache::CacheActionCode::busy:
    case cache::CacheActionCode::retry_not_available:
    case cache::CacheActionCode::interrupted:
    case cache::CacheActionCode::failed:
      return download_failure(result.detail.empty()
                                  ? "the controlled batch cache could not provide an artifact"
                                  : result.detail);
  }
  return download_failure("the controlled batch cache returned an unknown result");
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::pause(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }
  // A synchronous cache transfer cannot honestly promise to interrupt an
  // in-flight transport at an arbitrary instant. Retain the existing transfer
  // state and require a registered downloader pause primitive before claiming
  // a user-requested pause.
  return download_failure(
      "no registered controlled downloader pause operation is available for this batch item");
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::resume(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }
  auto const result = cache_.resume(target.cache_asset.identity);
  switch (result.code) {
    case cache::CacheActionCode::completed:
    case cache::CacheActionCode::already_available:
      return {.code = batch::InstallationDownloadCode::cached_ready,
              .detail = result.detail};
    case cache::CacheActionCode::paused:
      return {.code = batch::InstallationDownloadCode::paused,
              .detail = result.detail};
    case cache::CacheActionCode::restarted_from_zero:
      return {.code = batch::InstallationDownloadCode::restart_required,
              .detail = result.detail.empty()
                            ? "download continuation is unavailable and must restart from zero"
                            : result.detail};
    case cache::CacheActionCode::waiting_for_network:
      return {.code = batch::InstallationDownloadCode::waiting_network,
              .detail = result.detail};
    case cache::CacheActionCode::unsupported:
    case cache::CacheActionCode::asset_not_current:
      return {.code = batch::InstallationDownloadCode::source_invalid,
              .detail = result.detail};
    case cache::CacheActionCode::interrupted:
    case cache::CacheActionCode::location_unavailable:
    case cache::CacheActionCode::insufficient_space:
    case cache::CacheActionCode::space_unknown:
    case cache::CacheActionCode::busy:
    case cache::CacheActionCode::retry_not_available:
    case cache::CacheActionCode::failed:
      return download_failure(result.detail.empty()
                                  ? "the controlled batch download could not be resumed"
                                  : result.detail);
  }
  return download_failure("the controlled batch cache returned an unknown resume result");
}

batch::InstallationDownloadObservation
WindowsInstallationDownloadAdapter::stop(
    batch::InstallationEffectTarget const& target) {
  if (!target.valid()) {
    return download_failure("controlled batch cache target is invalid");
  }
  if (!frozen_cache_root_is_selected(cache_, target.cache_root)) {
    return download_failure("the frozen batch cache root is unavailable");
  }

  auto const result = cache_.abandon(target.cache_asset.identity);
  switch (result.code) {
    case cache::CacheActionCode::interrupted:
      return {.code = batch::InstallationDownloadCode::stopped,
              .detail = "the controlled batch download temporary bytes were discarded"};
    case cache::CacheActionCode::asset_not_current:
    case cache::CacheActionCode::unsupported:
      return {.code = batch::InstallationDownloadCode::source_invalid,
              .detail = result.detail};
    case cache::CacheActionCode::completed:
    case cache::CacheActionCode::already_available:
    case cache::CacheActionCode::paused:
    case cache::CacheActionCode::restarted_from_zero:
    case cache::CacheActionCode::waiting_for_network:
    case cache::CacheActionCode::location_unavailable:
    case cache::CacheActionCode::insufficient_space:
    case cache::CacheActionCode::space_unknown:
    case cache::CacheActionCode::busy:
    case cache::CacheActionCode::retry_not_available:
    case cache::CacheActionCode::failed:
      return download_failure(result.detail.empty()
                                  ? "the controlled batch download could not be stopped"
                                  : result.detail);
  }
  return download_failure("the controlled batch cache returned an unknown stop result");
}

batch::ControlledProfileReadiness
WindowsControlledProfileReadinessAdapter::observe(
    domain::installation_batch::FrozenExecutionProfile const& profile) {
  if (!profile.valid()) {
    return {.code = batch::ControlledProfileReadinessCode::failed,
            .detail = "the frozen controlled profile is invalid"};
  }
  if (profile.execution ==
      domain::software_catalog::WindowsExecutionReadiness::declaration_only ||
      !profile_registered(profile)) {
    return {.code = batch::ControlledProfileReadinessCode::unavailable,
            .detail = "the controlled profile has no matching project-owned Windows executor"};
  }
  return {.code = batch::ControlledProfileReadinessCode::registered,
          .detail = "project-owned Windows executor is registered for this profile"};
}

WindowsOpaqueCacheInstallerLauncher::~WindowsOpaqueCacheInstallerLauncher() {
  std::scoped_lock lock{mutex_};
  for (auto found = operations_.begin(); found != operations_.end();) {
    auto current = found++;
    auto const native = static_cast<HANDLE>(current->second.process);
    if (native != nullptr && ::WaitForSingleObject(native, 0) == WAIT_TIMEOUT) {
      // Closing the workbench must not terminate an official installer. There
      // is no durable operation record for this adapter, so the next process
      // cannot adopt this handle and must report the persisted batch outcome
      // as unknown. Keep the materialized payload while the child may still
      // use it; removing it here would race the running installer. The
      // in-memory record is dropped with this adapter and is not recoverable.
      ::CloseHandle(native);
      current->second.process = nullptr;
      continue;
    }
    cleanup_operation_locked(current, false);
  }
  // Any running child above is intentionally left outside this adapter's
  // observation boundary. Completed or terminated children have already had
  // their temporary payload removed by cleanup_operation_locked().
  operations_.clear();
}

bool WindowsOpaqueCacheInstallerLauncher::operation_matches_target(
    OwnedProcess const& operation,
    batch::InstallationEffectTarget const& target) const noexcept {
  return operation.item_id == target.item_id &&
         operation.profile_id == target.execution_profile.profile_id &&
         operation.baseline == target.execution_profile.baseline &&
         operation.cache_asset_identity == target.cache_asset.identity &&
         operation.cache_root == target.cache_root;
}

void WindowsOpaqueCacheInstallerLauncher::cleanup_operation_locked(
    std::unordered_map<std::string, OwnedProcess>::iterator found,
    bool terminate) noexcept {
  if (found == operations_.end()) {
    return;
  }
  auto const native = static_cast<HANDLE>(found->second.process);
  if (native != nullptr && terminate &&
      ::WaitForSingleObject(native, 0) == WAIT_TIMEOUT) {
    static_cast<void>(::TerminateProcess(native, 1));
    static_cast<void>(::WaitForSingleObject(native, 5'000));
  }
  if (native != nullptr) {
    ::CloseHandle(native);
  }
  std::error_code error;
  std::filesystem::remove(found->second.payload, error);
  if (!found->second.cleanup_directory.empty()) {
    std::filesystem::remove(found->second.cleanup_directory, error);
  }
  operations_.erase(found);
}

batch::ControlledInstallerObservation
WindowsOpaqueCacheInstallerLauncher::launch(
    batch::ControlledInstallerLaunch const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "the controlled installer launch target is invalid"};
  }
  if (!frozen_cache_root_is_selected(cache_, request.target.cache_root)) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = "the frozen batch cache root is unavailable"};
  }
  if (!cached_asset_is_present(cache_, request.target.cache_asset.identity)) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = "the frozen controlled cache artifact is unavailable"};
  }
  auto const kind = controlled_installer_kind(request.target.execution_profile,
                                              request.target.item_id);
  if (!kind.has_value() || !profile_registered(request.target.execution_profile,
                                                request.target.item_id)) {
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "the frozen profile is not a registered project-owned Windows profile"};
  }
  auto const payload = cache_.read_completed_payload(request.target.cache_asset.identity);
  if (payload.code != cache::CompletedCachePayloadReadCode::found || payload.bytes.empty()) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = payload.detail.empty() ? "controlled cache payload is unavailable" : payload.detail};
  }
  if (request.target.cache_asset.expected_bytes.has_value() &&
      *request.target.cache_asset.expected_bytes != payload.bytes.size()) {
    return {.code = batch::InstallerLaunchCode::source_invalid,
            .detail = "controlled cache payload size does not match the frozen asset"};
  }
  if (request.target.cache_asset.expected_sha256.has_value()) {
    auto const expected_sha256 =
        normalize_sha256(*request.target.cache_asset.expected_sha256);
    if (!expected_sha256.has_value()) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = "controlled cache payload SHA-256 is malformed"};
    }
    auto digest = sha256_hex(payload.bytes);
    if (!digest.has_value() || *digest != *expected_sha256) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = "controlled cache payload SHA-256 does not match the frozen asset"};
    }
  }
  std::vector<std::byte> launch_bytes;
  std::filesystem::path cleanup_directory;
  std::wstring extension = L".exe";
  if (*kind == ControlledInstallerKind::archive) {
    if (request.target.cache_asset.kind != cache_domain::CacheAssetKind::archive_package ||
        request.target.cache_asset.archive_members.empty()) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = "archive launcher requires a reviewed archive asset and member whitelist"};
    }
    auto const inspection = inspect_windows_controlled_archive(
        payload.bytes, request.target.cache_asset.archive_members,
        request.target.cache_asset.identity.architecture);
    if (inspection.code != WindowsArchiveValidationCode::valid ||
        inspection.members.size() != 1U) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = inspection.detail.empty() ? "controlled archive failed validation" : inspection.detail};
    }
    // Re-parse to obtain the validated member bytes. This keeps the public
    // inspection result small while ensuring only registered content is ever
    // written to disk.
    auto parsed = parse_controlled_archive(
        payload.bytes, request.target.cache_asset.archive_members,
        request.target.cache_asset.identity.architecture);
    if (parsed.code != WindowsArchiveValidationCode::valid || parsed.members.size() != 1U) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = "controlled archive member materialization failed"};
    }
    launch_bytes = std::move(parsed.members.front().bytes);
    extension = L".exe";
  } else {
    if (request.target.cache_asset.kind == cache_domain::CacheAssetKind::archive_package) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = "ordinary installer registration cannot consume an archive payload"};
    }
    auto const machine = pe_machine(payload.bytes);
    auto const payload_is_valid = *kind == ControlledInstallerKind::msi
                                      ? has_msi_signature(payload.bytes)
                                      : machine.has_value() &&
                                            windows_controlled_pe_machine_matches(
                                                request.target.cache_asset.identity.architecture,
                                                *machine);
    if (!payload_is_valid) {
      return {.code = batch::InstallerLaunchCode::source_invalid,
              .detail = "controlled cache payload is not a matching installer or PE architecture"};
    }
    launch_bytes.assign(payload.bytes.begin(), payload.bytes.end());
    extension = *kind == ControlledInstallerKind::msi ? L".msi" : L".exe";
  }
  auto path = controlled_payload_path(launch_bytes, extension);
  if (!path.has_value()) {
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "controlled installer payload could not be materialized"};
  }
  cleanup_directory = path->parent_path();
  auto const native_path = path->wstring();
  std::wstring application_path = native_path;
  std::wstring command_line = quote_argument(native_path);
  if (*kind == ControlledInstallerKind::msi) {
    auto msiexec = system_msiexec_path();
    if (!msiexec.has_value() ||
        ::GetFileAttributesW(msiexec->c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::error_code error;
      std::filesystem::remove(*path, error);
      std::filesystem::remove(cleanup_directory, error);
      return {.code = batch::InstallerLaunchCode::failed,
              .detail = "the fixed Windows Installer executable is unavailable"};
    }
    application_path = *msiexec;
    command_line = quote_argument(application_path) + L" /i " + quote_argument(native_path) +
                   L" /norestart";
  }
  STARTUPINFOW startup{.cb = sizeof(startup)};
  PROCESS_INFORMATION process{};
  if (!::CreateProcessW(application_path.c_str(), command_line.data(), nullptr, nullptr,
                        FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr,
                        &startup, &process)) {
    std::error_code error;
    std::filesystem::remove(*path, error);
    std::filesystem::remove(cleanup_directory, error);
    return {.code = batch::InstallerLaunchCode::failed,
            .detail = "controlled installer process could not be created"};
  }
  ::CloseHandle(process.hThread);
  std::scoped_lock lock{mutex_};
  auto const handle = std::string{"azzs-op-"} +
                      std::to_string(next_operation_++);
  operations_.emplace(handle, OwnedProcess{.process = process.hProcess,
                                            .payload = std::move(*path),
                                            .cleanup_directory = std::move(cleanup_directory),
                                            .item_id = request.target.item_id,
                                            .profile_id = request.target.execution_profile.profile_id,
                                            .baseline = request.target.execution_profile.baseline,
                                            .cache_asset_identity =
                                                request.target.cache_asset.identity,
                                            .cache_root = request.target.cache_root});
  return {.code = batch::InstallerLaunchCode::started,
          .opaque_operation_handle = handle,
          .detail = "controlled installer process started with an owned process handle"};
}

batch::ControlledInstallerCompletionObservation
WindowsOpaqueCacheInstallerLauncher::observe_completion(
    batch::ControlledInstallerCompletionRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "the controlled installer completion target is invalid"};
  }
  if (!request.opaque_operation_handle.has_value()) {
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer operation handle is unavailable"};
  }
  if (!profile_registered(request.target.execution_profile, request.target.item_id)) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "the frozen profile is not bound to this software item"};
  }
  std::scoped_lock lock{mutex_};
  auto found = operations_.find(*request.opaque_operation_handle);
  if (found == operations_.end() || found->second.process == nullptr) {
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer operation handle is not owned by this adapter"};
  }
  if (!operation_matches_target(found->second, request.target)) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "controlled installer operation is bound to a different frozen target"};
  }
  auto const native = static_cast<HANDLE>(found->second.process);
  auto const wait = ::WaitForSingleObject(native, 0);
  if (wait == WAIT_TIMEOUT) {
    return {.code = batch::InstallerCompletionCode::running,
            .detail = "controlled installer process is still running"};
  }
  if (wait != WAIT_OBJECT_0) {
    cleanup_operation_locked(found, false);
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer process wait observation failed"};
  }
  DWORD exit_code{};
  if (!::GetExitCodeProcess(native, &exit_code)) {
    cleanup_operation_locked(found, false);
    return {.code = batch::InstallerCompletionCode::unknown,
            .detail = "controlled installer process exit code is unavailable"};
  }
  auto const exit_success = exit_code == 0;
  cleanup_operation_locked(found, false);
  if (!exit_success) {
    return {.code = batch::InstallerCompletionCode::failed,
            .detail = "controlled installer exited unsuccessfully; result remains unconfirmed"};
  }
  return {.code = batch::InstallerCompletionCode::completed,
          .post_install = batch::PostInstallCompletionCode::not_required,
          .detail = "controlled installer process completed; registry verification is still required"};
}

batch::ControlledInstallerTerminationObservation
WindowsOpaqueCacheInstallerLauncher::force_terminate(
    batch::ControlledInstallerTerminationRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "the controlled installer termination target is invalid"};
  }
  if (!request.opaque_operation_handle.has_value() ||
      request.opaque_operation_handle->empty()) {
    return {.code = batch::InstallerTerminationCode::unknown,
            .detail = "the controlled installer operation handle is unavailable"};
  }
  std::scoped_lock lock{mutex_};
  auto found = operations_.find(*request.opaque_operation_handle);
  if (found == operations_.end() || found->second.process == nullptr) {
    return {.code = batch::InstallerTerminationCode::unknown,
            .detail = "controlled installer operation handle is not owned by this adapter"};
  }
  if (!operation_matches_target(found->second, request.target)) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "controlled installer operation is bound to a different frozen target"};
  }
  if (!profile_registered(request.target.execution_profile, request.target.item_id)) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "the frozen profile is not bound to this software item"};
  }
  auto const native = static_cast<HANDLE>(found->second.process);
  if (::WaitForSingleObject(native, 0) == WAIT_OBJECT_0) {
    cleanup_operation_locked(found, false);
    return {.code = batch::InstallerTerminationCode::terminated,
            .detail = "controlled installer process had already exited and owned resources were cleaned"};
  }
  if (!::TerminateProcess(native, 1) ||
      ::WaitForSingleObject(native, 5'000) != WAIT_OBJECT_0) {
    return {.code = batch::InstallerTerminationCode::failed,
            .detail = "owned controlled installer process could not be terminated"};
  }
  cleanup_operation_locked(found, false);
  return {.code = batch::InstallerTerminationCode::terminated,
          .detail = "owned controlled installer process was force-terminated"};
}

batch::InstallVerificationObservation
WindowsInstallationResultVerifier::verify(
    batch::InstallVerificationRequest const& request) {
  if (!request.target.valid()) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the controlled installation verification target is invalid"};
  }
  if (!valid_verification_phase(request.phase)) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the controlled verification phase is not recognized"};
  }
  if (!profile_registered(request.target.execution_profile, request.target.item_id)) {
    return {.code = batch::InstallVerificationCode::failed,
            .detail = "the frozen profile is not bound to this software item"};
  }
  if (request.target.execution_profile.result_detection !=
      domain::software_catalog::ResultDetectionStrategy::project_owned_presence_probe) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = "the frozen profile does not authorize a project-owned presence probe"};
  }
  if (request.phase != batch::InstallVerificationPhase::before_launch &&
      (!request.opaque_operation_handle.has_value() ||
       request.opaque_operation_handle->empty())) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = "the post-launch verification operation handle is unavailable"};
  }
  auto const observed = presence_.detect_version(
      request.target.item_id, request.target.execution_profile.baseline.version);
  if (!observed.completed) {
    return {.code = batch::InstallVerificationCode::unknown,
            .detail = observed.detail.empty() ? "Windows presence observation is incomplete" : observed.detail};
  }
  if (observed.present) {
    return {.code = batch::InstallVerificationCode::installed,
            .detail = "fixed Windows uninstall registry presence was observed"};
  }
  return {.code = batch::InstallVerificationCode::absent,
          .detail = "fixed Windows uninstall registry presence was not observed"};
}

WindowsInstallationFactSink::WindowsInstallationFactSink(
    application::ExecutionLog& log)
    : log_(log), correlation_(log_.begin_correlation()) {}

void WindowsInstallationFactSink::observe(
    batch::InstallationFact const& fact) noexcept {
  try {
    application::ExecutionEvent event{
        .kind = fact.kind == batch::InstallationFactKind::coverage_gap
                    ? application::ExecutionEventKind::coverage_gap
                    : application::ExecutionEventKind::adapter_result,
        .component = "installation-batch-windows",
        .stage = stage_for(fact.kind),
        .result = fact.result,
        .fields = {
            {"batch_id", fact.batch_id,
             application::DiagnosticValueDisposition::retain},
            {"item_id", fact.item_id,
             application::DiagnosticValueDisposition::retain},
            {"item_state", domain::installation_batch::to_string(fact.item_state),
             application::DiagnosticValueDisposition::retain},
        },
    };
    static_cast<void>(log_.append(correlation_, event));
  } catch (...) {
    // The runner owns its durable outcome decision. Best-effort adapter facts
    // must never alter that decision or terminate a Windows host callback.
  }
}

}  // namespace azzs::adapters::windows
