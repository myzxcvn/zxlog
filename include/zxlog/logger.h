#pragma once

#include "zxlog/site.h"
#include "zxlog/crc32.h"
#include "zxlog/options.h"
#include "zxlog/stats.h"
#include "zxlog/detail/admission_state.h"
#include "zxlog/detail/async_runtime.h"
#include "zxlog/detail/diagnostics.h"
#include "zxlog/detail/output_backend.h"
#include <fmt/format.h>
#include <chrono>
#include <memory>
#include <tuple>

namespace zxlog {
namespace detail {
template <typename... Args, typename Format>
void validate_format(Format format) {
    fmt::format_string<Args...> checked = format;
    (void)checked;
}
}  // namespace detail

class Logger {
public:
    static Logger &instance();
    static std::unique_ptr<Logger> create(const Options &options, std::string *error = nullptr);
    // Per-thread registrations are bounded; closed instances do not occupy slots.
    static constexpr std::size_t kMaxLoggersPerThread = detail::AsyncRuntime::kMaxLoggersPerThread;
    ~Logger();
    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    bool initialize(const Options &options, std::string *error = nullptr);
    bool enabled(Level level) const noexcept {
        return !detail::Diagnostics::in_error_handler() && admission_.enabled(level);
    }
    void set_level(Level level) noexcept { admission_.set_level(level); }
    // RUNTIME only: enqueued control event orders output transition with events.
    bool set_mode(OutputMode mode);
    // Flush buffered output. durable=true requests fdatasync before acknowledging.
    bool flush(bool durable = false);
    // shutdown may overlap logging; destruction requires all callers to stop.
    void shutdown();
    Stats stats() const noexcept;
    ErrorInfo last_error() const;
    bool output_failed() const noexcept { return diagnostics_.output_failed(); }

    template <typename... Args>
    void record(Level level, std::uint32_t site_id, Args &&...args) noexcept {
        static_assert(((detail::type_of<Args>() != Type::Invalid) && ...),
                      "zxlog: argument cannot be serialized");
        if (detail::Diagnostics::in_error_handler() || !admission_.enter_record()) {
            return;
        }

        const auto finish = [this]() noexcept { admission_.leave_record(); };
        const auto reservation = runtime_.reserve(admission_.instance_id(), diagnostics_);
        if (!reservation) {
            finish();
            return;
        }
        auto &slot = reservation.record();
        std::uint8_t *data = slot.bytes.data();
        std::uint8_t *p = data + kRecordHeaderSize;
        if (!(detail::write_value(p, data + slot.bytes.size(), args) && ...)) {
            diagnostics_.drop(detail::DropReason::RecordTooLarge);
            // An incomplete slot is never published and consumes no sequence.
            finish();
            return;
        }
        const auto bytes = static_cast<std::size_t>(p - data);
        slot.length = static_cast<std::uint32_t>(bytes);
        slot.level = level;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const std::uint64_t stamp =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
        detail::write_u32(data, static_cast<std::uint32_t>(bytes));
        detail::write_u32(data + 4, site_id);
        detail::write_u64(data + 8, stamp);
        detail::write_u32(data + 16, detail::record_crc32(data, bytes));
        // Linearization point for cross-producer ordering and mode barriers.
        runtime_.publish(reservation);
        diagnostics_.accepted();
        finish();
        runtime_.wake();
    }

private:
    Logger();
    // shutdown() joins the runtime before these components are destroyed.
    detail::AdmissionState admission_;
    detail::Diagnostics diagnostics_;
    detail::OutputBackend output_;
    detail::AsyncRuntime runtime_;
};

namespace detail {

inline Logger *logger_ptr(Logger &logger) noexcept { return &logger; }
inline Logger *logger_ptr(Logger *logger) noexcept { return logger; }
inline Logger *logger_ptr(std::nullptr_t) noexcept { return nullptr; }

template <typename Deleter>
Logger *logger_ptr(const std::unique_ptr<Logger, Deleter> &logger) noexcept {
    return logger.get();
}

inline Logger *logger_ptr(const std::shared_ptr<Logger> &logger) noexcept { return logger.get(); }

}  // namespace detail
}  // namespace zxlog

#define ZXLOG_DETAIL(target_, level_, literal_, ...)                                                     \
    do {                                                                                                 \
        auto &&zxlog_handle_ = (target_);                                                                \
        auto *zxlog_logger_ = ::zxlog::detail::logger_ptr(zxlog_handle_);                                \
        if (zxlog_logger_ && zxlog_logger_->enabled(level_)) {                                           \
            std::apply(                                                                                  \
                [&](auto &&...zxlog_args_) {                                                             \
                    if (false) {                                                                         \
                        ::zxlog::detail::validate_format<decltype(::zxlog::detail::wire_value(           \
                            zxlog_args_))...>(FMT_STRING(literal_));                                     \
                    }                                                                                    \
                    static constexpr auto zxlog_site_ ZXLOG_SITE_ATTR =                                  \
                        ::zxlog::detail::make_site<decltype(zxlog_args_)...>(level_, __FILE__, __LINE__, \
                                                                             __COUNTER__, literal_);     \
                    zxlog_logger_->record(level_, zxlog_site_.id, zxlog_args_...);                       \
                },                                                                                       \
                std::forward_as_tuple(__VA_ARGS__));                                                     \
        }                                                                                                \
    } while (false)

#define ZXLOG_DETAIL_LEVEL_TRACE ::zxlog::Level::Trace
#define ZXLOG_DETAIL_LEVEL_DEBUG ::zxlog::Level::Debug
#define ZXLOG_DETAIL_LEVEL_INFO ::zxlog::Level::Info
#define ZXLOG_DETAIL_LEVEL_WARN ::zxlog::Level::Warn
#define ZXLOG_DETAIL_LEVEL_ERROR ::zxlog::Level::Error
#define ZXLOG_DETAIL_LEVEL_CRITICAL ::zxlog::Level::Critical

// Recognize a level token, not the number of variadic formatting arguments.
#define ZXLOG_DETAIL_SECOND(a_, b_, ...) b_
#define ZXLOG_DETAIL_CHECK(...) ZXLOG_DETAIL_SECOND(__VA_ARGS__, 0, 0)
#define ZXLOG_DETAIL_PROBE() ~, 1
#define ZXLOG_DETAIL_IS_PAREN_PROBE(...) ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_IS_PAREN(x_) ZXLOG_DETAIL_CHECK(ZXLOG_DETAIL_IS_PAREN_PROBE x_)
#define ZXLOG_DETAIL_CAT_IMPL(a_, b_) a_##b_
#define ZXLOG_DETAIL_CAT(a_, b_) ZXLOG_DETAIL_CAT_IMPL(a_, b_)
#define ZXLOG_DETAIL_TAG_TRACE ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_TAG_DEBUG ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_TAG_INFO ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_TAG_WARN ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_TAG_ERROR ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_TAG_CRITICAL ZXLOG_DETAIL_PROBE()
#define ZXLOG_DETAIL_GLOBAL(level_, ...) \
    ZXLOG_DETAIL(::zxlog::Logger::instance(), ZXLOG_DETAIL_LEVEL_##level_, __VA_ARGS__)
#define ZXLOG_DETAIL_EXPLICIT(target_, level_, ...) \
    ZXLOG_DETAIL(target_, ZXLOG_DETAIL_LEVEL_##level_, __VA_ARGS__)
#define ZXLOG_DETAIL_ROUTE_1 ZXLOG_DETAIL_GLOBAL
#define ZXLOG_DETAIL_ROUTE_0 ZXLOG_DETAIL_EXPLICIT
#define ZXLOG_DETAIL_PAREN_1(first_, ...) ZXLOG_DETAIL_EXPLICIT(first_, __VA_ARGS__)
#define ZXLOG_DETAIL_PAREN_0(first_, ...) \
    ZXLOG_DETAIL_CAT(ZXLOG_DETAIL_ROUTE_, ZXLOG_DETAIL_CHECK(ZXLOG_DETAIL_TAG_##first_))(first_, __VA_ARGS__)
// ZX_LOG(INFO, ...) or ZX_LOG(logger, INFO, ...). Parenthesize complex logger
// expressions, e.g. ZX_LOG((*ptr), INFO, ...). Null handles discard lazily.
#define ZX_LOG(first_, ...) \
    ZXLOG_DETAIL_CAT(ZXLOG_DETAIL_PAREN_, ZXLOG_DETAIL_IS_PAREN(first_))(first_, __VA_ARGS__)
