#include "azzs/adapters/windows/windows_lease_token_source.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <Rpc.h>

#include <memory>
#include <string>

namespace azzs::adapters::windows {
namespace {

struct RpcStringDeleter final {
  void operator()(unsigned char* value) const noexcept {
    if (value != nullptr) {
      RPC_CSTR owned = value;
      ::RpcStringFreeA(&owned);
    }
  }
};

}  // namespace

std::string WindowsLeaseTokenSource::next_token() {
  UUID id{};
  auto const status = ::UuidCreate(&id);
  if (status != RPC_S_OK && status != RPC_S_UUID_LOCAL_ONLY) {
    return {};
  }
  RPC_CSTR raw = nullptr;
  if (::UuidToStringA(&id, &raw) != RPC_S_OK || raw == nullptr) {
    return {};
  }
  using RpcString = std::unique_ptr<unsigned char, RpcStringDeleter>;
  RpcString value{raw};
  return reinterpret_cast<char const*>(raw);
}

}  // namespace azzs::adapters::windows
