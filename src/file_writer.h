#pragma once
#include "zxlog/protocol.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zxlog::detail {
// Single-consumer, POSIX/Linux writer. No other process may use the same path.
// Batches writes; performs bounded rotation; repairs a torn final binary record
// on startup, then archives the prior active segment to avoid mixed clock epochs.
class FileWriter {
public:
    FileWriter() = default;
    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
    ~FileWriter();

    struct Config {
        std::string path;
        std::uint64_t max_file_bytes = 1u << 20;
        unsigned max_files = 4;     // inclusive of active file
        std::size_t batch_bytes = 16384;
        unsigned sync_every_records = 0; // 0 => explicit flush(true) only
        bool repair_on_start = true;
        bool sync_on_rotate = true;
    };

    bool open(const Config& config, const std::array<std::uint8_t, kBinaryHeaderSize>& header,
              bool binary, std::string* error);
    bool append(const std::uint8_t* bytes, std::size_t size);
    bool flush(bool durable);
    void close() noexcept;
    bool good() const noexcept { return !failed_; }
    std::uint64_t rotations() const noexcept { return rotations_.load(std::memory_order_relaxed); }
    std::uint64_t repaired_bytes() const noexcept { return repaired_bytes_.load(std::memory_order_relaxed); }
    std::uint64_t sync_errors() const noexcept { return sync_errors_.load(std::memory_order_relaxed); }
private:
    bool create_file();
    bool rotate();
    bool flush_pending();
    bool write_all(const std::uint8_t* p, std::size_t n);
    bool sync_file();
    bool sync_directory();
    bool repair_existing();
    bool prune_excess_archives();
    void fail(const char* reason, int err = 0);

    int fd_ = -1;
    bool binary_ = false;
    bool failed_ = false;
    bool initialized_ = false;
    Config cfg_;
    std::array<std::uint8_t, kBinaryHeaderSize> header_{};
    std::vector<std::uint8_t> pending_;
    std::uint64_t size_ = 0;
    std::atomic<std::uint64_t> rotations_{0};
    std::atomic<std::uint64_t> repaired_bytes_{0};
    std::atomic<std::uint64_t> sync_errors_{0};
    unsigned unsynced_records_ = 0;
    std::string error_;
};
} // namespace zxlog::detail
