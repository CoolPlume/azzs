#pragma once

#include <memory>

#include "azzs/application/driver_acquisition.hpp"
#include "azzs/application/offline_package_cache.hpp"
#include "azzs/application/software_selection.hpp"

namespace azzs::adapters::windows {

// Keeps tests independent from the Windows network state while exposing only
// the single read-only fact used by the application ports. The production
// query observes the current profile; it does not initiate a connection or
// change adapter state.
class WindowsInternetAvailabilityQuery {
 public:
  virtual ~WindowsInternetAvailabilityQuery() = default;
  [[nodiscard]] virtual bool has_internet_access() const = 0;
};

class WindowsInternetAvailabilityObserver final
    : public application::driver_acquisition::DriverNetworkObserver,
      public application::software_selection::NetworkObserver,
      public application::offline_package_cache::PackageCacheNetworkObserver {
 public:
  WindowsInternetAvailabilityObserver();
  explicit WindowsInternetAvailabilityObserver(
      std::unique_ptr<WindowsInternetAvailabilityQuery> query);
  ~WindowsInternetAvailabilityObserver() override;

  [[nodiscard]] bool available() const noexcept override;

 private:
  std::unique_ptr<WindowsInternetAvailabilityQuery> query_;
};

}  // namespace azzs::adapters::windows
