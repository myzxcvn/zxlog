#pragma once

#include "zxlog/protocol.h"
#include "zxlog/identity.h"
#include "zxlog/error.h"
#include "zxlog/sink.h"
#include "zxlog/dispatcher.h"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace zxlog {
struct Options {
    OutputMode initial_mode = OutputMode::Binary;
    Level level = Level::Trace;
    unsigned max_producers = 8;         // retired queues are reusable after draining
    std::size_t per_thread_slots = 32;  // SPSC capacity per producer, allocated at init
    std::size_t max_record_bytes = kMaxRecordSize;  // 32..1024, includes record header
    std::size_t max_queue_bytes = 8u << 20;  // cap for producer payload + slot metadata
    std::shared_ptr<Dispatcher> dispatcher; // optional shared worker
    std::vector<std::shared_ptr<Sink>> extra_sinks; // optional worker-side outputs
    std::string binary_path = "robot.blog";
    std::string text_path = "robot.log";
    std::string dictionary_path = "robot.zdict";
    std::array<std::uint8_t, 16> build_id = linked_build_id();
    std::uint64_t max_file_bytes = 1u << 20;
    unsigned max_files = 4;  // active + up to 3 rotated archives
    std::size_t io_batch_bytes = 16384;
    std::size_t max_text_bytes = 64u << 10;  // complete line, including prefix/newline
    std::chrono::milliseconds flush_interval{100};  // 0 disables periodic write
    std::chrono::milliseconds sync_interval{0};     // periodic fdatasync, opt-in
    Level flush_on_level = Level::Off;
    Level sync_on_level = Level::Off;
    std::function<void(const ErrorInfo &)> error_handler;  // backend thread; must return promptly
    std::chrono::milliseconds error_interval{1000};      // rate limit per instance
    unsigned sync_every_records = 0;  // 0 disables record-count-triggered sync
    bool repair_on_start = true;
    bool sync_on_rotate = true;
};

}  // namespace zxlog
