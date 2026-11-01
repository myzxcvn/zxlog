#pragma once
#include "zxlog/site.h"
#include "zxlog/crc32.h"
#include <fmt/format.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifndef ZXLOG_BUILD_MODE
#define ZXLOG_BUILD_MODE 3
#endif
#if ZXLOG_BUILD_MODE < 1 || ZXLOG_BUILD_MODE > 3
#error "ZXLOG_BUILD_MODE must be 1=TEXT, 2=BINARY, or 3=RUNTIME"
#endif

namespace zxlog {
namespace detail {
class Dictionary;
class FileWriter;

template<typename... Args, typename Format>
void validate_format(Format format) {
    // FMT_STRING enables compile-time validation with C++17.
    fmt::format_string<Args...> checked = format;
    (void)checked;
}
} // namespace detail

struct Options {
    OutputMode initial_mode = OutputMode::Binary;
    Level level = Level::Trace;
    unsigned max_producers = 8;            // registration slots never recycled in v0.4
    std::size_t per_thread_slots = 32;      // SPSC capacity per producer, allocated at init
    std::string binary_path = "robot.blog";
    std::string text_path = "robot.log";
    std::string dictionary_path = "robot.zdict";
    std::array<std::uint8_t, 16> build_id{};
    std::uint64_t max_file_bytes = 1u << 20;
    unsigned max_files = 4;                 // active + up to 3 rotated archives
    std::size_t io_batch_bytes = 16384;
    unsigned sync_every_records = 0;       // 0 = only explicit flush(true) / rotation sync
    bool repair_on_start = true;
    bool sync_on_rotate = true;
};
struct Stats {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t io_errors = 0;
    std::uint64_t rotations = 0;
    std::uint64_t repaired_bytes = 0;
    std::uint64_t sync_errors = 0;
};

class Logger {
public:
    static Logger& instance();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool initialize(const Options& options, std::string* error = nullptr);
    bool enabled(Level level) const noexcept {
        return accepting_.load(std::memory_order_acquire) &&
            static_cast<unsigned>(level) >= static_cast<unsigned>(minimum_.load(std::memory_order_relaxed)) &&
            level != Level::Off;
    }
    void set_level(Level level) noexcept { minimum_.store(level, std::memory_order_relaxed); }
    // RUNTIME only: enqueued control event orders output transition with events.
    bool set_mode(OutputMode mode);
    // Flush buffered output. durable=true requests fdatasync before acknowledging.
    bool flush(bool durable = false);
    void shutdown();
    Stats stats() const noexcept;

    template<typename... Args>
    void record(std::uint32_t site_id, Args&&... args) noexcept {
        static_assert(((detail::type_of<Args>() != Type::Invalid) && ...),
                      "zxlog: argument cannot be serialized");
        if (!accepting_.load(std::memory_order_acquire)) return;
        inflight_.fetch_add(1, std::memory_order_acq_rel);
        if (!accepting_.load(std::memory_order_acquire)) {
            inflight_.fetch_sub(1, std::memory_order_release);
            return;
        }
        const auto finish = [this]() noexcept { inflight_.fetch_sub(1, std::memory_order_release); };
        std::size_t bytes = kRecordHeaderSize;
        bool fits = true;
        if constexpr (sizeof...(Args) > 0) {
            const auto add_size = [&](const auto& arg) {
                if (!fits) return;
                const auto n = detail::value_size(arg);
                if (n > kMaxRecordSize - bytes) fits = false;
                else bytes += n;
            };
            (add_size(args), ...);
        }
        if (!fits) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            finish(); return;
        }
        ProducerQueue* q = current_producer();
        if (!q) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            finish(); return;
        }
        const std::uint64_t tail = q->write.load(std::memory_order_relaxed);
        if (tail - q->read.load(std::memory_order_acquire) >= q->slots.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            finish(); return;
        }
        Slot& slot = q->slots[tail % q->slots.size()];
        slot.length = static_cast<std::uint32_t>(bytes);
        std::uint8_t* data = slot.bytes.data();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const std::uint64_t stamp = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        detail::write_u32(data, static_cast<std::uint32_t>(bytes));
        detail::write_u32(data + 4, site_id);
        detail::write_u64(data + 8, stamp);
        if constexpr (sizeof...(Args) > 0) {
            std::uint8_t* p = data + kRecordHeaderSize;
            (detail::write_value(p, args), ...);
        }
        detail::write_u32(data + 16, detail::record_crc32(data, bytes));
        // Linearization point for cross-producer ordering and mode barriers.
        slot.sequence = sequence_.fetch_add(1, std::memory_order_acq_rel);
        q->write.store(tail + 1, std::memory_order_release);
        accepted_.fetch_add(1, std::memory_order_relaxed);
        finish();
        wake_cv_.notify_one();
    }

private:
    Logger() = default;
    ~Logger();
    struct Slot {
        std::array<std::uint8_t, kMaxRecordSize> bytes{};
        std::uint64_t sequence = 0;
        std::uint32_t length = 0;
    };
    struct ProducerQueue {
        explicit ProducerQueue(std::size_t count) : slots(count) {}
        std::vector<Slot> slots;
        alignas(64) std::atomic<std::uint64_t> write{0};
        alignas(64) std::atomic<std::uint64_t> read{0};
        std::atomic<unsigned> state{0}; // 0 free, 1 active, 2 retiring
    };
    enum class ControlKind : std::uint8_t { ModeChange, Barrier };
    struct Control {
        std::uint64_t sequence = 0;
        ControlKind kind = ControlKind::Barrier;
        OutputMode mode = OutputMode::Binary;
        bool durable = false;
        std::shared_ptr<std::promise<bool>> ack;
    };
    ProducerQueue* current_producer() noexcept;
    bool enqueue_control(Control control);
    bool pop_event(Slot& scratch, Control& ctrl, bool& is_control);
    void consume();
    void write_log(const Slot& slot);
    bool flush_writers(bool durable);
    static std::uint64_t ns_wall() noexcept;
    static std::uint64_t ns_steady() noexcept;
    void note_io_error() noexcept { io_errors_.fetch_add(1, std::memory_order_relaxed); }

    std::atomic<bool> accepting_{false};
    std::atomic<Level> minimum_{Level::Off};
    std::atomic<std::uint64_t> accepted_{0}, dropped_{0}, io_errors_{0};
    std::atomic<std::uint64_t> sequence_{0}, inflight_{0};
    std::uint64_t consumed_sequence_ = 0; // consumer thread only
    std::mutex lifecycle_mutex_;
    std::mutex control_mutex_;
    std::deque<Control> controls_;
    std::condition_variable control_space_cv_;
    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;
    std::vector<std::shared_ptr<ProducerQueue>> queues_;
    std::atomic<bool> closing_{false};
    bool initialized_ = false;
    OutputMode active_mode_ = OutputMode::Binary;
    Options options_;
    std::thread worker_;
    std::unique_ptr<detail::FileWriter> binary_file_;
    std::unique_ptr<detail::FileWriter> text_file_;
#if ZXLOG_BUILD_MODE != 2
    std::unique_ptr<detail::Dictionary> dictionary_;
#endif
    std::uint64_t start_wall_ns_ = 0, start_steady_ns_ = 0;
};
} // namespace zxlog

#define ZXLOG_DETAIL(level_, literal_, ...)                                      \
    do {                                                                          \
        auto& zxlog_logger_ = ::zxlog::Logger::instance();                         \
        if (zxlog_logger_.enabled(level_)) {                                      \
            std::apply([&](auto&&... zxlog_args_) {                               \
                if (false) {                                                     \
                    ::zxlog::detail::validate_format<                           \
                        decltype(::zxlog::detail::wire_value(zxlog_args_))...>(   \
                        FMT_STRING(literal_));                                   \
                }                                                                 \
                static constexpr auto zxlog_site_ ZXLOG_SITE_ATTR =               \
                    ::zxlog::detail::make_site<decltype(zxlog_args_)...>(         \
                        level_, __FILE__, __LINE__, __COUNTER__, literal_);        \
                zxlog_logger_.record(zxlog_site_.id, zxlog_args_...);             \
            }, std::forward_as_tuple(__VA_ARGS__));                              \
        }                                                                         \
    } while (false)

#define ZXLOG_TRACE(...) ZXLOG_DETAIL(::zxlog::Level::Trace, __VA_ARGS__)
#define ZXLOG_DEBUG(...) ZXLOG_DETAIL(::zxlog::Level::Debug, __VA_ARGS__)
#define ZXLOG_INFO(...)  ZXLOG_DETAIL(::zxlog::Level::Info, __VA_ARGS__)
#define ZXLOG_WARN(...)  ZXLOG_DETAIL(::zxlog::Level::Warn, __VA_ARGS__)
#define ZXLOG_ERROR(...) ZXLOG_DETAIL(::zxlog::Level::Error, __VA_ARGS__)
#define ZXLOG_CRITICAL(...) ZXLOG_DETAIL(::zxlog::Level::Critical, __VA_ARGS__)
#ifndef ZXLOG_NO_SHORT_MACROS
#define LOG_TRACE(...) ZXLOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...) ZXLOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...) ZXLOG_INFO(__VA_ARGS__)
#define LOG_WARN(...) ZXLOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) ZXLOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...) ZXLOG_CRITICAL(__VA_ARGS__)
#endif
