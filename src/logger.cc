#include "zxlog/logger.h"

#include "file_writer.h"
#include "path_lease.h"
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
#include <limits>
#include <stdexcept>

namespace zxlog {
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

Logger::Logger() : instance_id_(next_instance_id()) {}

std::unique_ptr<Logger> Logger::create(const Options &options, std::string *error) {
    try {
        auto logger = std::unique_ptr<Logger>(new Logger);
        if (!logger->initialize(options, error)) {
            return nullptr;
        }

        return logger;
    } catch (const std::exception &ex) {
        if (error) {
            *error = ex.what();
        }

        return nullptr;
    }
}

Logger &Logger::instance() {
    static Logger singleton;
    return singleton;
}

Logger::~Logger() { shutdown(); }

std::uint64_t Logger::ns_wall() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t Logger::ns_steady() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool Logger::initialize(const Options &options, std::string *error) {
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    if (error) {
        error->clear();
    }

    if (initialized_) {
        if (error) {
            *error = "zxlog supports one lifecycle per logger";
        }

        return false;
    }

    if (options.max_producers < 1 || options.max_producers > 256 || options.per_thread_slots < 2 ||
        options.per_thread_slots > 65536) {
        if (error) {
            *error = "invalid SPSC registration / slot capacity";
        }

        return false;
    }

    if (options.initial_mode != OutputMode::Text && options.initial_mode != OutputMode::Binary) {
        if (error) {
            *error = "invalid output mode";
        }

        return false;
    }
    if (options.flush_interval.count() < 0 || options.sync_interval.count() < 0 ||
        options.flush_interval > std::chrono::hours(24) || options.sync_interval > std::chrono::hours(24) ||
        options.level > Level::Off || options.flush_on_level > Level::Off || options.sync_on_level > Level::Off) {
        if (error) *error = "invalid flush interval or level";
        return false;
    }
#if ZXLOG_BUILD_MODE == 1
    if (options.initial_mode != OutputMode::Text) {
        if (error) {
            *error = "TEXT build only";
        }

        return false;
    }
#elif ZXLOG_BUILD_MODE == 2
    if (options.initial_mode != OutputMode::Binary) {
        if (error) {
            *error = "BINARY build only";
        }

        return false;
    }
#endif
    constexpr auto minimum_file_bytes =
        ZXLOG_BUILD_MODE == 1 ? kMaxRecordSize : kBinaryHeaderSize + kMaxRecordSize;
    if (options.max_files < 1 || options.max_files > 256 || options.io_batch_bytes == 0 ||
        options.max_text_bytes == 0 || options.max_text_bytes > (1u << 20) ||
        options.io_batch_bytes > (1u << 20) || options.max_file_bytes < minimum_file_bytes ||
        options.max_file_bytes > (std::uint64_t(1) << 40)) {
        if (error) {
            *error = "invalid file writer quota/batch configuration";
        }

        return false;
    }

    try {
        // Nothing is published to producers until all resources and the worker
        // exist. Local RAII closes writers before releasing their path lease.
        std::vector<std::shared_ptr<ProducerQueue>> queues;
        queues.reserve(options.max_producers);
        for (unsigned i = 0; i < options.max_producers; ++i) {
            queues.push_back(std::make_shared<ProducerQueue>(options.per_thread_slots));
        }
#if ZXLOG_BUILD_MODE != 2
        auto dictionary = std::make_unique<detail::Dictionary>();
        if (!dictionary->load(options.dictionary_path, options.build_id, error)) {
            return false;
        }
#endif
        std::vector<std::string> paths;
#if ZXLOG_BUILD_MODE != 2
        paths.push_back(options.text_path);
#endif
#if ZXLOG_BUILD_MODE != 1
        paths.push_back(options.binary_path);
#endif
        auto lease = detail::PathLease::acquire(paths, error);
        if (!lease) {
            return false;
        }

        std::size_t path_index = 0;
        detail::FileWriter::Config fc;
        fc.max_file_bytes = options.max_file_bytes;
        fc.max_files = options.max_files;
        fc.batch_bytes = options.io_batch_bytes;
        fc.sync_every_records = options.sync_every_records;
        fc.repair_on_start = options.repair_on_start;
        fc.sync_on_rotate = options.sync_on_rotate;
        const auto wall = ns_wall();
        const auto steady = ns_steady();
#if ZXLOG_BUILD_MODE != 2
        auto text_file = std::make_unique<detail::FileWriter>();
        fc.path = lease->paths()[path_index++];
        if (!text_file->open(fc, {}, false, error)) {
            return false;
        }
#endif
#if ZXLOG_BUILD_MODE != 1
        std::array<std::uint8_t, kBinaryHeaderSize> header{};
        std::memcpy(header.data(), "ZXLOG02\0", 8);
        detail::write_u16(header.data() + 8, kWireVersion);
        detail::write_u16(header.data() + 10, kBinaryHeaderSize);
        std::memcpy(header.data() + 12, options.build_id.data(), options.build_id.size());
        detail::write_u64(header.data() + 28, wall);
        detail::write_u64(header.data() + 36, steady);
        detail::write_u32(header.data() + 44, static_cast<std::uint32_t>(::getpid()));
        detail::write_u64(header.data() + 48, wall ^ (steady << 1));
        detail::write_u32(header.data() + 56, static_cast<std::uint32_t>(steady >> 32));
        detail::write_u32(header.data() + 60, detail::crc32(header.data(), 60));
        auto binary_file = std::make_unique<detail::FileWriter>();
        fc.path = lease->paths()[path_index++];
        if (!binary_file->open(fc, header, true, error)) {
            return false;
        }
#endif
        options_ = options;
        active_mode_ = options.initial_mode;
        start_wall_ns_ = wall;
        start_steady_ns_ = steady;
        queues_ = std::move(queues);
        path_lease_ = std::move(lease);
#if ZXLOG_BUILD_MODE != 2
        dictionary_ = std::move(dictionary);
        text_file_ = std::move(text_file);
#endif
#if ZXLOG_BUILD_MODE != 1
        binary_file_ = std::move(binary_file);
#endif
        closing_ = false;
        minimum_.store(options.level, std::memory_order_release);
        worker_ = std::thread([this] { consume(); });
        initialized_ = true;
        record_state_.store(0, std::memory_order_release);
        accepting_.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception &ex) {
        // This also covers thread creation failure after ownership transfer.
        text_file_.reset();
        binary_file_.reset();
#if ZXLOG_BUILD_MODE != 2
        dictionary_.reset();
#endif
        queues_.clear();
        path_lease_.reset();
        if (error) {
            *error = ex.what();
        }

        return false;
    }
}

Logger::ProducerQueue *Logger::current_producer() noexcept {
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
    if (tls.last && tls.last->id == instance_id_) {
        return tls.last->queue;
    }

    Binding *free = nullptr;
    for (auto &binding : tls.bindings) {
        if (binding.id == instance_id_) {
            tls.last = &binding;
            return binding.queue;
        }

        if (!free && binding.lifetime.expired()) {
            free = &binding;
        }
    }

    if (!free) {
        return nullptr;
    }

    for (const auto &q : queues_) {
        unsigned expected = 0;
        if (q->state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
            free->id = instance_id_;
            free->lifetime = q;
            free->queue = q.get();
            tls.last = free;
            return q.get();
        }
    }

    return nullptr;
}

bool Logger::enqueue_control(Control control) {
    std::unique_lock<std::mutex> lock(control_mutex_);
    if (!accepting_.load(std::memory_order_acquire) || closing_) {
        return false;
    }

    control_space_cv_.wait(
        lock, [&] { return controls_.size() < 64 || !accepting_.load(std::memory_order_acquire); });
    if (!accepting_.load(std::memory_order_acquire) || closing_) {
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

bool Logger::set_mode(OutputMode mode) {
#if ZXLOG_BUILD_MODE == 3
    if (mode != OutputMode::Text && mode != OutputMode::Binary) {
        return false;
    }

    Control c;
    c.kind = ControlKind::ModeChange;
    c.mode = mode;
    return enqueue_control(std::move(c));
#else
    (void)mode;
    return false;
#endif
}

bool Logger::flush(bool durable) {
    auto promise = std::make_shared<std::promise<bool>>();
    auto ready = promise->get_future();
    Control c;
    c.kind = ControlKind::Barrier;
    c.durable = durable;
    c.ack = std::move(promise);
    if (!enqueue_control(std::move(c))) {
        return false;
    }

    return ready.get();
}

bool Logger::pop_event(Slot &scratch, Control &ctrl, bool &is_control) {
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

        const auto &record = q.slots[head % q.slots.size()];
        if (record.sequence != expected) {
            continue;
        }

        scratch.sequence = record.sequence;
        scratch.length = record.length;
        scratch.level = record.level;
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

void Logger::write_log(const Slot &slot) {
    const auto *bytes = slot.bytes.data();
    if (slot.length < kRecordHeaderSize ||
        detail::read_u32(bytes + 16) != detail::record_crc32(bytes, slot.length)) {
        note_io_error();
        return;
    }
#if ZXLOG_BUILD_MODE != 1
    if (active_mode_ == OutputMode::Binary) {
        if (!binary_file_->append(bytes, slot.length)) {
            note_io_error();
        }

        return;
    }
#endif
#if ZXLOG_BUILD_MODE != 2
    const auto id = detail::read_u32(bytes + 4);
    const auto *entry = dictionary_->lookup(id);
    if (!entry) {
        note_io_error();
        return;
    }

    const auto stamp = detail::read_u64(bytes + 8);
    const auto delta = stamp >= start_steady_ns_ ? stamp - start_steady_ns_ : 0;
    std::ostringstream out;
    out << "[+" << delta / 1000000000ull << '.' << std::setw(3) << std::setfill('0')
        << (delta / 1000000ull) % 1000ull << "] [" << detail::level_name(entry->level) << "] ";
    auto line = out.str();
    const auto limit = std::min<std::uint64_t>(options_.max_text_bytes, options_.max_file_bytes);
    if (line.size() + 1 > limit) {
        text_too_large_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::string rendered;
    bool too_large = false;
    if (!detail::render_text(*entry, bytes + kRecordHeaderSize, slot.length - kRecordHeaderSize,
                             rendered, limit - line.size() - 1, &too_large)) {
        if (too_large) text_too_large_.fetch_add(1, std::memory_order_relaxed);
        else note_io_error();
        return;
    }
    line += rendered;
    line += '\n';
    if (!text_file_->append(reinterpret_cast<const std::uint8_t *>(line.data()), line.size())) {
        note_io_error();
    }
#endif
}

bool Logger::flush_writers(bool durable) {
    bool okay = true;
    if (text_file_ && !text_file_->flush(durable)) {
        note_io_error();
        okay = false;
    }

    if (binary_file_ && !binary_file_->flush(durable)) {
        note_io_error();
        okay = false;
    }

    return okay;
}

void Logger::consume() {
    Slot scratch;
    Control control;
    bool is_control = false;
    using Clock = std::chrono::steady_clock;
    auto next_flush = Clock::now() + options_.flush_interval;
    auto next_sync = Clock::now() + options_.sync_interval;
    for (;;) {
        const auto now = Clock::now();
        if (options_.sync_interval.count() > 0 && now >= next_sync) {
            if ((text_file_ && text_file_->needs_sync()) || (binary_file_ && binary_file_->needs_sync())) {
                (void)flush_writers(true);
            }
            next_sync = now + options_.sync_interval;
        }
        if (options_.flush_interval.count() > 0 && now >= next_flush) {
            if ((text_file_ && text_file_->has_pending()) || (binary_file_ && binary_file_->has_pending())) {
                (void)flush_writers(false);
            }
            next_flush = now + options_.flush_interval;
        }
        if (pop_event(scratch, control, is_control)) {
            ++consumed_sequence_;
            if (!is_control) {
                try {
                    write_log(scratch);
                    if (scratch.level >= options_.sync_on_level && options_.sync_on_level != Level::Off) {
                        (void)flush_writers(true);
                    } else if (scratch.level >= options_.flush_on_level && options_.flush_on_level != Level::Off) {
                        (void)flush_writers(false);
                    }
                } catch (...) {
                    note_io_error();
                }
            } else if (control.kind == ControlKind::ModeChange) {
                // Ensure old pending bytes are not interleaved with new output mode.
                if (!flush_writers(false)) {
                    note_io_error();
                }

                active_mode_ = control.mode;
            } else {
                const bool ok =
                    flush_writers(control.durable) && io_errors_.load(std::memory_order_acquire) == 0;
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
        if (options_.flush_interval.count() > 0) deadline = std::min(deadline, next_flush);
        if (options_.sync_interval.count() > 0) deadline = std::min(deadline, next_sync);
        wake_cv_.wait_until(lock, deadline);
    }

    (void)flush_writers(false);
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> life(lifecycle_mutex_);
    if (!initialized_) {
        return;
    }

    accepting_.store(false, std::memory_order_release);
    record_state_.fetch_or(kClosingBit, std::memory_order_acq_rel);
    control_space_cv_.notify_all();
    while ((record_state_.load(std::memory_order_acquire) & ~kClosingBit) != 0) {
        std::this_thread::yield();
    }

    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        closing_ = true;
    }

    wake_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }

    if (text_file_) {
        text_file_->close();
    }

    if (binary_file_) {
        binary_file_->close();
    }

    // TLS keeps weak references only; release slots and path ownership now.
    queues_.clear();
    path_lease_.reset();
    // Keep initialized=true: a new lifetime requires a new instance ID.
}

Stats Logger::stats() const noexcept {
    Stats stats;
    stats.accepted = accepted_.load(std::memory_order_relaxed);
    stats.dropped = dropped_.load(std::memory_order_relaxed);
    stats.io_errors = io_errors_.load(std::memory_order_relaxed);
    stats.text_too_large = text_too_large_.load(std::memory_order_relaxed);
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

}  // namespace zxlog
