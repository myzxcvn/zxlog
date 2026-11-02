#pragma once
#include "zxlog/protocol.h"
#include "zxlog/error.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace zxlog {

// The worker invokes each sink, after formatting in TEXT mode. In BINARY
// mode it receives complete CRC-protected event records, NOT the file header.
// Implementations must not synchronously invoke methods on their Logger.
// Sharing one Sink between Logger instances requires external synchronization.
// Sink operations are non-throwing, and false indicates a diagnostic failure.
class Sink {
public:
    virtual ~Sink() = default;
    virtual OutputMode mode() const noexcept = 0;
    virtual bool append(const std::uint8_t *data, std::size_t size) noexcept = 0;
    virtual bool flush(bool durable) noexcept = 0;
    virtual void close() noexcept {}
};

struct SinkStats {
    std::uint64_t rotations = 0;
    std::uint64_t repaired_bytes = 0;
    std::uint64_t sync_errors = 0;
};

// Adapts the existing hardened POSIX FileWriter. The owner must hold a
// PathLease for file and rotated archive names before calling open().
class FileSink final : public Sink {
public:
    struct Config {
        std::string path;
        std::uint64_t max_file_bytes = 1u << 20;
        unsigned max_files = 4;
        std::size_t batch_bytes = 16384;
        unsigned sync_every_records = 0;
        bool repair_on_start = true;
        bool sync_on_rotate = true;
    };
    FileSink(OutputMode mode, Config config,
             std::array<std::uint8_t, kBinaryHeaderSize> binary_header = {});
    ~FileSink() override;
    FileSink(const FileSink &) = delete;
    FileSink &operator=(const FileSink &) = delete;
    bool open(std::string *error = nullptr);
    OutputMode mode() const noexcept override { return mode_; }
    bool append(const std::uint8_t *data, std::size_t size) noexcept override;
    bool flush(bool durable) noexcept override;
    void close() noexcept override;
    bool has_pending() const noexcept;
    bool needs_sync() const noexcept;
    const ErrorInfo &last_error() const noexcept;
    SinkStats stats() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
    OutputMode mode_;
    Config config_;
    std::array<std::uint8_t, kBinaryHeaderSize> header_{};
};

// Simple stderr sink; append/flush are serialized across ConsoleSink objects.
class ConsoleSink final : public Sink {
public:
    OutputMode mode() const noexcept override { return OutputMode::Text; }
    bool append(const std::uint8_t *data, std::size_t size) noexcept override;
    bool flush(bool durable) noexcept override;
};
}  // namespace zxlog
