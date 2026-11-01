#pragma once

#include "zxlog/protocol.h"
#include <atomic>
#include <cstdint>
#include <mutex>

namespace zxlog::detail {

// Producer admission and lifecycle serialization. Publish/stop/wait are called
// while holding lock_lifecycle(); the record path never takes that mutex.
class AdmissionState {
public:
    AdmissionState();
    std::uint64_t instance_id() const noexcept { return instance_id_; }
    std::unique_lock<std::mutex> lock_lifecycle() { return std::unique_lock<std::mutex>(lifecycle_mutex_); }
    bool initialized() const noexcept { return initialized_; }
    bool accepting() const noexcept { return accepting_.load(std::memory_order_acquire); }
    bool enabled(Level level) const noexcept {
        return accepting() && level >= minimum_.load(std::memory_order_relaxed) && level != Level::Off;
    }
    void set_level(Level level) noexcept { minimum_.store(level, std::memory_order_relaxed); }
    bool enter_record() noexcept {
        if (!accepting()) return false;
        auto state = record_state_.load(std::memory_order_relaxed);
        while (!(state & kClosingBit)) {
            if (record_state_.compare_exchange_weak(state, state + 1, std::memory_order_acquire,
                                                    std::memory_order_relaxed)) return true;
        }
        return false;
    }
    void leave_record() noexcept { record_state_.fetch_sub(1, std::memory_order_release); }
    void publish(Level level) noexcept;
    void stop_accepting() noexcept;
    void wait_for_records() const noexcept;

private:
    static constexpr std::uint64_t kClosingBit = std::uint64_t{1} << 63;
    const std::uint64_t instance_id_;
    std::atomic<bool> accepting_{false};
    std::atomic<Level> minimum_{Level::Off};
    // Admission and close share one atomic modification order.
    std::atomic<std::uint64_t> record_state_{kClosingBit};
    std::mutex lifecycle_mutex_;
    bool initialized_ = false;
};

}  // namespace zxlog::detail
