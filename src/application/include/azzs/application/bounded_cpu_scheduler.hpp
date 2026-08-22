#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace azzs::application {

enum class CpuComputationState {
  completed,
  cancelled,
  failed,
};

template <typename Value>
struct CpuComputationResult final {
  CpuComputationState state{CpuComputationState::cancelled};
  std::optional<Value> value;
  std::exception_ptr error;
};

template <typename Value>
struct CpuComputationBatch final {
  // Entries retain the order of the input span, independent of completion order.
  std::vector<CpuComputationResult<Value>> results;
};

namespace detail {

template <typename Operation, typename Input>
concept IndependentCpuComputation =
    std::copy_constructible<Operation> &&
    std::invocable<Operation&, Input const&, std::stop_token> &&
    (!std::is_void_v<
        std::invoke_result_t<Operation&, Input const&, std::stop_token>>) &&
    std::move_constructible<std::remove_cvref_t<
        std::invoke_result_t<Operation&, Input const&, std::stop_token>>>;

}  // namespace detail

// Runs only independent CPU computations that do not modify system or business
// state. Controlled operations remain outside this scheduler's contract.
class BoundedCpuScheduler final {
 public:
  BoundedCpuScheduler() noexcept;
  // The requested maximum is clamped to the worker capacity. When possible,
  // one logical processor remains outside the worker set for UI responsiveness.
  explicit BoundedCpuScheduler(std::size_t requested_maximum) noexcept;

  [[nodiscard]] std::size_t maximum_concurrency() const noexcept;

  template <typename Input, typename Operation>
    requires detail::IndependentCpuComputation<Operation, Input>
  // The caller waits for the bounded workers and must therefore be a
  // core-owned non-UI operation context. Cancellation stops new work and
  // suppresses values that finish after the stop request. Exceptions remain
  // attached to their input result and do not stop independent work.
  [[nodiscard]] auto transform(
      std::span<Input const> inputs, Operation operation,
      std::stop_token cancellation = {}) const {
    using Value = std::remove_cvref_t<
        std::invoke_result_t<Operation&, Input const&, std::stop_token>>;

    // One scheduler owns one bounded worker set. Concurrent callers wait here
    // instead of multiplying the configured resource limit.
    std::scoped_lock invocation_lock{invocation_mutex_};

    CpuComputationBatch<Value> batch{
        .results = std::vector<CpuComputationResult<Value>>(inputs.size()),
    };
    if (inputs.empty() || cancellation.stop_requested()) {
      return batch;
    }

    std::atomic_size_t next_index{0};
    auto const worker_count =
        std::min(maximum_concurrency_, inputs.size());

    {
      std::vector<std::jthread> workers;
      workers.reserve(worker_count);

      for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back(
            [&, worker_operation = operation]() mutable {
              while (!cancellation.stop_requested()) {
                auto const index =
                    next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= inputs.size()) {
                  return;
                }
                if (cancellation.stop_requested()) {
                  return;
                }

                try {
                  auto value = std::invoke(worker_operation, inputs[index],
                                           cancellation);
                  // A value returned after cancellation is intentionally not
                  // published as a completed business result.
                  if (cancellation.stop_requested()) {
                    return;
                  }

                  auto& result = batch.results[index];
                  result.value.emplace(std::move(value));
                  result.state = CpuComputationState::completed;
                } catch (...) {
                  auto& result = batch.results[index];
                  result.error = std::current_exception();
                  result.state = CpuComputationState::failed;
                }
              }
            });
      }
    }

    return batch;
  }

 private:
  std::size_t maximum_concurrency_;
  mutable std::mutex invocation_mutex_;
};

}  // namespace azzs::application
