#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

#include "azzs/adapters/windows/windows_internet_availability_observer.hpp"

namespace {

using azzs::adapters::windows::WindowsInternetAvailabilityObserver;
using azzs::adapters::windows::WindowsInternetAvailabilityQuery;
using azzs::application::driver_acquisition::DriverNetworkObserver;
using azzs::application::software_selection::NetworkObserver;

[[nodiscard]] bool expect(bool condition, char const* message) {
  if (!condition) {
    std::cerr << "windows internet availability observer contract failed: "
              << message << '\n';
  }
  return condition;
}

class FakeAvailabilityQuery final : public WindowsInternetAvailabilityQuery {
 public:
  enum class Response {
    internet_access,
    no_profile,
    non_internet_access,
    failure,
  };

  [[nodiscard]] bool has_internet_access() const override {
    if (response == Response::failure) {
      throw std::runtime_error{"network query failed"};
    }
    return response == Response::internet_access;
  }

  Response response{Response::no_profile};
};

[[nodiscard]] WindowsInternetAvailabilityObserver observer_for(
    FakeAvailabilityQuery::Response response) {
  auto query = std::make_unique<FakeAvailabilityQuery>();
  query->response = response;
  return WindowsInternetAvailabilityObserver{std::move(query)};
}

[[nodiscard]] bool internet_access_is_available() {
  auto observer = observer_for(FakeAvailabilityQuery::Response::internet_access);
  return expect(observer.available(),
                "InternetAccess must report available");
}

[[nodiscard]] bool availability_is_exposed_through_both_observer_ports() {
  auto observer = observer_for(FakeAvailabilityQuery::Response::internet_access);
  DriverNetworkObserver const& driver_network = observer;
  NetworkObserver const& selection_network = observer;
  return expect(driver_network.available() && selection_network.available(),
                "both observation ports must expose the same availability fact");
}

[[nodiscard]] bool absent_or_non_internet_profile_is_unavailable() {
  auto no_profile = observer_for(FakeAvailabilityQuery::Response::no_profile);
  auto local_access =
      observer_for(FakeAvailabilityQuery::Response::non_internet_access);
  return expect(!no_profile.available(),
                "an absent profile must report unavailable") &&
         expect(!local_access.available(),
                "a non-InternetAccess profile must report unavailable");
}

[[nodiscard]] bool query_failure_is_unavailable() {
  auto observer = observer_for(FakeAvailabilityQuery::Response::failure);
  return expect(!observer.available(),
                "a Windows connectivity query failure must report unavailable");
}

}  // namespace

int main() {
  return internet_access_is_available() &&
                 availability_is_exposed_through_both_observer_ports() &&
                 absent_or_non_internet_profile_is_unavailable() &&
                 query_failure_is_unavailable()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
