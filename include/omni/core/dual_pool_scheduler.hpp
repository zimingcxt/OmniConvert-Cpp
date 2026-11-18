#pragma once

#include "omni/core/types.hpp"

#include <atomic>
#include <functional>
#include <vector>

namespace omni::core {

class DualPoolScheduler {
public:
    using TaskProgressFn = std::function<void(int rowId, int percent, const std::string& statusText)>;
    using TaskFinishedFn = std::function<void(const TaskResult& result)>;
    using BatchFinishedFn = std::function<void(int successCount, int failCount, std::uintmax_t savedBytes)>;

    DualPoolScheduler() = default;
    ~DualPoolScheduler() = default;

    void requestCancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
    bool isCancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

    // Blocking call intended to be executed from background coordinator thread;
    // dispatches tasks across worker threads (parallel CPU workers for Image/Novel, bounded workers for Audio/Video).
    void executeBatch(
        const std::vector<TaskItem>& tasks,
        const ConversionOptions& options,
        const TaskProgressFn& onProgress,
        const TaskFinishedFn& onTaskFinished,
        const BatchFinishedFn& onBatchFinished);

private:
    std::atomic<bool> cancelled_{false};
};

} // namespace omni::core
