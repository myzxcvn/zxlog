#include "zxlog/detail/diagnostics.h"
#include "zxlog/options.h"
#include <utility>

namespace zxlog::detail {
namespace {
std::uint64_t steady_ns() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}

thread_local bool Diagnostics::in_error_handler_ = false;

void Diagnostics::configure(const Options &options) {
    auto handler = options.error_handler; // Copy may throw; publish only after it succeeds.
    error_handler_ = std::move(handler);
    error_interval_ = options.error_interval;
}

void Diagnostics::file_error(const ErrorInfo &error) noexcept {
    output_failed_.store(true, std::memory_order_relaxed);
    report(error.code, error.operation.c_str(), error.path, error.system_error);
}

void Diagnostics::file_error(ErrorCode code, const char *operation) noexcept {
    // Do not construct an allocating ErrorInfo before entering the guarded
    // Diagnostics::report() path. Important for noexcept Sink error handling.
    output_failed_.store(true, std::memory_order_relaxed);
    // FileWriter supplies its own sync counter; additional Sinks do not.
    if (code == ErrorCode::Sync) ++sink_sync_errors_;
    report(code, operation);
}

ErrorInfo Diagnostics::last_error() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void Diagnostics::report(ErrorCode code, const char *operation, const std::string &path, int error) noexcept {
    io_errors_.fetch_add(1, std::memory_order_relaxed);
    switch (code) {
    case ErrorCode::InvalidRecord: ++invalid_records_; break;
    case ErrorCode::UnknownSite: ++unknown_sites_; break;
    case ErrorCode::Format: ++format_errors_; break;
    case ErrorCode::Open: ++open_errors_; break;
    case ErrorCode::Write: ++write_errors_; break;
    case ErrorCode::Rotate: ++rotate_errors_; break;
    case ErrorCode::Sync: break; // FileWriter counts failed sync operations.
    default: ++internal_errors_; break;
    }
    try {
        ErrorInfo info{code, error, steady_ns(), operation, path};
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            last_error_ = info;
        }
        const auto interval = static_cast<std::uint64_t>(error_interval_.count()) * 1000000;
        if (error_handler_ &&
            (last_error_callback_ns_ == 0 || info.steady_ns - last_error_callback_ns_ >= interval)) {
            last_error_callback_ns_ = info.steady_ns;
            in_error_handler_ = true;
            try { error_handler_(info); } catch (...) { /* User handler cannot kill the worker. */ }
            in_error_handler_ = false;
        }
    } catch (...) { /* Error reporting must still work when allocation fails: counters remain. */ }
}

Stats Diagnostics::snapshot() const noexcept {
    Stats stats;
    stats.accepted = accepted_.load(std::memory_order_relaxed);
    stats.dropped = dropped_.load(std::memory_order_relaxed);
    stats.io_errors = io_errors_.load(std::memory_order_relaxed);
    stats.text_too_large = text_too_large_.load(std::memory_order_relaxed);
    stats.sync_errors = sink_sync_errors_.load(std::memory_order_relaxed);
#define ZXLOG_READ_STAT(name) stats.name = name##_.load(std::memory_order_relaxed)
    ZXLOG_READ_STAT(queue_full);
    ZXLOG_READ_STAT(producer_limit);
    ZXLOG_READ_STAT(tls_limit);
    ZXLOG_READ_STAT(record_too_large);
    ZXLOG_READ_STAT(invalid_records);
    ZXLOG_READ_STAT(unknown_sites);
    ZXLOG_READ_STAT(format_errors);
    ZXLOG_READ_STAT(write_errors);
    ZXLOG_READ_STAT(open_errors);
    ZXLOG_READ_STAT(rotate_errors);
    ZXLOG_READ_STAT(internal_errors);
#undef ZXLOG_READ_STAT
    return stats;
}

}  // namespace zxlog::detail
