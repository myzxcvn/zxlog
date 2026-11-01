#include "zxlog/logger.h"
#include <exception>

namespace zxlog {

Logger::Logger() = default;

std::unique_ptr<Logger> Logger::create(const Options &options, std::string *error) {
    if (detail::Diagnostics::in_error_handler()) {
        if (error) *error = "cannot create a logger from an error handler";
        return nullptr;
    }
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

bool Logger::initialize(const Options &options, std::string *error) {
    if (detail::Diagnostics::in_error_handler()) {
        if (error) *error = "cannot initialize a logger from an error handler";
        return false;
    }
    auto life = admission_.lock_lifecycle();
    if (error) error->clear();
    if (admission_.initialized()) {
        if (error) *error = "zxlog supports one lifecycle per logger";
        return false;
    }
    const auto identity = linked_build_id();
    if (identity == std::array<std::uint8_t, 16>{} || options.build_id != identity) {
        if (error) *error = "unfinalized ELF or build ID differs from linked image";
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
        options.error_interval.count() < 0 || options.error_interval > std::chrono::hours(24) ||
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
        runtime_.prepare(options.max_producers, options.per_thread_slots);
        if (!output_.open(options, error)) {
            runtime_.release_queues();
            return false;
        }
        diagnostics_.configure(options);
        runtime_.start(output_, diagnostics_);
        // Publish only after all resources and the consumer exist.
        admission_.publish(options.level);
        return true;
    } catch (const std::exception &ex) {
        output_.reset();
        runtime_.release_queues();
        if (error) *error = ex.what();
        return false;
    }
}

bool Logger::set_mode(OutputMode mode) { return runtime_.set_mode(mode, admission_); }
bool Logger::flush(bool durable) { return runtime_.flush(durable, admission_); }
ErrorInfo Logger::last_error() const { return diagnostics_.last_error(); }

void Logger::shutdown() {
    if (detail::Diagnostics::in_error_handler()) return;
    auto life = admission_.lock_lifecycle();
    if (!admission_.initialized()) return;
    admission_.stop_accepting();
    runtime_.notify_control_waiters();
    admission_.wait_for_records();
    runtime_.stop();
    output_.close();
    runtime_.release_queues();
    // initialized remains true: reuse requires a new instance ID.
}

Stats Logger::stats() const noexcept {
    auto stats = diagnostics_.snapshot();
    output_.add_stats(stats);
    return stats;
}

}  // namespace zxlog
