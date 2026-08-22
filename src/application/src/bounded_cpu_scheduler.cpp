#include "azzs/application/bounded_cpu_scheduler.hpp"

#include <algorithm>
#include <thread>

namespace azzs::application {
namespace {

[[nodiscard]] std::size_t cpu_worker_capacity() noexcept {
  auto const logical_processors =
      std::max<std::size_t>(1, std::thread::hardware_concurrency());
  return logical_processors > 1 ? logical_processors - 1 : 1;
}

}  // namespace

BoundedCpuScheduler::BoundedCpuScheduler() noexcept
    : maximum_concurrency_(cpu_worker_capacity()) {}

BoundedCpuScheduler::BoundedCpuScheduler(
    std::size_t requested_maximum) noexcept
    : maximum_concurrency_(std::min(
          std::max<std::size_t>(1, requested_maximum),
          cpu_worker_capacity())) {}

std::size_t BoundedCpuScheduler::maximum_concurrency() const noexcept {
  return maximum_concurrency_;
}

}  // namespace azzs::application
