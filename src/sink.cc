#include "zxlog/sink.h"
#include "file_writer.h"
#include <cstdio>
#include <mutex>
#include <utility>

namespace zxlog {
struct FileSink::State {
    detail::FileWriter writer;
    bool opened = false;
};

FileSink::FileSink(OutputMode mode, Config config,
                   std::array<std::uint8_t, kBinaryHeaderSize> binary_header)
    : state_(std::make_unique<State>()), mode_(mode), config_(std::move(config)),
      header_(binary_header) {}
FileSink::~FileSink() = default;

bool FileSink::open(std::string *error) {
    if (state_->opened || (mode_ != OutputMode::Binary && mode_ != OutputMode::Text)) {
        if (error) *error = "invalid or already opened FileSink";
        return false;
    }
    detail::FileWriter::Config opts;
    opts.path = config_.path;
    opts.max_file_bytes = config_.max_file_bytes;
    opts.max_files = config_.max_files;
    opts.batch_bytes = config_.batch_bytes;
    opts.sync_every_records = config_.sync_every_records;
    opts.repair_on_start = config_.repair_on_start;
    opts.sync_on_rotate = config_.sync_on_rotate;
    state_->opened = state_->writer.open(opts, header_, mode_ == OutputMode::Binary, error);
    return state_->opened;
}
bool FileSink::append(const std::uint8_t *data, std::size_t size) noexcept {
    if (!state_->opened) return false;
    try { return state_->writer.append(data, size); } catch (...) { return false; }
}
bool FileSink::flush(bool durable) noexcept {
    if (!state_->opened) return false;
    try { return state_->writer.flush(durable); } catch (...) { return false; }
}
void FileSink::close() noexcept { state_->writer.close(); }
bool FileSink::has_pending() const noexcept { return state_->writer.has_pending(); }
bool FileSink::needs_sync() const noexcept { return state_->writer.needs_sync(); }
const ErrorInfo &FileSink::last_error() const noexcept { return state_->writer.last_error(); }
SinkStats FileSink::stats() const noexcept {
    return {state_->writer.rotations(), state_->writer.repaired_bytes(),
            state_->writer.sync_errors()};
}

namespace {
std::mutex &console_lock() { static std::mutex mutex; return mutex; }
}
bool ConsoleSink::append(const std::uint8_t *data, std::size_t size) noexcept {
    std::lock_guard<std::mutex> lock(console_lock());
    return std::fwrite(data, 1, size, stderr) == size;
}
bool ConsoleSink::flush(bool) noexcept {
    std::lock_guard<std::mutex> lock(console_lock());
    return std::fflush(stderr) == 0;
}
}  // namespace zxlog
