#pragma once

#include "zxlog/config.h"
#include "zxlog/protocol.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zxlog {
struct Options;
struct Stats;
namespace detail {
class Dictionary;
class FileWriter;
class PathLease;
class Diagnostics;
struct LogRecord;

// Resources are prepared before the worker starts. Only the worker renders,
// writes and changes modes; close/reset run after it has stopped.
class OutputBackend {
public:
    struct FlushPolicy {
        std::chrono::milliseconds flush_interval{100};
        std::chrono::milliseconds sync_interval{0};
        Level flush_on_level = Level::Off;
        Level sync_on_level = Level::Off;
    };

    OutputBackend();
    ~OutputBackend();
    bool open(const Options &options, std::string *error);
    void close() noexcept;
    void reset() noexcept;  // rollback a failed initialization
    void write(const LogRecord &record, Diagnostics &diagnostics);
    bool flush(bool durable, Diagnostics &diagnostics);
    void set_mode(OutputMode mode, Diagnostics &diagnostics);
    bool has_pending() const noexcept;
    bool needs_sync() const noexcept;
    const FlushPolicy &policy() const noexcept { return policy_; }
    void add_stats(Stats &stats) const noexcept;

private:
    FlushPolicy policy_;
    OutputMode active_mode_ = OutputMode::Binary;
    // Declare lease before writers, so destruction closes writers first.
    std::unique_ptr<PathLease> path_lease_;
    std::unique_ptr<FileWriter> binary_file_;
    std::unique_ptr<FileWriter> text_file_;
#if ZXLOG_BUILD_MODE != 2
    std::unique_ptr<Dictionary> dictionary_;
    std::string dictionary_path_, text_path_;
    std::size_t text_limit_ = 0;
    std::uint64_t start_steady_ns_ = 0;
#endif
};

}  // namespace detail
}  // namespace zxlog
