#include "zxlog/detail/async_runtime.h"
#include "zxlog/detail/output_backend.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace zxlog::detail {

bool AsyncRuntime::prepare(unsigned max_producers, std::size_t slots, std::size_t record_bytes,
                           std::size_t budget, std::string *error) {
    // Divide before multiplying to avoid overflow; budget accounts for the
    // preallocated producer slots, not thread stacks or writer buffers.
    const std::size_t per_slot = record_bytes + sizeof(ProducerQueue::Slot);
    if (slots == 0 || max_producers == 0 ||
        per_slot > budget / slots / max_producers) {
        if (error) *error = "SPSC queue memory budget exceeded";
        return false;
    }
    std::vector<std::shared_ptr<ProducerQueue>> queues;
    queues.reserve(max_producers);
    for (unsigned i = 0; i < max_producers; ++i)
        queues.push_back(std::make_shared<ProducerQueue>(slots, record_bytes));
    queues_ = std::move(queues);
    queue_payload_bytes_ = static_cast<std::size_t>(max_producers) * slots * record_bytes;
    queue_memory_bytes_ = static_cast<std::size_t>(max_producers) * slots * per_slot;
    return true;
}

void AsyncRuntime::start(OutputBackend &output, Diagnostics &diagnostics) {
    closing_ = false;
    worker_ = std::thread([this, &output, &diagnostics] { consume(output, diagnostics); });
}

void AsyncRuntime::stop() {
    // Admission is closed and all producers have left before this point.
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        closing_ = true;
    }
    wake_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

AsyncRuntime::ProducerQueue *AsyncRuntime::current_producer(std::uint64_t instance_id, Diagnostics &diagnostics) noexcept {
    struct Binding {
        std::uint64_t id = 0;
        std::weak_ptr<ProducerQueue> lifetime;
        ProducerQueue *queue = nullptr;
        ~Binding() {
            if (auto alive = lifetime.lock()) {
                alive->state.store(2, std::memory_order_release);
            }
        }
    };

    struct ThreadQueues {
        std::array<Binding, kMaxLoggersPerThread> bindings;
        Binding *last = nullptr;
    };

    thread_local ThreadQueues tls;
    // record() holds an admission count and shutdown waits before freeing
    // queues. IDs never repeat, so a raw cached queue is safe on this path.
    if (tls.last && tls.last->id == instance_id) {
        return tls.last->queue;
    }

    Binding *free = nullptr;
    for (auto &binding : tls.bindings) {
        if (binding.id == instance_id) {
            tls.last = &binding;
            return binding.queue;
        }

        if (!free && binding.lifetime.expired()) {
            free = &binding;
        }
    }

    if (!free) {
        diagnostics.drop(DropReason::TlsLimit);
        return nullptr;
    }

    for (const auto &q : queues_) {
        unsigned expected = 0;
        if (q->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            free->id = instance_id;
            free->lifetime = q;
            free->queue = q.get();
            tls.last = free;
            return q.get();
        }
    }

    diagnostics.drop(DropReason::ProducerLimit);
    return nullptr;
}

bool AsyncRuntime::enqueue_control(Control control, const AdmissionState &admission) {
    if (Diagnostics::in_error_handler()) return false;
    std::unique_lock<std::mutex> lock(control_mutex_);
    if (!admission.accepting() || closing_) {
        return false;
    }

    control_space_cv_.wait(
        lock, [&] { return controls_.size() < 64 || !admission.accepting(); });
    if (!admission.accepting() || closing_) {
        return false;
    }

    // Reserve sequence only after the potentially allocating insertion succeeds.
    try {
        controls_.push_back(std::move(control));
    } catch (const std::bad_alloc &) {
        return false;
    }

    controls_.back().sequence = sequence_.fetch_add(1, std::memory_order_acq_rel);
    lock.unlock();
    wake_cv_.notify_one();
    return true;
}

bool AsyncRuntime::set_mode(OutputMode mode, const AdmissionState &admission) {
#if ZXLOG_BUILD_MODE == 3
    if (mode != OutputMode::Text && mode != OutputMode::Binary) {
        return false;
    }

    Control c;
    c.kind = ControlKind::ModeChange;
    c.mode = mode;
    return enqueue_control(std::move(c), admission);
#else
    (void)mode;
    (void)admission;
    return false;
#endif
}

bool AsyncRuntime::flush(bool durable, const AdmissionState &admission) {
    if (Diagnostics::in_error_handler()) return false;
    auto promise = std::make_shared<std::promise<bool>>();
    auto ready = promise->get_future();
    Control c;
    c.kind = ControlKind::Barrier;
    c.durable = durable;
    c.ack = std::move(promise);
    if (!enqueue_control(std::move(c), admission)) {
        return false;
    }

    return ready.get();
}

bool AsyncRuntime::pop_event(LogRecord &scratch, Control &ctrl, bool &is_control) {
    const std::uint64_t expected = consumed_sequence_;
    for (const auto &handle : queues_) {
        auto &q = *handle;
        const std::uint64_t head = q.read.load(std::memory_order_relaxed);
        const std::uint64_t tail = q.write.load(std::memory_order_acquire);
        if (head == tail) {
            unsigned retiring = 2;
            (void)q.state.compare_exchange_strong(retiring, 0, std::memory_order_acq_rel);
            continue;
        }

        const auto &record = q.record(head);
        if (record.sequence != expected) {
            continue;
        }

        scratch.sequence = record.sequence;
        scratch.length = record.length;
        scratch.level = record.level;
        std::memcpy(scratch.bytes.data(), q.data(head), scratch.length);
        q.read.store(head + 1, std::memory_order_release);
        is_control = false;
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (!controls_.empty() && controls_.front().sequence == expected) {
            ctrl = std::move(controls_.front());
            controls_.pop_front();
            is_control = true;
            control_space_cv_.notify_one();
            return true;
        }
    }

    return false;
}

void AsyncRuntime::consume(OutputBackend &output, Diagnostics &diagnostics) {
    LogRecord scratch;
    const auto &policy = output.policy();
    Control control;
    bool is_control = false;
    using Clock = std::chrono::steady_clock;
    auto next_flush = Clock::now() + policy.flush_interval;
    auto next_sync = Clock::now() + policy.sync_interval;
    for (;;) {
        const auto now = Clock::now();
        if (policy.sync_interval.count() > 0 && now >= next_sync) {
            if (output.needs_sync()) {
                (void)output.flush(true, diagnostics);
            }
            next_sync = now + policy.sync_interval;
        }
        if (policy.flush_interval.count() > 0 && now >= next_flush) {
            if (output.has_pending()) {
                (void)output.flush(false, diagnostics);
            }
            next_flush = now + policy.flush_interval;
        }
        if (pop_event(scratch, control, is_control)) {
            ++consumed_sequence_;
            if (!is_control) {
                try {
                    output.write(scratch, diagnostics);
                    if (scratch.level >= policy.sync_on_level && policy.sync_on_level != Level::Off) {
                        (void)output.flush(true, diagnostics);
                    } else if (scratch.level >= policy.flush_on_level && policy.flush_on_level != Level::Off) {
                        (void)output.flush(false, diagnostics);
                    }
                } catch (...) {
                    diagnostics.report(ErrorCode::Internal, "write event exception");
                }
            } else if (control.kind == ControlKind::ModeChange) {
                // Ensure old pending bytes are not interleaved with new output mode.
                output.set_mode(control.mode, diagnostics);
            } else {
                const bool ok =
                    output.flush(control.durable, diagnostics) && !diagnostics.has_errors();
                if (control.ack) {
                    control.ack->set_value(ok);
                }
            }

            control = Control{};
            continue;
        }

        if (closing_ && consumed_sequence_ == sequence_.load(std::memory_order_acquire)) {
            break;
        }

        std::unique_lock<std::mutex> lock(wake_mutex_);
        // Bounded polling avoids the lost-notification window without a shared
        // mutex in the hot producer path. Idle wake is at most ~5 ms.
        auto deadline = Clock::now() + std::chrono::milliseconds(5);
        if (policy.flush_interval.count() > 0) deadline = std::min(deadline, next_flush);
        if (policy.sync_interval.count() > 0) deadline = std::min(deadline, next_sync);
        wake_cv_.wait_until(lock, deadline);
    }

    (void)output.flush(false, diagnostics);
}

}  // namespace zxlog::detail
