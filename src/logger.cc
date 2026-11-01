#include "zxlog/logger.h"
#include "file_writer.h"
#if ZXLOG_BUILD_MODE != 2
#include "text_format.h"
#endif
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <unistd.h>

namespace zxlog {
Logger& Logger::instance() { static Logger singleton; return singleton; }
Logger::~Logger() { shutdown(); }
std::uint64_t Logger::ns_wall() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
std::uint64_t Logger::ns_steady() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool Logger::initialize(const Options& options, std::string* error) {
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    if (initialized_) { if (error) *error = "zxlog supports one lifecycle per process"; return false; }
    if (options.max_producers < 1 || options.max_producers > 256 ||
        options.per_thread_slots < 2 || options.per_thread_slots > 65536) {
        if (error) *error = "invalid SPSC registration / slot capacity";
        return false;
    }
#if ZXLOG_BUILD_MODE == 1
    if (options.initial_mode != OutputMode::Text) { if (error) *error = "TEXT build only"; return false; }
#elif ZXLOG_BUILD_MODE == 2
    if (options.initial_mode != OutputMode::Binary) { if (error) *error = "BINARY build only"; return false; }
#endif
    options_ = options;
    active_mode_ = options.initial_mode;
    start_wall_ns_ = ns_wall();
    start_steady_ns_ = ns_steady();
    queues_.reserve(options.max_producers);
    for (unsigned i = 0; i < options.max_producers; ++i)
        queues_.push_back(std::make_shared<ProducerQueue>(options.per_thread_slots));

    detail::FileWriter::Config fc;
    fc.max_file_bytes = options.max_file_bytes;
    fc.max_files = options.max_files;
    fc.batch_bytes = options.io_batch_bytes;
    fc.sync_every_records = options.sync_every_records;
    fc.repair_on_start = options.repair_on_start;
    fc.sync_on_rotate = options.sync_on_rotate;

#if ZXLOG_BUILD_MODE != 2
    dictionary_ = std::make_unique<detail::Dictionary>();
    if (!dictionary_->load(options.dictionary_path, options.build_id, error)) return false;
    text_file_ = std::make_unique<detail::FileWriter>();
    fc.path = options.text_path;
    if (!text_file_->open(fc, {}, false, error)) return false;
#endif
#if ZXLOG_BUILD_MODE != 1
    std::array<std::uint8_t, kBinaryHeaderSize> header{};
    std::memcpy(header.data(), "ZXLOG02\0", 8);
    detail::write_u16(header.data() + 8, kWireVersion);
    detail::write_u16(header.data() + 10, kBinaryHeaderSize);
    std::memcpy(header.data() + 12, options.build_id.data(), options.build_id.size());
    detail::write_u64(header.data() + 28, start_wall_ns_);
    detail::write_u64(header.data() + 36, start_steady_ns_);
    detail::write_u32(header.data() + 44, static_cast<std::uint32_t>(::getpid()));
    // 12-byte process/boot discriminator, not a cryptographic nonce.
    detail::write_u64(header.data() + 48, start_wall_ns_ ^ (start_steady_ns_ << 1));
    detail::write_u32(header.data() + 56, static_cast<std::uint32_t>(start_steady_ns_ >> 32));
    detail::write_u32(header.data() + 60, detail::crc32(header.data(), 60));
    binary_file_ = std::make_unique<detail::FileWriter>();
    fc.path = options.binary_path;
    if (!binary_file_->open(fc, header, true, error)) return false;
#endif
    closing_ = false;
    minimum_.store(options.level, std::memory_order_release);
    initialized_ = true;
    accepting_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { consume(); });
    return true;
}

Logger::ProducerQueue* Logger::current_producer() noexcept {
    // TLS owns a reference so a late TLS destructor cannot access freed storage.
    // A retiring queue returns to the pool only after the consumer has drained it.
    struct ThreadQueue {
        std::shared_ptr<ProducerQueue> queue;
        ~ThreadQueue() {
            if (queue) queue->state.store(2, std::memory_order_release);
        }
    };
    thread_local ThreadQueue tls;
    if (tls.queue) return tls.queue.get();
    for (const auto& q : queues_) {
        unsigned expected = 0;
        if (q->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            tls.queue = q;
            return q.get();
        }
    }
    return nullptr;
}

bool Logger::enqueue_control(Control control) {
    std::unique_lock<std::mutex> lock(control_mutex_);
    if (!accepting_.load(std::memory_order_acquire) || closing_) return false;
    control_space_cv_.wait(lock, [&] {
        return controls_.size() < 64 || !accepting_.load(std::memory_order_acquire);
    });
    if (!accepting_.load(std::memory_order_acquire) || closing_) return false;
    control.sequence = sequence_.fetch_add(1, std::memory_order_acq_rel);
    controls_.push_back(std::move(control));
    lock.unlock();
    wake_cv_.notify_one();
    return true;
}
bool Logger::set_mode(OutputMode mode) {
#if ZXLOG_BUILD_MODE == 3
    if (mode != OutputMode::Text && mode != OutputMode::Binary) return false;
    Control c; c.kind = ControlKind::ModeChange; c.mode = mode;
    return enqueue_control(std::move(c));
#else
    (void)mode;
    return false;
#endif
}
bool Logger::flush(bool durable) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto ready = promise->get_future();
    Control c; c.kind = ControlKind::Barrier; c.durable = durable; c.ack = std::move(promise);
    if (!enqueue_control(std::move(c))) return false;
    return ready.get();
}

bool Logger::pop_event(Slot& scratch, Control& ctrl, bool& is_control) {
    const std::uint64_t expected = consumed_sequence_;
    for (const auto& handle : queues_) {
        auto& q = *handle;
        const std::uint64_t head = q.read.load(std::memory_order_relaxed);
        const std::uint64_t tail = q.write.load(std::memory_order_acquire);
        if (head == tail) {
            unsigned retiring = 2;
            (void)q.state.compare_exchange_strong(retiring, 0, std::memory_order_acq_rel);
            continue;
        }
        const auto& record = q.slots[head % q.slots.size()];
        if (record.sequence != expected) continue;
        scratch.sequence = record.sequence;
        scratch.length = record.length;
        std::memcpy(scratch.bytes.data(), record.bytes.data(), scratch.length);
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

void Logger::write_log(const Slot& slot) {
    const auto* bytes = slot.bytes.data();
    if (slot.length < kRecordHeaderSize ||
        detail::read_u32(bytes + 16) != detail::record_crc32(bytes, slot.length)) {
        note_io_error(); return;
    }
#if ZXLOG_BUILD_MODE != 1
    if (active_mode_ == OutputMode::Binary) {
        if (!binary_file_->append(bytes, slot.length)) note_io_error();
        return;
    }
#endif
#if ZXLOG_BUILD_MODE != 2
    const auto id = detail::read_u32(bytes + 4);
    const auto* entry = dictionary_->lookup(id);
    if (!entry) { note_io_error(); return; }
    std::string rendered;
    if (!detail::render_text(*entry, bytes + kRecordHeaderSize,
                             slot.length - kRecordHeaderSize, rendered)) {
        note_io_error(); return;
    }
    const auto stamp = detail::read_u64(bytes + 8);
    const auto delta = stamp >= start_steady_ns_ ? stamp - start_steady_ns_ : 0;
    std::ostringstream out;
    out << "[+" << delta / 1000000000ull << '.' << std::setw(3) << std::setfill('0')
        << (delta / 1000000ull) % 1000ull << "] [" << detail::level_name(entry->level)
        << "] " << rendered << '\n';
    const auto line = out.str();
    if (!text_file_->append(reinterpret_cast<const std::uint8_t*>(line.data()), line.size()))
        note_io_error();
#endif
}

bool Logger::flush_writers(bool durable) {
    bool okay = true;
    if (text_file_ && !text_file_->flush(durable)) { note_io_error(); okay = false; }
    if (binary_file_ && !binary_file_->flush(durable)) { note_io_error(); okay = false; }
    return okay;
}

void Logger::consume() {
    Slot scratch;
    Control control;
    bool is_control = false;
    for (;;) {
        if (pop_event(scratch, control, is_control)) {
            ++consumed_sequence_;
            if (!is_control) {
                try { write_log(scratch); }
                catch (...) { note_io_error(); }
            } else if (control.kind == ControlKind::ModeChange) {
                // Ensure old pending bytes are not interleaved with new output mode.
                if (!flush_writers(false)) note_io_error();
                active_mode_ = control.mode;
            } else {
                const bool ok = flush_writers(control.durable) &&
                    io_errors_.load(std::memory_order_acquire) == 0;
                if (control.ack) control.ack->set_value(ok);
            }
            control = Control{};
            continue;
        }
        if (closing_ && consumed_sequence_ == sequence_.load(std::memory_order_acquire))
            break;
        std::unique_lock<std::mutex> lock(wake_mutex_);
        // Bounded polling avoids the lost-notification window without a shared
        // mutex in the hot producer path. Idle wake is at most ~5 ms.
        wake_cv_.wait_for(lock, std::chrono::milliseconds(5));
    }
    (void)flush_writers(false);
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    if (!initialized_) return;
    accepting_.store(false, std::memory_order_release);
    control_space_cv_.notify_all();
    while (inflight_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        closing_ = true;
    }
    wake_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (text_file_) text_file_->close();
    if (binary_file_) binary_file_->close();
    // Keep initialized=true to prevent TLS registrations from being reused.
}

Stats Logger::stats() const noexcept {
    Stats stats;
    stats.accepted = accepted_.load(std::memory_order_relaxed);
    stats.dropped = dropped_.load(std::memory_order_relaxed);
    stats.io_errors = io_errors_.load(std::memory_order_relaxed);
    if (binary_file_) {
        stats.rotations += binary_file_->rotations();
        stats.repaired_bytes += binary_file_->repaired_bytes();
        stats.sync_errors += binary_file_->sync_errors();
    }
    if (text_file_) {
        stats.rotations += text_file_->rotations();
        stats.repaired_bytes += text_file_->repaired_bytes();
        stats.sync_errors += text_file_->sync_errors();
    }
    return stats;
}
} // namespace zxlog
