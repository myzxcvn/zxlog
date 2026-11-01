#include "zxlog/detail/admission_state.h"
#include <limits>
#include <stdexcept>
#include <thread>

namespace zxlog::detail {
namespace {

std::uint64_t next_instance_id() {
    static std::atomic<std::uint64_t> next{1};
    auto id = next.load(std::memory_order_relaxed);
    while (id != std::numeric_limits<std::uint64_t>::max()) {
        if (next.compare_exchange_weak(id, id + 1, std::memory_order_relaxed)) {
            return id;
        }
    }

    throw std::overflow_error("zxlog instance IDs exhausted");
}

}  // namespace

AdmissionState::AdmissionState() : instance_id_(next_instance_id()) {}

void AdmissionState::publish(Level level) noexcept {
    minimum_.store(level, std::memory_order_release);
    initialized_ = true;
    record_state_.store(0, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
}

void AdmissionState::stop_accepting() noexcept {
    accepting_.store(false, std::memory_order_release);
    record_state_.fetch_or(kClosingBit, std::memory_order_acq_rel);
}

void AdmissionState::wait_for_records() const noexcept {
    while ((record_state_.load(std::memory_order_acquire) & ~kClosingBit) != 0)
        std::this_thread::yield();
}

}  // namespace zxlog::detail
