#pragma once

#include "zxlog/error.h"
#include "zxlog/stats.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

namespace zxlog {
struct Options;
namespace detail {

enum class DropReason { QueueFull, ProducerLimit, TlsLimit, RecordTooLarge };

// Counters support concurrent producers/readers. Error reporting and callbacks
// run on the consumer; snapshots use their own mutex, never held in callbacks.
class Diagnostics {
public:
    void configure(const Options &options);
    static bool in_error_handler() noexcept { return in_error_handler_; }
    void accepted() noexcept { accepted_.fetch_add(1, std::memory_order_relaxed); }
    void drop(DropReason reason) noexcept {
        switch (reason) {
        case DropReason::QueueFull: queue_full_.fetch_add(1, std::memory_order_relaxed); break;
        case DropReason::ProducerLimit: producer_limit_.fetch_add(1, std::memory_order_relaxed); break;
        case DropReason::TlsLimit: tls_limit_.fetch_add(1, std::memory_order_relaxed); break;
        case DropReason::RecordTooLarge: record_too_large_.fetch_add(1, std::memory_order_relaxed); break;
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    void reject_text() noexcept { text_too_large_.fetch_add(1, std::memory_order_relaxed); }
    bool has_errors() const noexcept { return io_errors_.load(std::memory_order_acquire) != 0; }
    bool output_failed() const noexcept { return output_failed_.load(std::memory_order_relaxed); }
    Stats snapshot() const noexcept;
    ErrorInfo last_error() const;
    void file_error(const ErrorInfo &error) noexcept;
    void file_error(ErrorCode code, const char *operation) noexcept;
    void report(ErrorCode code, const char *operation, const std::string &path = {}, int error = 0) noexcept;

private:
    static thread_local bool in_error_handler_;
    std::atomic<std::uint64_t> accepted_{0}, dropped_{0}, io_errors_{0}, text_too_large_{0};
    std::atomic<std::uint64_t> queue_full_{0}, producer_limit_{0}, tls_limit_{0}, record_too_large_{0};
    std::atomic<std::uint64_t> invalid_records_{0}, unknown_sites_{0}, format_errors_{0};
    std::atomic<std::uint64_t> write_errors_{0}, open_errors_{0}, rotate_errors_{0}, internal_errors_{0};
    std::atomic<std::uint64_t> sink_sync_errors_{0};
    std::atomic<bool> output_failed_{false};
    mutable std::mutex error_mutex_;
    ErrorInfo last_error_;
    std::function<void(const ErrorInfo &)> error_handler_;
    std::chrono::milliseconds error_interval_{1000};
    std::uint64_t last_error_callback_ns_ = 0;  // consumer only
};

}  // namespace detail
}  // namespace zxlog
