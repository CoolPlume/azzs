#include "azzs/adapters/windows/windows_internet_availability_observer.hpp"

#include <winrt/Windows.Networking.Connectivity.h>

#include <memory>
#include <utility>

namespace azzs::adapters::windows {
namespace {

class WinrtInternetAvailabilityQuery final
    : public WindowsInternetAvailabilityQuery {
 public:
  [[nodiscard]] bool has_internet_access() const override {
    auto const profile =
        winrt::Windows::Networking::Connectivity::NetworkInformation::
            GetInternetConnectionProfile();
    return profile && profile.GetNetworkConnectivityLevel() ==
                          winrt::Windows::Networking::Connectivity::
                              NetworkConnectivityLevel::InternetAccess;
  }
};

}  // namespace

WindowsInternetAvailabilityObserver::WindowsInternetAvailabilityObserver()
    : query_(std::make_unique<WinrtInternetAvailabilityQuery>()) {}

WindowsInternetAvailabilityObserver::WindowsInternetAvailabilityObserver(
    std::unique_ptr<WindowsInternetAvailabilityQuery> query)
    : query_(std::move(query)) {}

WindowsInternetAvailabilityObserver::~WindowsInternetAvailabilityObserver() =
    default;

bool WindowsInternetAvailabilityObserver::available() const noexcept {
  if (!query_) {
    return false;
  }
  try {
    return query_->has_internet_access();
  } catch (...) {
    return false;
  }
}

}  // namespace azzs::adapters::windows
