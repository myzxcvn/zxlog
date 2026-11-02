#pragma once

#include "zxlog/detail/admission_state.h"
#include "zxlog/detail/diagnostics.h"
#include "zxlog/detail/log_record.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

namespace zxlog::detail {
class OutputBackend;

// Owns producer registrations, ordering, control barriers and the consumer.
// Logger closes admission and waits for producers before stopping this runtime.
class AsyncRuntime {
    struct ProducerQueue {
        struct Slot {
            std::uint64_t sequence = 0;
            std::uint32_t length = 0;
            Level level = Level::Info;
        };
        ProducerQueue(std::size_t count, std::size_t record_bytes)
            : slots(count), payload(count * record_bytes), stride(record_bytes) {}
        Slot &record(std::uint64_t index) noexcept { return slots[index % slots.size()]; }
        std::uint8_t *data(std::uint64_t index) noexcept {
            return payload.data() + (index % slots.size()) * stride;
        }
        std::vector<Slot> slots;
        std::vector<std::uint8_t> payload;
        const std::size_t stride;
        alignas(64) std::atomic<std::uint64_t> write{0};
        alignas(64) std::atomic<std::uint64_t> read{0};
        std::atomic<unsigned> state{0};  // 0 free, 1 active, 2 retiring
    };

public:
    static constexpr std::size_t kMaxLoggersPerThread = 32;
    class Reservation {
    public:
        explicit operator bool() const noexcept { return queue_ != nullptr; }
        ProducerQueue::Slot &record() const noexcept { return queue_->record(tail_); }
        std::uint8_t *data() const noexcept { return queue_->data(tail_); }
        std::size_t capacity() const noexcept { return queue_->stride; }
    private:
        friend class AsyncRuntime;
        Reservation(ProducerQueue *queue, std::uint64_t tail) : queue_(queue), tail_(tail) {}
        ProducerQueue *queue_;
        std::uint64_t tail_;
    };

    bool prepare(unsigned max_producers, std::size_t slots, std::size_t record_bytes,
                 std::size_t budget, std::string *error);
    std::size_t queue_memory_bytes() const noexcept { return queue_memory_bytes_; }
    std::size_t queue_payload_bytes() const noexcept { return queue_payload_bytes_; }
    void start(OutputBackend &output, Diagnostics &diagnostics);
    void stop();
    void release_queues() noexcept { queues_.clear(); }
    void notify_control_waiters() noexcept { control_space_cv_.notify_all(); }
    void wake() noexcept { wake_cv_.notify_one(); }
    Reservation reserve(std::uint64_t instance_id, Diagnostics &diagnostics) noexcept {
        auto *queue = current_producer(instance_id, diagnostics);
        if (!queue) return {nullptr, 0};
        const auto tail = queue->write.load(std::memory_order_relaxed);
        if (tail - queue->read.load(std::memory_order_acquire) >= queue->slots.size()) {
            diagnostics.drop(DropReason::QueueFull);
            return {nullptr, 0};
        }
        return {queue, tail};
    }
    void publish(const Reservation &reservation) noexcept {
        // Linearization point for producer order and control barriers.
        reservation.record().sequence = sequence_.fetch_add(1, std::memory_order_acq_rel);
        reservation.queue_->write.store(reservation.tail_ + 1, std::memory_order_release);
    }
    bool set_mode(OutputMode mode, const AdmissionState &admission);
    bool flush(bool durable, const AdmissionState &admission);

private:
    enum class ControlKind : std::uint8_t { ModeChange, Barrier };
    struct Control {
        std::uint64_t sequence = 0;
        ControlKind kind = ControlKind::Barrier;
        OutputMode mode = OutputMode::Binary;
        bool durable = false;
        std::shared_ptr<std::promise<bool>> ack;
    };
    ProducerQueue *current_producer(std::uint64_t instance_id, Diagnostics &diagnostics) noexcept;
    bool enqueue_control(Control control, const AdmissionState &admission);
    bool pop_event(LogRecord &scratch, Control &control, bool &is_control);
    void consume(OutputBackend &output, Diagnostics &diagnostics);
    std::atomic<std::uint64_t> sequence_{0};
    std::uint64_t consumed_sequence_ = 0;  // consumer only
    std::mutex control_mutex_;
    std::deque<Control> controls_;
    std::condition_variable control_space_cv_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::vector<std::shared_ptr<ProducerQueue>> queues_;
    std::size_t queue_memory_bytes_ = 0;
    std::size_t queue_payload_bytes_ = 0;
    std::atomic<bool> closing_{false};
    std::thread worker_;
};

}  // namespace zxlog::detail
